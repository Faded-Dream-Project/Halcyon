/*
 * pick.h — viewport picking: objects, faces (ray cast) and
 * verts/edges (projected screen-space nearest)
 */

#ifndef HAL_PICK_H
#define HAL_PICK_H

#include "../core/mesh.h"
#include "../core/scene.h"
#include "camera.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hal_pick_kind {
    HAL_PICK_NONE = 0,
    HAL_PICK_OBJECT,
    HAL_PICK_VERT,
    HAL_PICK_EDGE,
    HAL_PICK_FACE,
} hal_pick_kind;

typedef struct hal_pick {
    hal_pick_kind kind;
    hal_object_id object; /* the owning object (0 for NONE)   */
    size_t index;         /* vert/edge/face index             */
    hal_f32 depth;        /* view-space distance of the hit   */
} hal_pick;

/* Möller–Trumbore; returns the ray parameter t, or -1 when missed. */
hal_f32 hal_ray_triangle(hal_ray r, hal_vec3f v0, hal_vec3f v1,
                         hal_vec3f v2);

/* Nearest face hit on one mesh (world transform applied), or -1. */
int hal_pick_face(hal_mesh *mesh, hal_mat4f world, hal_ray ray,
                  hal_f32 *out_t);

/* Topmost object (with its face index) under a viewport pixel. */
hal_pick hal_pick_scene(hal_scene *scene, hal_camera *cam,
                        hal_mat4f view_proj, hal_rectf viewport,
                        hal_f32 px, hal_f32 py);

/* Screen-space element picking on one mesh: the vert/edge whose
 * projection is nearest the pixel within `max_dist` logical px
 * (edges measure point-to-segment distance). -1 when nothing is
 * close enough. */
int hal_pick_vert(hal_mesh *mesh, hal_mat4f world, hal_camera *cam,
                  hal_mat4f view_proj, hal_rectf viewport, hal_f32 px,
                  hal_f32 py, hal_f32 max_dist);
int hal_pick_edge(hal_mesh *mesh, hal_mat4f world, hal_camera *cam,
                  hal_mat4f view_proj, hal_rectf viewport, hal_f32 px,
                  hal_f32 py, hal_f32 max_dist);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PICK_H */
