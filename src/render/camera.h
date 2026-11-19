/*
 * camera.h — the viewport camera: orbit / pan / zoom and the
 * projection math
 *
 * An orbit camera: a target point, a distance, and yaw/pitch angles
 * (the "turntable" model every 3D tool uses). Pan moves the target in
 * the view plane; zoom scales the distance. Perspective only for now.
 */

#ifndef HAL_CAMERA_H
#define HAL_CAMERA_H

#include "../core/math.h"
#include "../platform/types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_camera {
    hal_vec3f target;   /* the orbit pivot                       */
    hal_f32 distance;   /* eye distance from the target (> 0)    */
    hal_f32 yaw;        /* radians around +Y (0 looks along -Z)  */
    hal_f32 pitch;      /* radians above the horizon, clamped    */
    hal_f32 fov_y;      /* vertical field of view, radians       */
    hal_f32 near_z;
    hal_f32 far_z;
} hal_camera;

/* A sensible default: looking at the origin from up-and-front. */
hal_camera hal_camera_default(void);

/* The eye position (from target + angles + distance). */
hal_vec3f hal_camera_eye(const hal_camera *cam);

/* View and projection matrices (D-005 conventions: right-handed,
 * Y-up, NDC z in [0,1]). */
hal_mat4f hal_camera_view(const hal_camera *cam);
hal_mat4f hal_camera_projection(const hal_camera *cam, hal_f32 aspect);

/* ---- Controls (viewport input; deltas in screen units) ---- */

/* Orbit: yaw follows +x, pitch follows +y (both in radians). */
void hal_camera_orbit(hal_camera *cam, hal_f32 dx_radians,
                      hal_f32 dy_radians);

/* Pan: moves the target in the view plane. dx/dy in world units per
 * the camera's scale at the target distance (the caller multiplies by
 * a screen-to-world factor). */
void hal_camera_pan(hal_camera *cam, hal_f32 dx, hal_f32 dy);

/* Zoom: factor < 1 pulls in, > 1 pushes out (clamped to a sane
 * range). */
void hal_camera_zoom(hal_camera *cam, hal_f32 factor);

/* ---- Screen mapping ----
 * `viewport` is in the SAME units as the projection's aspect basis
 * (the canvas's logical pixels); the origin is its top-left. */

/* World point -> viewport pixel. out_w is the clip w (negative when
 * behind the eye — the caller checks). */
hal_vec3f hal_camera_project(const hal_camera *cam, hal_mat4f view_proj,
                             hal_vec3f world, hal_rectf viewport,
                             hal_f32 *out_w);

/* Viewport pixel -> a world-space ray (origin + normalized direction)
 * for picking. */
typedef struct hal_ray {
    hal_vec3f origin;
    hal_vec3f dir; /* normalized */
} hal_ray;

hal_ray hal_camera_ray(const hal_camera *cam, hal_mat4f view_proj,
                       hal_rectf viewport, hal_f32 px, hal_f32 py);

/* View frustum planes for culling: 6 planes (L R B T N F) as
 * (a,b,c,d) with dot(n, p) + d >= 0 inside. */
typedef struct hal_frustum {
    hal_vec4f planes[6];
} hal_frustum;

hal_frustum hal_frustum_from_matrix(hal_mat4f view_proj);

/* Sphere (center, radius) fully outside any plane = invisible. */
bool hal_frustum_sphere_visible(const hal_frustum *f, hal_vec3f center,
                                hal_f32 radius);

#ifdef __cplusplus
}
#endif

#endif /* HAL_CAMERA_H */
