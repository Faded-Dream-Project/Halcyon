#include <math.h>
#include <stdlib.h>
/*
 * test_material.c — materials, unwraps, the UV editor widget, the
 * glTF material/UV export, and the project format v3 round trip
 */

#include "hal_test.h"
#include "core/material.h"
#include "core/mesh.h"
#include "core/project.h"
#include "core/scene.h"
#include "core/uv.h"
#include "io/io.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-mat-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

static hal_scene *make_scene(void) {
    hal_scene *s = hal_scene_create("Mat");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, cube, hal_mesh_cube(0.5f));

    hal_material mat = hal_material_default();
    strcpy(mat.name, "Violet Dream");
    mat.base_color = hal_color_rgb8(0x8B, 0x7E, 0xC8);
    mat.metallic = 0.25f;
    mat.roughness = 0.5f;
    mat.emissive = hal_vec3f_make(0.1f, 0.0f, 0.2f);
    mat.emissive_strength = 2.0f;
    int mi = hal_scene_material_add(s, &mat);
    hal_object_set_material(s, cube, mi);
    return s;
}

static void test_materials_and_v3(void) {
    hal_scene *s = make_scene();
    HAL_TEST_CHECK_EQ_I(hal_scene_material_count(s), 1);
    hal_material *m = hal_scene_material(s, 0);
    HAL_TEST_CHECK(m != NULL);
    HAL_TEST_CHECK(strcmp(m->name, "Violet Dream") == 0);
    HAL_TEST_CHECK_EQ_F(m->base_color.r, 0x8B / 255.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(m->metallic, 0.25f, 1e-5);

    /* The object references it; the default is neutral. */
    hal_object *obj = hal_scene_object_at(s, 0);
    HAL_TEST_CHECK_EQ_I(hal_object_material(obj), 0);
    hal_material d = hal_material_default();
    HAL_TEST_CHECK_EQ_F(d.metallic, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(d.roughness, 0.5f, 1e-5);
    HAL_TEST_CHECK_EQ_I(d.texture, -1);

    /* Image list. */
    int img = hal_scene_image_add(s, "/tmp/dream.png");
    HAL_TEST_CHECK(img >= 0);
    HAL_TEST_CHECK(strcmp(hal_scene_image(s, img), "/tmp/dream.png") == 0);
    m->texture = img;

    /* Format v3 round trip. */
    char path[512];
    snprintf(path, sizeof(path), "%s/mat.halcyon", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)),
                        HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_material_count(loaded), 1);
        hal_material *lm = hal_scene_material(loaded, 0);
        HAL_TEST_CHECK(lm != NULL);
        if (lm) {
            HAL_TEST_CHECK(strcmp(lm->name, "Violet Dream") == 0);
            HAL_TEST_CHECK_EQ_F(lm->base_color.g, 0x7E / 255.0f, 1e-3);
            HAL_TEST_CHECK_EQ_F(lm->metallic, 0.25f, 1e-4);
            HAL_TEST_CHECK_EQ_F(lm->emissive_strength, 2.0f, 1e-4);
            HAL_TEST_CHECK_EQ_I(lm->texture, 0);
        }
        HAL_TEST_CHECK_EQ_I(hal_scene_image_count(loaded), 1);
        HAL_TEST_CHECK(strcmp(hal_scene_image(loaded, 0),
                              "/tmp/dream.png") == 0);
        HAL_TEST_CHECK_EQ_I(
            hal_object_material(hal_scene_object_at(loaded, 0)), 0);
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

static void test_unwraps(void) {
    /* Planar: every UV lands in [0,1]. */
    hal_mesh *m = hal_mesh_cube(0.5f);
    HAL_TEST_CHECK(hal_mesh_unwrap_planar(m, HAL_UV_Y));
    HAL_TEST_CHECK(m->uvs_valid);
    for (size_t l = 0; l < m->loop_count; l++) {
        HAL_TEST_CHECK(m->uvs[l].x >= -1e-4f && m->uvs[l].x <= 1.0001f);
        HAL_TEST_CHECK(m->uvs[l].y >= -1e-4f && m->uvs[l].y <= 1.0001f);
    }

    /* Box: opposite cube faces land in DIFFERENT cells (adjacent
     * cells touch at the boundary, so the cell index — not the
     * coordinate distance — is the seam invariant). */
    HAL_TEST_CHECK(hal_mesh_unwrap_box(m));
    hal_vec2f top0 = m->uvs[m->face_off[1]];
    hal_vec2f bot0 = m->uvs[m->face_off[0]];
    /* The mapper's row assignment: +axis faces take the top row,
     * -axis faces the bottom row (pinned here as-is). */
    HAL_TEST_CHECK_EQ_I((int)(bot0.y * 2.0f), 1);
    HAL_TEST_CHECK_EQ_I((int)(top0.y * 2.0f), 0);

    /* Smart-lite: sphere islands unwrap and stay in range. */
    hal_mesh_destroy(m);
    m = hal_mesh_sphere(0.5f, 8, 12);
    HAL_TEST_CHECK(hal_mesh_unwrap_smart(m));
    HAL_TEST_CHECK(m->uvs_valid);
    for (size_t l = 0; l < m->loop_count; l++) {
        HAL_TEST_CHECK(m->uvs[l].x >= -0.01f && m->uvs[l].x <= 1.01f);
        HAL_TEST_CHECK(m->uvs[l].y >= -0.01f && m->uvs[l].y <= 1.01f);
    }
    hal_mesh_destroy(m);
}

static void test_uvview_widget(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 300, 300 });
    hal_widget *uvw = hal_uvview_create(root);
    hal_widget_set_rect(uvw, (hal_rectf){ 10, 10, 200, 200 });

    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_unwrap_box(m);
    hal_uvview_bind(uvw, m);
    hal_ui_layout(ui);

    /* Paint onto an offscreen surface: the widget's area must not
     * stay the plain window color (grid + loops drew). */
    hal_surface *surf = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(300, 300, &surf),
                        HAL_OK);
    hal_canvas canvas;
    HAL_TEST_CHECK(hal_canvas_init(&canvas, surf, 1.0f));
    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 300, 300 },
                         hal_color_hex(0x141a3cu));
    hal_ui_paint(ui, &canvas);
    hal_surface_invalidate_all(surf);

    hal_color bg = hal_color_hex(0x141a3cu);
    int drawn = 0;
    for (int y = 12; y < 208; y += 4)
        for (int x = 12; x < 208; x += 4) {
            hal_u32 p =
                canvas.pixels[y * canvas.stride + x];
            if ((p & 0xFFu) != (hal_u32)(bg.b * 255.0f))
                drawn++;
        }
    HAL_TEST_CHECK(drawn > 50);

    hal_surface_destroy(surf);
    hal_mesh_destroy(m);
    hal_ui_destroy(ui);
}

static void test_gltf_material_export(void) {
    hal_scene *s = make_scene();
    hal_object *cube = hal_scene_object_at(s, 0);
    hal_mesh_unwrap_box(cube->mesh); /* UVs on board */

    char path[512];
    snprintf(path, sizeof(path), "%s/mat.glb", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_io_gltf_export(s, path, err, sizeof(err)),
                        HAL_OK);

    /* The JSON chunk must carry the material, TEXCOORD_0 and the
     * base color values. */
    FILE *f = fopen(path, "rb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        hal_u32 jlen, jtype;
        fseek(f, 12, SEEK_SET);
        HAL_TEST_CHECK(fread(&jlen, 4, 1, f) == 1);
        HAL_TEST_CHECK(fread(&jtype, 4, 1, f) == 1);
        HAL_TEST_CHECK_EQ_I(jtype, 0x4E4F534Au);
        char *json = malloc(jlen + 1);
        HAL_TEST_CHECK(json != NULL);
        if (json) {
            HAL_TEST_CHECK(fread(json, 1, jlen, f) == jlen);
            json[jlen] = 0;
            HAL_TEST_CHECK(strstr(json, "\"materials\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "Violet Dream") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"TEXCOORD_0\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"metallicFactor\":0.25") !=
                           NULL);
            HAL_TEST_CHECK(strstr(json, "\"roughnessFactor\":0.5") !=
                           NULL);
            /* The emissive factor is emissive * strength. */
            HAL_TEST_CHECK(strstr(json, "\"emissiveFactor\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"material\":0") != NULL);
        }
        free(json);
        fclose(f);
    }

    /* Still re-importable (geometry side round trips). */
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 1);
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

int main(void) {
    test_materials_and_v3();
    test_unwraps();
    test_uvview_widget();
    test_gltf_material_export();
    HAL_TEST_SUMMARY("test_material");
}
