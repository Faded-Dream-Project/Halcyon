/*
 * camera.c — orbit camera math, projection, rays, frustum culling
 */

#include "camera.h"

#include <math.h>

hal_camera hal_camera_default(void) {
    hal_camera cam = {
        .target = hal_vec3f_zero(),
        .distance = 4.6f,
        .yaw = 0.785398f,  /* 45 degrees: the classic 3/4 view */
        .pitch = 0.55f,
        .fov_y = 50.0f * HAL_DEG2RAD_f32,
        .near_z = 0.05f,
        .far_z = 500.0f,
    };
    return cam;
}

hal_vec3f hal_camera_eye(const hal_camera *cam) {
    /* Pitch lifts the eye above the horizon; yaw swings it around Y.
     * yaw = 0, pitch = 0 looks along -Z from +Z. */
    hal_f32 cp = (hal_f32)__builtin_cos((double)cam->pitch);
    hal_vec3f dir = hal_vec3f_make(
        cp * (hal_f32)__builtin_sin((double)cam->yaw),
        (hal_f32)__builtin_sin((double)cam->pitch),
        cp * (hal_f32)__builtin_cos((double)cam->yaw));
    return hal_vec3f_add(cam->target, hal_vec3f_mul(dir, cam->distance));
}

hal_mat4f hal_camera_view(const hal_camera *cam) {
    return hal_mat4f_look_at(hal_camera_eye(cam), cam->target,
                             hal_vec3f_axis_y());
}

hal_mat4f hal_camera_projection(const hal_camera *cam, hal_f32 aspect) {
    return hal_mat4f_perspective(cam->fov_y, aspect, cam->near_z,
                                 cam->far_z);
}

void hal_camera_orbit(hal_camera *cam, hal_f32 dx_radians,
                      hal_f32 dy_radians) {
    if (!cam)
        return;
    cam->yaw += dx_radians;
    cam->pitch += dy_radians;
    /* Clamp pitch just short of the poles so the up vector never
     * degenerates. */
    if (cam->pitch > 1.55f)
        cam->pitch = 1.55f;
    if (cam->pitch < -1.55f)
        cam->pitch = -1.55f;
}

void hal_camera_pan(hal_camera *cam, hal_f32 dx, hal_f32 dy) {
    if (!cam)
        return;
    hal_mat4f view = hal_camera_view(cam);
    /* The view matrix's right/up are its first rows (row-major
     * reading of the column-major storage); move the target opposite
     * to the drag so content follows the pointer. */
    hal_vec3f right = hal_vec3f_make(view.m[0], view.m[4], view.m[8]);
    hal_vec3f up = hal_vec3f_make(view.m[1], view.m[5], view.m[9]);
    cam->target = hal_vec3f_sub(cam->target,
                                hal_vec3f_add(hal_vec3f_mul(right, dx),
                                              hal_vec3f_mul(up, dy)));
}

void hal_camera_zoom(hal_camera *cam, hal_f32 factor) {
    if (!cam || factor <= 0.0f)
        return;
    cam->distance *= factor;
    if (cam->distance < 0.02f)
        cam->distance = 0.02f;
    if (cam->distance > 4000.0f)
        cam->distance = 4000.0f;
}

hal_vec3f hal_camera_project(const hal_camera *cam, hal_mat4f view_proj,
                             hal_vec3f world, hal_rectf viewport,
                             hal_f32 *out_w) {
    (void)cam;
    hal_vec4f clip =
        hal_mat4f_mul_vec4(view_proj, hal_vec4f_make(world.x, world.y,
                                                     world.z, 1.0f));
    if (out_w)
        *out_w = clip.w;
    if (clip.w > 1e-6f) {
        hal_f32 nx = clip.x / clip.w;
        hal_f32 ny = clip.y / clip.w;
        return hal_vec3f_make(
            viewport.x + (nx * 0.5f + 0.5f) * viewport.width,
            viewport.y + (0.5f - ny * 0.5f) * viewport.height,
            clip.z / clip.w);
    }
    /* Behind the eye: mirror onto the plane w = eps so callers can
     * still interpolate toward a visible partner. */
    hal_f32 iw = 1.0f / (clip.w != 0.0f ? (clip.w < 0.0f ? -clip.w : clip.w)
                                        : 1e-6f);
    hal_f32 nx = clip.x * iw;
    hal_f32 ny = clip.y * iw;
    return hal_vec3f_make(
        viewport.x + (nx * 0.5f + 0.5f) * viewport.width,
        viewport.y + (0.5f - ny * 0.5f) * viewport.height, 2.0f);
}

hal_ray hal_camera_ray(const hal_camera *cam, hal_mat4f view_proj,
                       hal_rectf viewport, hal_f32 px, hal_f32 py) {
    hal_ray ray = { hal_camera_eye(cam), hal_vec3f_make(0.0f, 0.0f, -1.0f) };

    /* Invert the projection: NDC from the pixel, then unproject the
     * near and far points through the inverse matrix. */
    hal_f32 ndc_x =
        ((px - viewport.x) / viewport.width) * 2.0f - 1.0f;
    hal_f32 ndc_y =
        1.0f - ((py - viewport.y) / viewport.height) * 2.0f;

    hal_mat4f inv = hal_mat4f_invert(view_proj);
    hal_vec3f near_p = hal_mat4f_transform_point(
        inv, hal_vec3f_make(ndc_x, ndc_y, 0.0f));
    hal_vec3f far_p = hal_mat4f_transform_point(
        inv, hal_vec3f_make(ndc_x, ndc_y, 1.0f));

    ray.origin = near_p;
    ray.dir = hal_vec3f_normalize(hal_vec3f_sub(far_p, near_p));
    return ray;
}

hal_frustum hal_frustum_from_matrix(hal_mat4f vp) {
    /* Gribb-Hartmann: each plane is a row-combination of the matrix. */
    hal_frustum f;
    const hal_f32 *m = vp.m;
    /* Column-major storage: element (row r, col c) = m[c*4+r].
     * left  = row3 + row0, right = row3 - row0, etc. */
    hal_f32 rows[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            rows[r][c] = m[c * 4 + r];

    struct {
        int a, b;
    } combos[6] = {
        { 3, 0 }, { 3, 0 }, { 3, 1 }, { 3, 1 }, { 3, 2 }, { 3, 2 },
    };
    hal_f32 signs[6] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
    for (int i = 0; i < 6; i++) {
        hal_f32 a = signs[i];
        hal_vec4f p = hal_vec4f_make(
            rows[3][0] + a * rows[combos[i].b][0],
            rows[3][1] + a * rows[combos[i].b][1],
            rows[3][2] + a * rows[combos[i].b][2],
            rows[3][3] + a * rows[combos[i].b][3]);
        hal_f32 len = (hal_f32)__builtin_sqrt(
            (double)hal_vec4f_dot(p, p));
        if (len > 1e-9f) {
            f.planes[i] = hal_vec4f_make(p.x / len, p.y / len, p.z / len,
                                         p.w / len);
        } else {
            f.planes[i] = hal_vec4f_make(0.0f, 0.0f, 1.0f, 1e9f);
        }
    }
    return f;
}

bool hal_frustum_sphere_visible(const hal_frustum *f, hal_vec3f center,
                                hal_f32 radius) {
    if (!f)
        return true;
    for (int i = 0; i < 6; i++) {
        const hal_vec4f *p = &f->planes[i];
        hal_f32 d = p->x * center.x + p->y * center.y + p->z * center.z +
                    p->w;
        if (d < -radius)
            return false;
    }
    return true;
}
