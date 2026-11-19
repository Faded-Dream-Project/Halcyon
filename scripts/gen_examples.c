/*
 * gen_examples.c — builds the shipped example projects (a tool, a
 * prop, an animated character) and verifies each one reloads.
 *
 * Build & run through scripts/gen-examples.sh (links Halcyon's
 * objects); outputs land in examples/.
 */

#include "core/anim.h"
#include "core/armature.h"
#include "core/material.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/project.h"
#include "core/scene.h"

#include <stdio.h>
#include <string.h>

static bool save_and_reload(hal_scene *s, const char *path) {
    char err[256] = { 0 };
    if (hal_project_save(s, path, err, sizeof(err)) != HAL_OK) {
        fprintf(stderr, "save %s: %s\n", path, err);
        return false;
    }
    hal_scene *loaded = NULL;
    if (hal_project_load(path, &loaded, err, sizeof(err)) != HAL_OK) {
        fprintf(stderr, "reload %s: %s\n", path, err);
        return false;
    }
    bool ok = hal_scene_object_count(loaded) ==
              hal_scene_object_count(s);
    if (ok) {
        for (size_t i = 0; i < hal_scene_object_count(loaded); i++) {
            hal_object *a = hal_scene_object_at(s, i);
            hal_object *b = hal_scene_object_at(loaded, i);
            if (strcmp(a->name, b->name) != 0)
                ok = false;
        }
    }
    hal_scene_destroy(loaded);
    return ok;
}

/* A simple tool: the sword composition (blade, guard, handle). */
static void build_tool(hal_scene *s) {
    hal_material steel = hal_material_default();
    strcpy(steel.name, "Steel");
    steel.base_color = hal_color_rgb8(0xC8, 0xCC, 0xD4);
    steel.metallic = 0.9f;
    steel.roughness = 0.2f;
    int mat_steel = hal_scene_material_add(s, &steel);

    hal_object_id blade =
        hal_scene_object_add(s, "Blade", HAL_OBJECT_MESH);
    hal_mesh *m = hal_mesh_create();
    hal_f32 w = 0.06f, t = 0.015f, len = 1.0f;
    hal_vec3f c[8] = {
        { -w, 0, -t }, { w, 0, -t }, { w, 0, t }, { -w, 0, t },
        { -w * 0.4f, len, -t * 0.4f }, { w * 0.4f, len, -t * 0.4f },
        { w * 0.4f, len, t * 0.4f }, { -w * 0.4f, len, t * 0.4f },
    };
    for (int i = 0; i < 8; i++)
        hal_mesh_vert_add(m, c[i]);
    hal_u32 q[6][4] = {
        { 0, 3, 7, 4 }, { 1, 5, 6, 2 }, { 0, 1, 2, 3 },
        { 4, 7, 6, 5 }, { 0, 4, 5, 1 }, { 3, 2, 6, 7 },
    };
    for (int i = 0; i < 6; i++)
        hal_mesh_face_add(m, q[i], 4);
    hal_mesh_rebuild_edges(m);
    hal_object_set_mesh(s, blade, m);
    hal_object_set_location(s, blade, hal_vec3f_make(0, 0.12f, 0));
    hal_object_set_material(s, blade, mat_steel);

    hal_object_id guard =
        hal_scene_object_add(s, "Guard", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, guard, hal_mesh_cube(0.16f));
    hal_object_set_location(s, guard, hal_vec3f_make(0, 0.08f, 0));
    hal_object_set_scale(s, guard, hal_vec3f_make(3.2f, 0.25f, 0.6f));
    hal_object_set_material(s, guard, mat_steel);

    hal_object_id handle =
        hal_scene_object_add(s, "Handle", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, handle,
                        hal_mesh_cylinder(0.025f, 0.24f, 8));
    hal_object_set_location(s, handle, hal_vec3f_make(0, -0.12f, 0));
}

/* A prop: a crate on a barrel, both with materials. */
static void build_prop(hal_scene *s) {
    hal_material wood = hal_material_default();
    strcpy(wood.name, "Wood");
    wood.base_color = hal_color_rgb8(0x9C, 0x6B, 0x3F);
    wood.roughness = 0.85f;
    int mwood = hal_scene_material_add(s, &wood);

    hal_material steel = hal_material_default();
    strcpy(steel.name, "BarrelSteel");
    steel.base_color = hal_color_rgb8(0x5E, 0x66, 0x6E);
    steel.metallic = 0.7f;
    steel.roughness = 0.35f;
    int msteel = hal_scene_material_add(s, &steel);

    hal_object_id barrel =
        hal_scene_object_add(s, "Barrel", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, barrel, hal_mesh_cylinder(0.45f, 1.2f, 16));
    hal_object_set_location(s, barrel, hal_vec3f_make(0, 0.6f, 0));
    hal_object_set_material(s, barrel, msteel);

    hal_object_id crate =
        hal_scene_object_add(s, "Crate", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, crate, hal_mesh_cube(0.5f));
    hal_object_set_location(s, crate, hal_vec3f_make(0, 1.55f, 0));
    hal_object_set_rotation_euler(
        s, crate, hal_vec3f_make(0.0f, 0.5f, 0.0f));
    hal_object_set_material(s, crate, mwood);
}

/* An animated character: a two-bone rig with a bound, keyed cube
 * "limb" waving. */
static void build_character(hal_scene *s) {
    hal_object_id arm_id =
        hal_scene_object_add(s, "Rig", HAL_OBJECT_ARMATURE);
    hal_armature *arm = hal_armature_create();
    hal_armature_bone_add(arm, "Upper", hal_vec3f_zero(),
                          hal_vec3f_make(0, 1, 0), -1);
    hal_armature_bone_add(arm, "Lower", hal_vec3f_make(0, 1, 0),
                          hal_vec3f_make(0, 2, 0), 0);
    hal_object_set_armature(s, arm_id, arm);

    hal_object_id mesh_id =
        hal_scene_object_add(s, "Limb", HAL_OBJECT_MESH);
    hal_mesh *mesh = hal_mesh_cube(0.5f);
    hal_mesh_bind_armature(mesh, arm);
    hal_object_set_mesh(s, mesh_id, mesh);
    hal_object_set_skin(s, mesh_id, arm_id);

    hal_material skin = hal_material_default();
    strcpy(skin.name, "Skin");
    skin.base_color = hal_color_rgb8(0xC9, 0x9A, 0x7E);
    int mskin = hal_scene_material_add(s, &skin);
    hal_object_set_material(s, mesh_id, mskin);

    int ci = hal_scene_clip_add(s, "Wave");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 2.0f;
    clip->loop = true;
    hal_quatf q90 = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_clip_key_rot(clip, arm_id, 0, 0.0f, hal_quatf_identity(),
                     HAL_INTERP_SMOOTH);
    hal_clip_key_rot(clip, arm_id, 0, 1.0f, q90, HAL_INTERP_SMOOTH);
    hal_clip_key_rot(clip, arm_id, 0, 2.0f, hal_quatf_identity(),
                     HAL_INTERP_SMOOTH);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gen_examples <examples-dir>\n");
        return 1;
    }
    const char *dir = argv[1];
    char path[512];
    struct {
        const char *file;
        void (*build)(hal_scene *);
        const char *what;
    } examples[3] = {
        { "tool-sword.halcyon", build_tool,
          "a simple tool (sword)" },
        { "prop-crate.halcyon", build_prop,
          "a prop (crate on a barrel)" },
        { "character-wave.halcyon", build_character,
          "an animated character (rigged, skinned, waving)" },
    };
    int failures = 0;
    for (int i = 0; i < 3; i++) {
        hal_scene *s = hal_scene_create("Example");
        examples[i].build(s);
        snprintf(path, sizeof(path), "%s/%s", dir, examples[i].file);
        if (save_and_reload(s, path))
            printf("wrote %s (%s), reload verified\n", path,
                   examples[i].what);
        else
            failures++;
        hal_scene_destroy(s);
    }
    return failures == 0 ? 0 : 1;
}
