/*
 * preview.c — preview renders through the viewport rasterizer
 */

#include "preview.h"
#include "raster.h"
#include "render.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void prev_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

hal_camera hal_camera_frame_scene(hal_scene *scene, hal_camera cam,
                                  hal_f32 margin) {
    if (!scene || scene->count == 0)
        return cam;

    hal_vec3f bmin = hal_vec3f_make(1e30f, 1e30f, 1e30f);
    hal_vec3f bmax = hal_vec3f_make(-1e30f, -1e30f, -1e30f);
    bool any = false;
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->visible || !obj->mesh)
            continue;
        hal_mat4f world = hal_object_world_matrix(scene, obj);
        hal_mesh_bounds(obj->mesh, &obj->mesh->bbox_min,
                        &obj->mesh->bbox_max);
        for (int c = 0; c < 8; c++) {
            hal_vec3f p = hal_vec3f_make(
                c & 1 ? obj->mesh->bbox_max.x : obj->mesh->bbox_min.x,
                c & 2 ? obj->mesh->bbox_max.y : obj->mesh->bbox_min.y,
                c & 4 ? obj->mesh->bbox_max.z : obj->mesh->bbox_min.z);
            p = hal_mat4f_transform_point(world, p);
            bmin.x = fminf(bmin.x, p.x);
            bmin.y = fminf(bmin.y, p.y);
            bmin.z = fminf(bmin.z, p.z);
            bmax.x = fmaxf(bmax.x, p.x);
            bmax.y = fmaxf(bmax.y, p.y);
            bmax.z = fmaxf(bmax.z, p.z);
            any = true;
        }
    }
    if (!any)
        return cam;

    cam.target = hal_vec3f_mul(hal_vec3f_add(bmin, bmax), 0.5f);
    hal_vec3f size = hal_vec3f_sub(bmax, bmin);
    hal_f32 radius = 0.5f * fmaxf(size.x, fmaxf(size.y, size.z));
    if (radius < 1e-3f)
        radius = 1.0f;
    /* Fit the sphere in the FOV (tan of half-angle), with margin. */
    hal_f32 fit = radius /
                  (hal_f32)__builtin_tan((double)cam.fov_y * 0.5);
    cam.distance = fit * (margin > 1.0f ? margin : 1.2f);
    return cam;
}

/* Renders once into a fresh offscreen surface and saves the PNG. */
static hal_result render_once(hal_scene *scene, hal_camera cam, int w,
                              int h, bool grid, const char *path,
                              char *err, size_t err_cap) {
    hal_surface *surf = NULL;
    if (!hal_ok(hal_surface_offscreen_create(w, h, &surf))) {
        prev_err(err, err_cap, "cannot create a %dx%d surface", w, h);
        return HAL_ERR_SURFACE_CREATE;
    }
    hal_canvas canvas;
    if (!hal_canvas_init(&canvas, surf, 1.0f)) {
        hal_surface_destroy(surf);
        prev_err(err, err_cap, "cannot map the surface");
        return HAL_ERR_SURFACE_CREATE;
    }

    hal_render_opts opts = hal_render_opts_default();
    opts.draw_grid = grid;
    opts.draw_gizmo = false;
    hal_raster raster = { 0 };
    hal_rectf vp = { 0, 0, (hal_f32)w, (hal_f32)h };
    hal_render_view(&raster, scene, &cam, &canvas, vp, &opts);

    if (!hal_ok(hal_surface_save_png(surf, path))) {
        hal_raster_end(&raster);
        hal_surface_destroy(surf);
        prev_err(err, err_cap, "cannot write %s", path);
        return HAL_ERR_IO;
    }
    hal_raster_end(&raster);
    hal_surface_destroy(surf);
    return HAL_OK;
}

hal_result hal_preview_render(hal_scene *scene, hal_camera cam, int w,
                              int h, bool grid, const char *path,
                              char *err, size_t err_cap) {
    if (!scene || !path || w <= 0 || h <= 0) {
        prev_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    return render_once(scene, cam, w, h, grid, path, err, err_cap);
}

hal_result hal_preview_turntable(hal_scene *scene, hal_camera cam,
                                 int w, int h, int frames,
                                 const char *prefix, char *err,
                                 size_t err_cap) {
    if (!scene || !prefix || frames <= 0 || frames > 10000) {
        prev_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    char path[1024];
    for (int i = 0; i < frames; i++) {
        hal_camera frame_cam = cam;
        hal_camera_orbit(
            &frame_cam,
            (hal_f32)i / (hal_f32)frames * (hal_f32)HAL_TAU_f32, 0.0f);
        int n = snprintf(path, sizeof(path), "%s_%03d.png", prefix, i);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            prev_err(err, err_cap, "prefix too long");
            return HAL_ERR_INVALID_ARGUMENT;
        }
        hal_result r =
            render_once(scene, frame_cam, w, h, false, path, err,
                        err_cap);
        if (r != HAL_OK)
            return r;
    }
    return HAL_OK;
}

hal_result hal_preview_thumbnail(hal_scene *scene, int size,
                                 const char *path, char *err,
                                 size_t err_cap) {
    if (!scene || !path || size <= 0 || size > 4096) {
        prev_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    hal_camera cam = hal_camera_default();
    cam = hal_camera_frame_scene(scene, cam, 1.25f);
    return render_once(scene, cam, size, size, false, path, err,
                       err_cap);
}
