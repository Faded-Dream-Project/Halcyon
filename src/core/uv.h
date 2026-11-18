/*
 * uv.h — UV unwrap operators (planar, box, smart-lite)
 *
 * UVs are per-corner (per loop). All unwraps write normalized
 * coordinates into [0,1] (islands stay inside their cells).
 *
 * Smart-lite (honest scope): faces group into islands across shared
 * edges when their normals agree (dot > 0.9); each island projects on
 * its average normal's dominant axis and the islands pack greedily
 * into rows. It is a pragmatic projection unwrap, not angle-based
 * flattening.
 */

#ifndef HAL_UV_H
#define HAL_UV_H

#include "mesh.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hal_uv_axis {
    HAL_UV_X = 0,
    HAL_UV_Y = 1,
    HAL_UV_Z = 2,
} hal_uv_axis;

/* Projects every loop onto the plane perpendicular to `axis`
 * (dropping that coordinate), normalized to the mesh bounds. */
bool hal_mesh_unwrap_planar(hal_mesh *m, hal_uv_axis axis);

/* Box unwrap: each face lands in the cell of its dominant-normal
 * axis (3x2 grid), normalized to that group's bounds. */
bool hal_mesh_unwrap_box(hal_mesh *m);

/* Smart-lite unwrap: normal-similarity islands, per-island dominant
 * axis projection, greedy row packing. */
bool hal_mesh_unwrap_smart(hal_mesh *m);
/* ANGLE-BASED unwrap: edges whose faces' dihedral angle exceeds
 * `threshold_deg` become seams; charts grow across non-seam shared
 * EDGES (true adjacency, unlike smart-lite's vert adjacency); each
 * chart projects onto its dominant plane and the charts shelf-pack
 * into the UV square. */
bool hal_mesh_unwrap_angle(hal_mesh *m, hal_f32 threshold_deg);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UV_H */
