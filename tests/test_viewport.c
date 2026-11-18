/*
 * test_viewport.c — camera and rasterizer tests (headless: an
 * offscreen surface stands in for the window)
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/scene.h"
#include "render/camera.h"
#include "render/raster.h"
#include "render/render.h"

#include <math.h>
#include <string.h>

/* Builds a canvas over a fresh offscreen surface (helper). */
static bool make_canvas(hal_canvas *canvas, hal_surface **surf, int w,
                        int h) {
    if (!hal_ok(hal_surface_offscreen_create(w, h, surf)))
        return false;
    return hal_canvas_init(canvas, *surf, 1.0f);
}

static void test_camera_orbit_eye(void) {
    hal_camera cam = hal_camera_default();
    cam.target = hal_vec3f_zero();
    cam.distance = 10.0f;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;

    /* yaw=0, pitch=0: the eye sits on +Z looking at the origin. */
    hal_vec3f eye = hal_camera_eye(&cam);
    HAL_TEST_CHECK_EQ_F(eye.x, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(eye.y, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(eye.z, 10.0f, 1e-5);

    /* Pitch up 90 deg: the eye moves to +Y. */
    cam.pitch = (hal_f32)HAL_PI_f32 / 2.0f;
    eye = hal_camera_eye(&cam);
    HAL_TEST_CHECK_EQ_F(eye.y, 10.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(eye.x, 0.0f, 1e-4);

    /* The eye is always `distance` from the target. */
    cam.pitch = 0.7f;
    cam.yaw = 2.1f;
    eye = hal_camera_eye(&cam);
    HAL_TEST_CHECK_EQ_F(hal_vec3f_distance(eye, cam.target), 10.0f, 1e-4);
}

static void test_camera_orbit_clamps(void) {
    hal_camera cam = hal_camera_default();
    hal_camera_orbit(&cam, 0.0f, 5.0f);
    HAL_TEST_CHECK(cam.pitch <= 1.55f);
    hal_camera_orbit(&cam, 0.0f, -5.0f);
    HAL_TEST_CHECK(cam.pitch >= -1.55f);
    hal_camera_zoom(&cam, 0.000001f);
    HAL_TEST_CHECK(cam.distance >= 0.02f);
    hal_camera_zoom(&cam, 1000000.0f);
    HAL_TEST_CHECK(cam.distance <= 4000.0f);
}

static void test_camera_project_roundtrip(void) {
    hal_camera cam = hal_camera_default();
    hal_rectf viewport = { 0.0f, 0.0f, 800.0f, 600.0f };
    hal_mat4f view = hal_camera_view(&cam);
    hal_mat4f proj = hal_camera_projection(&cam, 800.0f / 600.0f);
    hal_mat4f vp = hal_mat4f_mul(proj, view);

    /* A world point in front of the camera projects inside the
     * viewport; the ray through that pixel returns the same point. */
    hal_vec3f world = hal_vec3f_make(0.3f, -0.2f, -1.0f);
    hal_f32 w = 0.0f;
    hal_vec3f screen = hal_camera_project(&cam, vp, world, viewport, &w);
    HAL_TEST_CHECK(w > 0.0f);
    HAL_TEST_CHECK(screen.x > 0.0f && screen.x < 800.0f);
    HAL_TEST_CHECK(screen.y > 0.0f && screen.y < 600.0f);

    hal_ray ray = hal_camera_ray(&cam, vp, viewport, screen.x, screen.y);
    /* The world point lies ON the ray. */
    hal_vec3f to_point = hal_vec3f_sub(world, ray.origin);
    hal_f32 t = hal_vec3f_dot(to_point, ray.dir);
    hal_vec3f closest = hal_vec3f_add(ray.origin,
                                      hal_vec3f_mul(ray.dir, t));
    HAL_TEST_CHECK(hal_vec3f_distance(closest, world) < 1e-3);
}

static void test_frustum_culling(void) {
    hal_camera cam = hal_camera_default();
    hal_mat4f view = hal_camera_view(&cam);
    hal_mat4f proj = hal_camera_projection(&cam, 16.0f / 9.0f);
    hal_frustum f = hal_frustum_from_matrix(hal_mat4f_mul(proj, view));

    /* The target is in view. */
    HAL_TEST_CHECK(hal_frustum_sphere_visible(&f, hal_vec3f_zero(), 0.5f));
    /* Far behind the camera: out. */
    HAL_TEST_CHECK(!hal_frustum_sphere_visible(
        &f, hal_vec3f_make(0.0f, 0.0f, cam.distance * 3.0f), 0.5f));
    /* NOTE: far-plane culling for the [0,1] NDC convention is not
     * yet verified (the plane extraction needs a dedicated test);
     * near/side culling is pinned below. */
    HAL_TEST_CHECK(hal_frustum_sphere_visible(
        &f, hal_vec3f_make(0.0f, 0.0f, -50.0f), 0.5f));
    /* Far off to the side: out. */
    HAL_TEST_CHECK(!hal_frustum_sphere_visible(
        &f, hal_vec3f_make(500.0f, 0.0f, 0.0f), 1.0f));
}

static void test_raster_depth(void) {
    hal_canvas canvas;
    hal_surface *surf = NULL;
    HAL_TEST_CHECK(make_canvas(&canvas, &surf, 100, 100));

    hal_raster r = { 0 };
    HAL_TEST_CHECK(hal_raster_begin(&r, &canvas,
                                    (hal_rectf){ 0, 0, 100, 100 }));
    hal_raster_clear(&r, hal_color_hex(0x000000u));

    /* A big background triangle at mid depth... */
    hal_vec3f far_a = { 10.0f, 10.0f, 0.6f };
    hal_vec3f far_b = { 90.0f, 10.0f, 0.6f };
    hal_vec3f far_c = { 10.0f, 90.0f, 0.6f };
    /* Clockwise in canvas space (y down) = front-facing here. */
    hal_raster_triangle(&r, far_a, 1.0f, far_c, 1.0f, far_b, 1.0f,
                        hal_color_hex(0xFF0000u));

    /* ...and a nearer triangle overlapping it. */
    hal_vec3f near_a = { 20.0f, 20.0f, 0.2f };
    hal_vec3f near_b = { 80.0f, 20.0f, 0.2f };
    hal_vec3f near_c = { 20.0f, 80.0f, 0.2f };
    hal_raster_triangle(&r, near_a, 1.0f, near_c, 1.0f, near_b, 1.0f,
                        hal_color_hex(0x00FF00u));

    /* Outside both: background. */
    hal_u32 bg = canvas.pixels[95 * canvas.stride + 95];
    HAL_TEST_CHECK_EQ_I(bg & 0xFFFFFFu, 0x000000u);
    /* Far triangle area only: red. */
    hal_u32 red = canvas.pixels[15 * canvas.stride + 15];
    HAL_TEST_CHECK_EQ_I(red & 0xFFFFFFu, 0xFF0000u);
    /* Overlap: the near (green) triangle wins by depth. */
    hal_u32 overlap = canvas.pixels[30 * canvas.stride + 30];
    HAL_TEST_CHECK_EQ_I(overlap & 0xFFFFFFu, 0x00FF00u);

    /* A triangle BEHIND the far one (larger z) is depth-rejected. */
    hal_vec3f back_a = { 15.0f, 15.0f, 0.9f };
    hal_vec3f back_b = { 85.0f, 15.0f, 0.9f };
    hal_vec3f back_c = { 15.0f, 85.0f, 0.9f };
    hal_raster_triangle(&r, back_a, 1.0f, back_c, 1.0f, back_b, 1.0f,
                        hal_color_hex(0x0000FFu));
    hal_u32 after = canvas.pixels[15 * canvas.stride + 15];
    HAL_TEST_CHECK_EQ_I(after & 0xFFFFFFu, 0xFF0000u);

    /* Back-face culling: the opposite winding never paints. */
    hal_raster_clear(&r, hal_color_hex(0x000000u));
    hal_raster_triangle(&r, far_a, 1.0f, far_b, 1.0f, far_c, 1.0f,
                        hal_color_hex(0xFFFFFFu)); /* CCW = back face */
    hal_u32 culled = canvas.pixels[20 * canvas.stride + 20];
    HAL_TEST_CHECK_EQ_I(culled & 0xFFFFFFu, 0x000000u);

    hal_raster_end(&r);
    hal_surface_destroy(surf);
}

static void test_raster_line_clips_depth_range(void) {
    hal_canvas canvas;
    hal_surface *surf = NULL;
    HAL_TEST_CHECK(make_canvas(&canvas, &surf, 60, 60));
    hal_raster r = { 0 };
    HAL_TEST_CHECK(hal_raster_begin(&r, &canvas, (hal_rectf){ 0, 0, 60, 60 }));
    hal_raster_clear(&r, hal_color_hex(0x000000u));

    /* A line inside the depth range paints... */
    hal_raster_line(&r, (hal_vec3f){ 5.0f, 5.0f, 0.5f },
                    (hal_vec3f){ 55.0f, 55.0f, 0.5f },
                    hal_color_hex(0xFFFFFFu));
    HAL_TEST_CHECK_EQ_I(canvas.pixels[30 * canvas.stride + 30] & 0xFFu,
                        0xFFu);

    /* ...one beyond z=1 does not. */
    hal_raster_line(&r, (hal_vec3f){ 5.0f, 5.0f, 2.0f },
                    (hal_vec3f){ 55.0f, 5.0f, 2.0f },
                    hal_color_hex(0xFFFFFFu));
    HAL_TEST_CHECK_EQ_I(canvas.pixels[5 * canvas.stride + 30] & 0xFFu,
                        0x00u);

    hal_raster_end(&r);
    hal_surface_destroy(surf);
}

static void test_render_view_scene(void) {
    hal_scene *scene = hal_scene_create("V");
    hal_object_id cube = hal_scene_object_add(scene, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(scene, cube, hal_mesh_cube(1.0f));

    hal_canvas canvas;
    hal_surface *surf = NULL;
    HAL_TEST_CHECK(make_canvas(&canvas, &surf, 300, 240));

    hal_camera cam = hal_camera_default(); /* looks at the origin */
    hal_render_opts opts = hal_render_opts_default();
    opts.draw_gizmo = false;

    hal_raster r = { 0 };
    hal_rectf vp = { 0, 0, 300, 240 };
    hal_render_view(&r, scene, &cam, &canvas, vp, &opts);

    /* The cube's silhouette covers the viewport center; the corners
     * (well outside the cube) stay background. */
    hal_u32 center = canvas.pixels[120 * canvas.stride + 150];
    hal_u32 corner = canvas.pixels[5 * canvas.stride + 5];
    HAL_TEST_CHECK(center != corner);
    /* The shaded face is a dimmed version of the object color, not
     * the background and not full-bright. */
    hal_u32 rgb = center & 0xFFFFFFu;
    HAL_TEST_CHECK(rgb != (opts.object.r > 0 ? 0xFFFFFFu : 0u));
    HAL_TEST_CHECK((rgb & 0xFFu) > 20); /* some blue in the lit face */

    /* Wireframe mode: the center pixel is either background or line —
     * but the cube's outline must produce SOME lit pixels on the
     * vertical center column. */
    hal_render_opts wire = opts;
    wire.shading = HAL_SHADING_WIREFRAME;
    hal_render_view(&r, scene, &cam, &canvas, vp, &wire);
    int lit = 0;
    for (int y = 0; y < 240; y++)
        if ((canvas.pixels[y * canvas.stride + 150] & 0xFFu) > 20)
            lit++;
    HAL_TEST_CHECK(lit >= 2);

    /* Culling: an object moved far off-screen paints nothing; move it
     * back and it paints again (center pixel differs from bg). */
    hal_object_set_location(scene, cube, hal_vec3f_make(500.0f, 0, 0));
    hal_render_view(&r, scene, &cam, &canvas, vp, &opts);
    hal_u32 empty = canvas.pixels[120 * canvas.stride + 150];
    HAL_TEST_CHECK_EQ_I(empty, (canvas.pixels[5 * canvas.stride + 5]));

    hal_object_set_location(scene, cube, hal_vec3f_make(0, 0, 0));
    hal_render_view(&r, scene, &cam, &canvas, vp, &opts);
    HAL_TEST_CHECK(canvas.pixels[120 * canvas.stride + 150] !=
                   canvas.pixels[5 * canvas.stride + 5]);

    hal_raster_end(&r);
    hal_surface_destroy(surf);
    hal_scene_destroy(scene);
}

int main(void) {
    test_camera_orbit_eye();
    test_camera_orbit_clamps();
    test_camera_project_roundtrip();
    test_frustum_culling();
    test_raster_depth();
    test_raster_line_clips_depth_range();
    test_render_view_scene();
    HAL_TEST_SUMMARY("test_viewport");
}
