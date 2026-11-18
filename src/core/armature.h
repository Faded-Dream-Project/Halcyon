/*
 * armature.h — bones, posing, skinning, auto weights, 2-bone IK
 *
 * Model (honest, simple): a bone is a rest-space head→tail segment in
 * its ARMATURE object's local space, plus a local pose rotation
 * applied about the bone's head. A child bone's rest positions are
 * expressed in the same armature space; the posed transform of the
 * parent chain carries them (rotating a parent swings descendants).
 *
 * Mesh objects reference an armature object (skin_armature); verts
 * carry up to 4 (bone, weight) influences. Skinning uses the standard
 * inverse-bind formulation: p' = Σ w_i * P_i * B_i^-1 * p.
 *
 * Auto weights (honest scope): nearest-bone-segment inverse-square
 * blending (2 strongest influences, normalized) — not heat diffusion.
 * IK (honest scope): an analytic 2-bone solver for a child-of-root
 * chain (the chain root's own pose is not re-solved).
 */

#ifndef HAL_ARMATURE_H
#define HAL_ARMATURE_H

#include "../platform/types.h"
#include "math.h"
#include "mesh.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_bone {
    char name[64];
    hal_vec3f head;      /* rest, armature space                  */
    hal_vec3f tail;      /* rest, armature space                  */
    hal_i32 parent;      /* bone index or -1                      */
    hal_quatf pose;      /* local pose rotation (identity = rest) */
    bool selected;
} hal_bone;

typedef struct hal_armature {
    hal_bone *bones;
    size_t count, cap;
} hal_armature;

hal_armature *hal_armature_create(void);
void hal_armature_destroy(hal_armature *arm);

/* Appends a bone (name copied; parent may be -1 or an existing bone).
 * Returns its index or -1. */
int hal_armature_bone_add(hal_armature *arm, const char *name,
                          hal_vec3f head, hal_vec3f tail, int parent);

/* A starter human-ish chain? No — a single root bone (0,0,0)-(0,1,0):
 * the Add > Armature entry point. */
hal_armature *hal_armature_default(void);

/* ---- Pose math ---- */

typedef struct hal_bone_pose {
    hal_vec3f head, tail;   /* posed, armature space           */
    hal_mat4f matrix;       /* posed transform, armature space */
} hal_bone_pose;

/* Poses the whole armature (one entry per bone, in bone order). */
void hal_armature_pose(const hal_armature *arm, hal_bone_pose *out);

/* The rest pose (unposed) for reference/bind computation. */
void hal_armature_rest_pose(const hal_armature *arm, hal_bone_pose *out);

/* ---- Skinning ---- */

/* Attaches `mesh` to `arm`: allocates 4 influences per vertex and
 * computes automatic weights (nearest-segment inverse-square). The
 * mesh keeps the weights; the object remembers the armature id.
 * Returns false on allocation failure. */
bool hal_mesh_bind_armature(hal_mesh *mesh, const hal_armature *arm);

/* Applies the armature's pose to the mesh's verts (writes the posed
 * positions into `out`, count == mesh->vert_count). Objects with no
 * weights copy the verts unchanged. */
bool hal_mesh_apply_pose(const hal_mesh *mesh, const hal_armature *arm,
                         hal_vec3f *out);

/* ---- IK ---- */

/* Analytic 2-bone IK: `tip` and its parent form the chain; `target`
 * is in armature space. Poles: the solution stays in the plane
 * spanned by the chain and the target. Returns true when the chain
 * is solvable (|l1 - l2| <= d <= l1 + l2 clamps otherwise). */
bool hal_armature_ik_2bone(hal_armature *arm, int tip,
                           hal_vec3f target);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ARMATURE_H */
