/*
 * test_rig.c — armatures: pose math, skinning, auto weights, IK
 */

#include "hal_test.h"
#include "core/armature.h"
#include "core/mesh_ops.h"
#include "core/mesh.h"
#include "core/project.h"
#include "core/scene.h"
#include "io/io.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-rig-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* A 2-bone chain up the Y axis: (0,0,0)-(0,1,0)-(0,2,0). */
static hal_armature *chain2(void) {
    hal_armature *arm = hal_armature_create();
    hal_armature_bone_add(arm, "Upper", hal_vec3f_zero(),
                          hal_vec3f_make(0, 1, 0), -1);
    hal_armature_bone_add(arm, "Lower", hal_vec3f_make(0, 1, 0),
                          hal_vec3f_make(0, 2, 0), 0);
    return arm;
}

static void test_pose_chain(void) {
    hal_armature *arm = chain2();

    /* Rest: identity pose keeps heads and tails in place. */
    hal_bone_pose pose[2];
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.y, 1.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(pose[1].tail.y, 2.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(pose[1].head.y, 1.0f, 1e-5);

    /* Rotating the parent 90 deg about Z swings the child: the tip
     * moves from (0,2,0) to (-2,1,0)... rotate (0,1,0) by 90deg
     * about Z gives (-1,0,0); the upper tail goes to (-1,0,0), and
     * the child (whose rest offsets ride the parent's matrix)
     * follows: the child head lands at the parent's posed tail, and
     * the tip ends at (-2, 1... let me just verify the tip follows
     * the rotation: |tip| stays 2 and it lies off the Y axis. */
    arm->bones[0].pose = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.x, -1.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.y, 0.0f, 1e-4);
    /* The child head must sit at the parent's posed tail. */
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].head, pose[0].tail) <
                   1e-4);
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].tail,
                                      hal_vec3f_make(-2.0f, 0.0f, 0.0f)) <
                   1e-4);
    hal_armature_destroy(arm);
}

static void test_auto_weights(void) {
    hal_armature *arm = chain2();
    hal_mesh *m = hal_mesh_plane(1.0f);

    HAL_TEST_CHECK(hal_mesh_bind_armature(m, arm));
    HAL_TEST_CHECK(m->skin_weights != NULL && m->skin_bones != NULL);
    for (size_t v = 0; v < m->vert_count; v++) {
        hal_f32 sum = 0.0f;
        int used = 0;
        for (size_t i = 0; i < 4; i++) {
            if (m->skin_bones[v * 4 + i] >= 0) {
                sum += m->skin_weights[v * 4 + i];
                used++;
            }
        }
        HAL_TEST_CHECK(used >= 1 && used <= 2);
        HAL_TEST_CHECK_EQ_F(sum, 1.0f, 1e-4);
    }
    hal_mesh_destroy(m);
    hal_armature_destroy(arm);
}

static void test_skinning(void) {
    hal_armature *arm = chain2();
    hal_mesh *m = hal_mesh_plane(1.0f);
    hal_mesh_bind_armature(m, arm);

    /* Unposed skin leaves verts in place. */
    hal_vec3f out[4];
    HAL_TEST_CHECK(hal_mesh_apply_pose(m, arm, out));
    for (size_t v = 0; v < m->vert_count; v++)
        HAL_TEST_CHECK(hal_vec3f_distance(out[v], m->verts[v]) < 1e-5);

    /* Posing the parent moves verts (the plane rides the chain).
     * REGRESSION (found by the glTF skin round trip): with only bone
     * 0 posed, every vertex maps through posed_0 regardless of
     * weights (posed_1 * rest_1^-1 == posed_0 when pose_1 is
     * identity), and the REST matrices must stay unposed — so the
     * result is EXACTLY R90 * p for a same-space binding. The old
     * rest-pose leaked the posed parent rotation into the binds. */
    hal_quatf q90 = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    arm->bones[0].pose = q90;
    HAL_TEST_CHECK(hal_mesh_apply_pose(m, arm, out));
    for (size_t v = 0; v < m->vert_count; v++) {
        hal_vec3f want =
            hal_quatf_rotate_vec3(q90, m->verts[v]);
        HAL_TEST_CHECK(hal_vec3f_distance(out[v], want) < 1e-4);
    }

    /* A far-away mesh with weights still sums to a valid position
     * (no NaNs). */
    for (size_t v = 0; v < m->vert_count; v++) {
        HAL_TEST_CHECK(isfinite(out[v].x) && isfinite(out[v].y) &&
                       isfinite(out[v].z));
    }
    hal_mesh_destroy(m);
    hal_armature_destroy(arm);
}

static void test_ik(void) {
    hal_armature *arm = chain2();

    /* Reach to (1, 1, 0): within the 2-unit reach. */
    HAL_TEST_CHECK(
        hal_armature_ik_2bone(arm, 1, hal_vec3f_make(1.0f, 1.0f, 0.0f)));
    hal_bone_pose pose[2];
    hal_armature_pose(arm, pose);
    /* The tip must land on (or very near) the target. */
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].tail,
                                      hal_vec3f_make(1.0f, 1.0f, 0.0f)) <
                   1e-3);

    /* Out-of-reach targets clamp to the maximum reach: the tip lands
     * on the sphere of radius l1 + l2 = 2 in the target direction. */
    hal_armature_destroy(arm);
    arm = chain2();
    HAL_TEST_CHECK(hal_armature_ik_2bone(arm, 1,
                                         hal_vec3f_make(10.0f, 0.0f,
                                                        0.0f)));
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(hal_vec3f_length(pose[1].tail), 2.0f, 1e-3);
    HAL_TEST_CHECK(hal_vec3f_distance(
                       pose[1].tail,
                       hal_vec3f_make(2.0f, 0.0f, 0.0f)) < 1e-3);

    hal_armature_destroy(arm);
}

static void test_project_armature_round_trip(void) {
    hal_scene *s = hal_scene_create("Rig");
    hal_object_id arm_id =
        hal_scene_object_add(s, "Rig", HAL_OBJECT_ARMATURE);
    hal_object_set_armature(s, arm_id, chain2());

    hal_object_id mesh_id =
        hal_scene_object_add(s, "Limb", HAL_OBJECT_MESH);
    hal_mesh *m = hal_mesh_cube(0.5f);
    /* REGRESSION: actually bind weights before saving — the weights
     * block used to be written BEFORE the mesh block, so reloading
     * failed ('weights outside a mesh object'). Caught by the
     * character example project. */
    hal_mesh_bind_armature(m, hal_scene_object(s, arm_id)->armature);
    hal_object_set_mesh(s, mesh_id, m);
    hal_object_set_skin(s, mesh_id, arm_id);

    char path[512];
    snprintf(path, sizeof(path), "%s/rig.halcyon", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)),
                        HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 2);
        hal_object *lo = hal_scene_object_at(loaded, 0);
        HAL_TEST_CHECK(lo && lo->armature);
        if (lo && lo->armature) {
            HAL_TEST_CHECK_EQ_I(lo->armature->count, 2);
            HAL_TEST_CHECK_EQ_I(lo->armature->bones[1].parent, 0);
            HAL_TEST_CHECK_EQ_F(lo->armature->bones[1].head.y, 1.0f,
                                1e-5);
        }
        hal_object *lm = hal_scene_object_at(loaded, 1);
        HAL_TEST_CHECK(lm && lm->skin_armature == lo->id);
        HAL_TEST_CHECK(lm && lm->mesh && lm->mesh->skin_weights !=
                                        NULL);
        if (lm && lm->mesh && lm->mesh->skin_weights) {
            hal_f32 sum = 0.0f;
            for (int i = 0; i < 4; i++)
                sum += lm->mesh->skin_weights[i];
            HAL_TEST_CHECK_EQ_F(sum, 1.0f, 1e-4);
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

/* The glTF skin round trip: a rigged mesh at a DIFFERENT transform
 * than its armature (the hard case), a bone-rotation clip, export,
 * import, and a numeric pose-equivalence check — the world-space
 * skinned vertices must match between the original and the imported
 * scene, which proves the exported G * IBM skinning semantics equal
 * Halcyon's in-app math. */
static void test_gltf_skin_round_trip(void) {
    hal_scene *s = hal_scene_create("SkinRT");
    hal_object_id arm_id =
        hal_scene_object_add(s, "Rig", HAL_OBJECT_ARMATURE);
    hal_object_set_location(s, arm_id, hal_vec3f_make(1.0f, 0.0f, 0.0f));
    hal_armature *arm = chain2();
    hal_object_set_armature(s, arm_id, arm);

    hal_object_id mesh_id =
        hal_scene_object_add(s, "Limb", HAL_OBJECT_MESH);
    hal_object_set_location(s, mesh_id,
                            hal_vec3f_make(0.5f, 0.2f, 0.0f));
    hal_mesh *mesh = hal_mesh_cube(0.5f);
    hal_mesh_bind_armature(mesh, arm);
    hal_object_set_mesh(s, mesh_id, mesh);
    hal_object_set_skin(s, mesh_id, arm_id);

    /* A bone-rotation clip: 0 -> 90 degrees about Z over 1 s. */
    int ci = hal_scene_clip_add(s, "Wave");
    hal_clip *clip = hal_scene_clip(s, ci);
    hal_quatf q90 = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_clip_key_rot(clip, arm_id, 0, 0.0f, hal_quatf_identity(),
                     HAL_INTERP_LINEAR);
    hal_clip_key_rot(clip, arm_id, 0, 1.0f, q90, HAL_INTERP_LINEAR);

    char path[512];
    snprintf(path, sizeof(path), "%s/skin.glb", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_io_gltf_export(s, path, err, sizeof(err)),
                        HAL_OK);

    /* Structural: the JSON carries the skin machinery. */
    FILE *f = fopen(path, "rb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        hal_u32 jlen, jtype;
        fseek(f, 12, SEEK_SET);
        HAL_TEST_CHECK(fread(&jlen, 4, 1, f) == 1);
        HAL_TEST_CHECK(fread(&jtype, 4, 1, f) == 1);
        char *json = malloc(jlen + 1);
        HAL_TEST_CHECK(json != NULL);
        if (json) {
            HAL_TEST_CHECK(fread(json, 1, jlen, f) == jlen);
            json[jlen] = 0;
            HAL_TEST_CHECK(strstr(json, "\"skins\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"joints\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"inverseBindMatrices\"") !=
                           NULL);
            HAL_TEST_CHECK(strstr(json, "\"JOINTS_0\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"WEIGHTS_0\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"skeleton\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"path\":\"rotation\"") !=
                           NULL);
        }
        free(json);
        fclose(f);
    }

    /* Import. */
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        /* The armature and the mesh both came back. */
        hal_object *lo_arm = NULL, *lo_mesh = NULL;
        for (size_t i = 0; i < hal_scene_object_count(loaded); i++) {
            hal_object *o = hal_scene_object_at(loaded, i);
            if (o->armature)
                lo_arm = o;
            if (o->mesh && o->skin_armature != HAL_OBJECT_ID_NONE)
                lo_mesh = o;
        }
        HAL_TEST_CHECK(lo_arm != NULL);
        HAL_TEST_CHECK(lo_mesh != NULL);
        if (lo_arm && lo_mesh && lo_arm->armature) {
            HAL_TEST_CHECK_EQ_I(lo_arm->armature->count, 2);
            /* Bones import with ARMATURE-LOCAL heads and the
             * armature object carries A (location 1,0,0). */
            HAL_TEST_CHECK_EQ_F(lo_arm->location.x, 1.0f, 1e-4);
            HAL_TEST_CHECK_EQ_F(lo_arm->armature->bones[0].head.x,
                                0.0f, 1e-4);
            HAL_TEST_CHECK_EQ_F(lo_arm->armature->bones[1].head.x,
                                0.0f, 1e-4);
            HAL_TEST_CHECK_EQ_F(lo_arm->armature->bones[1].head.y,
                                1.0f, 1e-4);
            HAL_TEST_CHECK(lo_mesh->mesh->skin_weights != NULL);
            HAL_TEST_CHECK(lo_mesh->skin_armature == lo_arm->id);

            /* THE equivalence check: same bone pose on both, world
             * skinned vertices must agree. */
            hal_quatf pose = hal_quatf_from_axis_angle(
                hal_vec3f_axis_z(), 60.0f * HAL_DEG2RAD_f32);
            arm->bones[0].pose = pose;
            lo_arm->armature->bones[0].pose = pose;

            /* The exported mesh duplicates verts per corner (36 for
             * the flat-shaded cube), so compare SETS: every imported
             * corner's world position must match some original
             * vert's world position, and vice versa. */
            hal_vec3f *out_orig = malloc(mesh->vert_count *
                                         sizeof(hal_vec3f));
            hal_vec3f *out_load = malloc(lo_mesh->mesh->vert_count *
                                         sizeof(hal_vec3f));
            HAL_TEST_CHECK(out_orig && out_load);
            hal_mat4f m2a_orig = hal_mat4f_mul(
                hal_mat4f_invert(
                    hal_object_world_matrix(s,
                                            hal_scene_object(s, arm_id))),
                hal_object_world_matrix(s,
                                        hal_scene_object(s, mesh_id)));
            HAL_TEST_CHECK(hal_mesh_apply_pose_ex(mesh, arm, m2a_orig,
                                                  out_orig));
            /* Imported: the armature object carries A, so
             * mesh->armature is A'^-1 * Wm' as in the original. */
            hal_mat4f m2a_load = hal_mat4f_mul(
                hal_mat4f_invert(
                    hal_object_world_matrix(loaded, lo_arm)),
                hal_object_world_matrix(loaded, lo_mesh));
            HAL_TEST_CHECK(hal_mesh_apply_pose_ex(
                lo_mesh->mesh, lo_arm->armature, m2a_load, out_load));

            hal_mat4f wm_orig =
                hal_object_world_matrix(s, hal_scene_object(s, mesh_id));
            hal_mat4f wm_load =
                hal_object_world_matrix(loaded, lo_mesh);
            if (out_orig && out_load) {
                /* Original -> imported (every original vert appears
                 * among the corners). */
                for (size_t v = 0; v < mesh->vert_count; v++) {
                    hal_vec3f w1 = hal_mat4f_transform_point(
                        wm_orig, out_orig[v]);
                    bool found = false;
                    for (size_t c = 0;
                         c < lo_mesh->mesh->vert_count && !found; c++) {
                        hal_vec3f w2 = hal_mat4f_transform_point(
                            wm_load, out_load[c]);
                        if (hal_vec3f_distance(w1, w2) < 1e-4)
                            found = true;
                    }
                    if (!found) {
                        HAL_TEST_CHECK(false);
                        break;
                    }
                }
                /* Imported -> original. */
                for (size_t c = 0; c < lo_mesh->mesh->vert_count; c++) {
                    hal_vec3f w2 = hal_mat4f_transform_point(
                        wm_load, out_load[c]);
                    bool found = false;
                    for (size_t v = 0;
                         v < mesh->vert_count && !found; v++) {
                        hal_vec3f w1 = hal_mat4f_transform_point(
                            wm_orig, out_orig[v]);
                        if (hal_vec3f_distance(w1, w2) < 1e-4)
                            found = true;
                    }
                    if (!found) {
                        HAL_TEST_CHECK(false);
                        break;
                    }
                }
            }
            free(out_orig);
            free(out_load);

            /* The clip survived: sampling it at t = 1 poses the
             * imported bone at 90 degrees. */
            HAL_TEST_CHECK_EQ_I(hal_scene_clip_count(loaded), 1);
            hal_clip *lclip = hal_scene_clip(loaded, 0);
            HAL_TEST_CHECK(lclip != NULL);
            if (lclip) {
                hal_clip_sample(lclip, loaded, 1.0f);
                hal_bone_pose lp[2];
                hal_armature_pose(lo_arm->armature, lp);
                /* 90 degrees about Z swings the imported stub tail
                 * (length 0.1) off +Y: head (0,0,0) + R90*(0,0.1,0)
                 * = (-0.1, 0, 0). */
                HAL_TEST_CHECK_EQ_F(lp[0].tail.x, -0.1f, 1e-2);
                HAL_TEST_CHECK_EQ_F(lp[0].tail.y, 0.0f, 1e-3);
            }
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

/* Weight painting: add/subtract with falloff, renormalization, and
 * free-slot acquisition. */
static void test_weight_paint(void) {
    hal_armature *arm = chain2();
    hal_mesh *m = hal_mesh_plane(1.0f);
    hal_mesh_bind_armature(m, arm);
    hal_mat4f world = hal_mat4f_identity();

    /* Paint bone 1 strongly at the plane's +X edge: verts near
     * (1,0,0) gain bone-1 weight, all sums stay 1. */
    HAL_TEST_CHECK(hal_mesh_paint_weight(
        m, world, hal_vec3f_make(1.0f, 0.0f, 0.0f), 1.2f, 1, 0.9f));
    bool any_bone1 = false;
    for (size_t v = 0; v < m->vert_count; v++) {
        hal_f32 sum = 0.0f;
        for (int i = 0; i < 4; i++) {
            size_t wi = v * 4 + (size_t)i;
            if (m->skin_bones[wi] < 0)
                continue;
            sum += m->skin_weights[wi];
            if (m->skin_bones[wi] == 1 && m->skin_weights[wi] > 0.1f)
                any_bone1 = true;
        }
        HAL_TEST_CHECK_EQ_F(sum, 1.0f, 1e-4);
    }
    HAL_TEST_CHECK(any_bone1);

    /* Subtract the same region: bone 1's weight falls back. */
    hal_f32 before = 0.0f, after = 0.0f;
    for (size_t v = 0; v < m->vert_count; v++)
        for (int i = 0; i < 4; i++) {
            size_t wi = v * 4 + (size_t)i;
            if (m->skin_bones[wi] == 1)
                before += m->skin_weights[wi];
        }
    HAL_TEST_CHECK(hal_mesh_paint_weight(
        m, world, hal_vec3f_make(1.0f, 0.0f, 0.0f), 1.2f, 1, -0.9f));
    for (size_t v = 0; v < m->vert_count; v++)
        for (int i = 0; i < 4; i++) {
            size_t wi = v * 4 + (size_t)i;
            if (m->skin_bones[wi] == 1)
                after += m->skin_weights[wi];
        }
    HAL_TEST_CHECK(after < before);

    /* A far point changes nothing. */
    HAL_TEST_CHECK(!hal_mesh_paint_weight(
        m, world, hal_vec3f_make(50.0f, 0.0f, 0.0f), 0.5f, 1, 0.9f));

    hal_mesh_destroy(m);
    hal_armature_destroy(arm);
}

int main(void) {
    test_pose_chain();
    test_auto_weights();
    test_skinning();
    test_ik();
    test_project_armature_round_trip();
    test_gltf_skin_round_trip();
    test_weight_paint();
    HAL_TEST_SUMMARY("test_rig");
}
