/*
 * test_preview.c — preview renders: PNG contents, turntable frames,
 * thumbnails (headless through offscreen surfaces)
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/project.h"
#include "core/scene.h"
#include "render/camera.h"
#include "platform/platform.h"
#include "render/preview.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-prev-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* A PNG that exists, is non-empty and NOT uniformly its first pixel. */
static bool png_has_content(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 200)
        return false;
    /* Cheap content check via the platform: load the PNG back as a
     * surface and count distinct colors. */
    hal_surface *surf = NULL;
    bool ok = hal_ok(hal_surface_from_image(path, &surf));
    fclose(f);
    if (!ok || !surf)
        return false;
    hal_surface_info info;
    hal_surface_info_get(surf, &info);
    long distinct = 0;
    hal_u32 first = info.pixels ? info.pixels[0] : 0;
    for (hal_i32 y = 0; y < info.height; y += 2)
        for (hal_i32 x = 0; x < info.width; x += 2) {
            hal_u32 p =
                info.pixels[(size_t)y * (size_t)info.stride + (size_t)x];
            if (p != first)
                distinct++;
        }
    hal_surface_destroy(surf);
    return distinct > 50;
}

static hal_scene *make_scene(void) {
    hal_scene *s = hal_scene_create("P");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, cube, hal_mesh_cube(0.5f));
    hal_object_set_location(s, cube, hal_vec3f_make(0.5f, 0.2f, 0.0f));
    return s;
}

static void test_preview_and_thumbnail(void) {
    hal_scene *s = make_scene();
    char path[512];
    char err[256] = { 0 };

    snprintf(path, sizeof(path), "%s/preview.png", tmp_dir());
    hal_camera cam = hal_camera_frame_scene(s, hal_camera_default(),
                                            1.2f);
    HAL_TEST_CHECK_EQ_I(
        hal_preview_render(s, cam, 320, 240, true, path, err,
                           sizeof(err)),
        HAL_OK);
    HAL_TEST_CHECK(png_has_content(path));

    snprintf(path, sizeof(path), "%s/thumb.png", tmp_dir());
    HAL_TEST_CHECK_EQ_I(
        hal_preview_thumbnail(s, 128, path, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(png_has_content(path));

    /* Bad args refuse. */
    HAL_TEST_CHECK(hal_preview_render(s, cam, 0, 10, false, path, err,
                                      sizeof(err)) != HAL_OK);

    hal_scene_destroy(s);
}

static void test_turntable(void) {
    hal_scene *s = make_scene();
    char prefix[512];
    char err[256] = { 0 };
    snprintf(prefix, sizeof(prefix), "%s/turn", tmp_dir());

    HAL_TEST_CHECK_EQ_I(
        hal_preview_turntable(s, hal_camera_default(), 160, 120, 8,
                              prefix, err, sizeof(err)),
        HAL_OK);
    char path[600];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "%s_%03d.png", prefix, i);
        HAL_TEST_CHECK(png_has_content(path));
    }

    hal_scene_destroy(s);
}

int main(void) {
    test_preview_and_thumbnail();
    test_turntable();
    HAL_TEST_SUMMARY("test_preview");
}
