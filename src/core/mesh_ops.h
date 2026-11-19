/*
 * mesh_ops.h — mesh selection state and modeling operators
 *
 * Selection lives on the mesh (byte per vertex/edge/face); the editor
 * owns WHICH selection mode (vertex/edge/face) is active. Operators
 * mutate the mesh in place and return true when anything changed —
 * the editor snapshots the mesh before calling (hal_mesh_clone) and
 * pushes the pair onto the undo stack, so every operation is undoable
 * (decision D-009: undo snapshots geometry; command-swap semantics).
 *
 * Scope notes (honest): extrude and inset operate per face (extruding
 * a region of adjacent faces leaves seams between the duplicates —
 * region-aware extrude is a later refinement); bevel handles manifold
 * edges whose endpoints each belong to only one selected edge (the
 * single-edge case exactly); loop cut requires quad faces in the ring.
 */

#ifndef HAL_MESH_OPS_H
#define HAL_MESH_OPS_H

#include "mesh.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Selection ---- */

/* Selection bytes are grown with the arrays (0 = unselected). */
void hal_mesh_select_all(hal_mesh *m, bool on);
size_t hal_mesh_count_selected_verts(const hal_mesh *m);
size_t hal_mesh_count_selected_edges(const hal_mesh *m);
size_t hal_mesh_count_selected_faces(const hal_mesh *m);

/* ---- Clone (undo snapshots) ---- */

hal_mesh *hal_mesh_clone(const hal_mesh *src);

/* ---- Operators (in place; true when the mesh changed) ---- */

/* Extrudes every selected face along its normal by `offset`,
 * duplicating the face's verts, connecting side quads, selecting the
 * new verts and the new face. */
bool hal_mesh_extrude_faces(hal_mesh *m, hal_f32 offset);
/* REGION-AWARE extrude: connected selected faces move as ONE region
 * (verts duplicated once, walls only on boundary edges — shared
 * interior edges stay seamless). Identical to the per-face extrude
 * for a single face. */
bool hal_mesh_extrude_region(hal_mesh *m, hal_f32 offset);

/* Insets every selected face: new corner verts lerped toward the
 * face centroid by t in (0,1), an inner face, and a ring of quads.
 * Selects the inner face's verts. */
bool hal_mesh_inset_faces(hal_mesh *m, hal_f32 t);

/* Bevels selected manifold edges by `amount` (world units): each
 * endpoint slides toward the average of its edge + face neighbor
 * directions, the adjacent faces are re-hooked to the new verts, and
 * a bevel quad fills the gap. */
bool hal_mesh_bevel_edges(hal_mesh *m, hal_f32 amount);

/* Cuts an edge loop through the quad ring containing `edge` at
 * parameter t in (0,1) along the ring's side edges. Returns true when
 * a ring was found and cut. */
bool hal_mesh_loop_cut(hal_mesh *m, hal_u32 edge, hal_f32 t);

/* Subdivides selected faces (all faces when none are selected): edge
 * midpoints (shared), a face center, and one quad per corner. */
bool hal_mesh_subdivide(hal_mesh *m);

/* Merges selected verts to their centroid. Faces referencing merged
 * verts are re-hooked; loops that collapse (consecutive duplicates)
 * shrink; faces below 3 corners die. */
bool hal_mesh_merge_selected_verts(hal_mesh *m);

typedef enum hal_delete_mode {
    HAL_DELETE_VERTS = 0,
    HAL_DELETE_FACES = 1,
} hal_delete_mode;

/* Deletes selected verts (with every edge/face touching them) or the
 * selected faces. Compacts the arrays and rebuilds edges. */
bool hal_mesh_delete_selected(hal_mesh *m, hal_delete_mode mode);

typedef enum hal_mirror_axis {
    HAL_MIRROR_X = 0,
    HAL_MIRROR_Y = 1,
    HAL_MIRROR_Z = 2,
} hal_mirror_axis;

/* Mirrors every vert across the object-local axis plane (the object
 * origin) and flips the winding so normals stay outward. */
bool hal_mesh_mirror(hal_mesh *m, hal_mirror_axis axis);

/* Reverses every face's winding (flips normals). */
void hal_mesh_flip_winding(hal_mesh *m);

/* Drops the derived-edge array and rebuilds it from the faces (each
 * undirected face edge once). Callers that mutate loops directly use
 * this; the operators call it themselves. */
void hal_mesh_rebuild_edges(hal_mesh *m);

/* Recomputes the normal caches (they are lazy; this forces). */
void hal_mesh_recalc_normals(hal_mesh *m);

/* ---- Convex hull (collision shapes) ----
 * The convex hull of the mesh's vertices as a new closed triangle
 * mesh (a collision shape). Returns NULL for degenerate inputs
 * (coplanar/collinear point sets) — callers fall back to a box.
 * Every input vertex lies on or inside the result (property-tested
 * in the suite). */
hal_mesh *hal_mesh_convex_hull(const hal_mesh *m);

/* ---- Knife ----
 * Cuts every face the WORLD-SPACE segment (a,b) crosses: crossings
 * with face edges become new verts (interpolated on the edge), and
 * each face with exactly two crossings splits into two. Returns the
 * number of cuts. */
size_t hal_mesh_cut_segment(hal_mesh *m, hal_mat4f world,
                            hal_vec3f a, hal_vec3f b);

/* ---- Weight painting ----
 * Adjusts `bone`'s weight on every vert within `radius` (world
 * units) of `point` (world space, transformed back through `world`);
 * smoothstep falloff, weights renormalized per vert. strength > 0
 * adds, < 0 subtracts. Returns true when anything changed. Requires
 * bound weights (skin_bones/skin_weights). */
bool hal_mesh_paint_weight(hal_mesh *m, hal_mat4f world,
                           hal_vec3f point, hal_f32 radius,
                           int bone, hal_f32 strength);

/* ---- LOD generation ----
 * Vertex-clustering decimation: verts snap to a lattice of `cell`
 * (scene units), welded verts merge, loops remap, and faces that
 * collapse (consecutive duplicates or below 3 corners) drop. Simple
 * and predictable — NOT quadric-based. Returns true when the mesh
 * changed. */
bool hal_mesh_decimate_cluster(hal_mesh *m, hal_f32 cell);

/* ---- Origin tools (M9) ----
 * Both SHIFT the geometry and return the offset applied, so the
 * caller can compensate the object's location (keeping the geometry
 * in place in the world). */

/* Moves the origin to the mesh's bounding-box center. */
hal_vec3f hal_mesh_origin_to_center(hal_mesh *m);

/* Moves the origin to the world origin (mesh-local (0,0,0)). */
hal_vec3f hal_mesh_origin_to_zero(hal_mesh *m);

#ifdef __cplusplus
}
#endif

#endif /* HAL_MESH_OPS_H */
