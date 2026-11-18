/*
 * mesh.h — Halcyon's mesh data structure and primitives
 *
 * A mesh is vertices, edges and polygon faces (a face is a run of
 * "loops", each loop a vertex index) — the shape Blender's edit mode
 * works with, chosen because M4's modeling tools (extrude, inset,
 * loop cut, merge) are natural over it. Faces are planar polygons
 * with CCW winding (right-handed, D-005); rendering triangulates with
 * a fan (faces Halcyon creates are convex; the M4+ tools keep them so).
 *
 * Normals are derived data: per-face (flat) computed from the winding,
 * per-vertex (smooth) area-weighted from adjacent faces, both cached
 * with a dirty flag the mutators set.
 */

#ifndef HAL_MESH_H
#define HAL_MESH_H

#include "../platform/types.h"
#include "math.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_mesh {
    /* Positions. */
    hal_vec3f *verts;
    size_t vert_count;
    size_t vert_cap;

    /* Edges: pairs of vertex indices (edge i = edges[2i], edges[2i+1]). */
    hal_u32 *edges;
    size_t edge_count; /* number of EDGES (loops below use vertex ids) */
    size_t edge_cap;

    /* Faces: face f's loops are loops[face_off[f] .. face_off[f+1]). */
    hal_u32 *loops;      /* vertex index per corner               */
    size_t loop_count;
    size_t loop_cap;
    size_t *face_off;    /* face_count + 1 entries                */
    size_t face_count;
    size_t face_cap;

    /* Per-corner (per-loop) texture coordinates, one per loop entry;
     * NULL until an unwrap fills them. uvs[loop] with v flipped for
     * image conventions (stored with +v DOWN like glTF/OGL images). */
    hal_vec2f *uvs;
    size_t uv_cap; /* private growth state                    */
    bool uvs_valid; /* set by an unwrap; cleared when loops are
                     * rebuilt from scratch                    */
    /* Edit-mode selection, one byte per element, grown with the
     * arrays (see mesh_ops.h). The capacities are private growth
     * state; readers use the counts above. */
    hal_u8 *vert_sel;
    size_t vert_sel_cap;
    hal_u8 *edge_sel;
    size_t edge_sel_cap;
    hal_u8 *face_sel;
    size_t face_sel_cap;

    /* Derived (owned caches; NULL until requested). */
    hal_vec3f *face_normals;  /* one per face                   */
    hal_vec3f *vert_normals;  /* one per vertex (smooth)        */
    hal_vec3f bbox_min, bbox_max;
    bool bbox_valid;
} hal_mesh;

hal_mesh *hal_mesh_create(void);
void hal_mesh_destroy(hal_mesh *mesh);

/* Vertex/edge/face mutators (they invalidate the normal and bbox
 * caches). Adding a face takes its corner vertex ids; the loop array
 * is copied. Returns the face index or -1. */
int hal_mesh_vert_add(hal_mesh *m, hal_vec3f p);
int hal_mesh_edge_add(hal_mesh *m, hal_u32 a, hal_u32 b);
int hal_mesh_face_add(hal_mesh *m, const hal_u32 *corners, size_t count);

/* Normals (recomputed lazily). */
const hal_vec3f *hal_mesh_face_normals(hal_mesh *m);
const hal_vec3f *hal_mesh_vert_normals(hal_mesh *m);

/* Drops the normal/bounds caches (the mutators call this; public for
 * mesh_ops). */
void hal_mesh_invalidate_derived(hal_mesh *m);

/* The bounding box (computed lazily; an empty mesh reports zeros). */
void hal_mesh_bounds(hal_mesh *m, hal_vec3f *out_min, hal_vec3f *out_max);

/* The bounding sphere radius around the origin (for culling). */
hal_f32 hal_mesh_bounds_radius(hal_mesh *m);

/* ---- Primitives (created at the origin, size "radius"-flavored) ---- */

/* A unit-ish cube: 2*size on each edge, centered. Quads (6 faces). */
hal_mesh *hal_mesh_cube(hal_f32 size);
/* UV sphere: rings x segments, radius r. Quads + triangle poles. */
hal_mesh *hal_mesh_sphere(hal_f32 radius, int rings, int segments);
/* Plane on XZ, 2*size across, one quad, +Y normal. */
hal_mesh *hal_mesh_plane(hal_f32 size);
/* Cylinder along +Y: r x height x r, capped (side quads + cap fans). */
hal_mesh *hal_mesh_cylinder(hal_f32 radius, hal_f32 height, int segments);
/* Cone along +Y (tip up): base radius r, height h. */
hal_mesh *hal_mesh_cone(hal_f32 radius, hal_f32 height, int segments);
/* Torus around +Y: main radius R, tube radius r. */
hal_mesh *hal_mesh_torus(hal_f32 main_radius, hal_f32 tube_radius,
                         int main_segments, int tube_segments);

#ifdef __cplusplus
}
#endif

#endif /* HAL_MESH_H */
