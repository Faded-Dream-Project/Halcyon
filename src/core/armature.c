/*
 * armature.c — bones, posing, skinning, auto weights, 2-bone IK
 */

#include "armature.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hal_armature *hal_armature_create(void) {
    return calloc(1, sizeof(hal_armature));
}

void hal_armature_destroy(hal_armature *arm) {
    if (!arm)
        return;
    free(arm->bones);
    free(arm);
}

int hal_armature_bone_add(hal_armature *arm, const char *name,
                          hal_vec3f head, hal_vec3f tail, int parent) {
    if (!arm || (parent >= 0 && (size_t)parent >= arm->count))
        return -1;
    if (arm->count == arm->cap) {
        size_t cap = arm->cap ? arm->cap * 2 : 8;
        hal_bone *grown = realloc(arm->bones, cap * sizeof(*grown));
        if (!grown)
            return -1;
        arm->bones = grown;
        arm->cap = cap;
    }
    hal_bone *b = &arm->bones[arm->count];
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "%s",
             name && name[0] ? name : "Bone");
    b->head = head;
    b->tail = tail;
    b->parent = parent;
    b->pose = hal_quatf_identity();
    arm->count++;
    return (int)arm->count - 1;
}

hal_armature *hal_armature_default(void) {
    hal_armature *arm = hal_armature_create();
    if (!arm)
        return NULL;
    hal_armature_bone_add(arm, "Root", hal_vec3f_zero(),
                          hal_vec3f_make(0.0f, 1.0f, 0.0f), -1);
    return arm;
}

/* ---- Pose math ----
 * A bone's posed transform: the parent's posed matrix applied to the
 * bone's rest head gives the posed head; the bone's own direction is
 * the rest direction rotated by (parent rotation * pose). The matrix
 * itself is translate(head) * rotate(chain_rot * pose) so children
 * (whose rest positions are armature-space) ride along. */

static hal_quatf chain_rotation(const hal_armature *arm,
                                const hal_bone_pose *poses, int bone) {
    if (bone < 0)
        return hal_quatf_identity();
    hal_quatf parent = chain_rotation(arm, poses, arm->bones[bone].parent);
    return hal_quatf_mul(parent, arm->bones[bone].pose);
}

static void pose_build(const hal_armature *arm, hal_bone_pose *out,
                       bool apply_pose) {
    for (size_t i = 0; i < arm->count; i++) {
        const hal_bone *b = &arm->bones[i];
        hal_bone_pose *p = &out[i];

        hal_mat4f parent_m =
            b->parent >= 0 ? out[b->parent].matrix
                           : hal_mat4f_identity();
        p->head = hal_mat4f_transform_point(parent_m, b->head);
        hal_vec3f rest_dir = hal_vec3f_sub(b->tail, b->head);
        /* The REST pose ignores bone rotations entirely (bind
         * matrices must be the unposed chain); the POSED chain
         * composes the ancestors' pose quats with the local one. */
        hal_quatf total =
            apply_pose
                ? hal_quatf_mul(chain_rotation(arm, out, b->parent),
                                b->pose)
                : hal_quatf_identity();
        hal_vec3f dir = hal_quatf_rotate_vec3(total, rest_dir);
        p->tail = hal_vec3f_add(p->head, dir);

        hal_mat4f m = hal_quatf_to_mat4(total);
        m.m[12] = p->head.x;
        m.m[13] = p->head.y;
        m.m[14] = p->head.z;
        p->matrix = m;
    }
}

void hal_armature_pose(const hal_armature *arm, hal_bone_pose *out) {
    if (!arm || !out)
        return;
    pose_build(arm, out, true);
}

void hal_armature_rest_pose(const hal_armature *arm, hal_bone_pose *out) {
    if (!arm || !out)
        return;
    pose_build(arm, out, false);
}

/* ---- Skinning ---- */

bool hal_mesh_bind_armature(hal_mesh *mesh, const hal_armature *arm) {
    if (!mesh || !arm || arm->count == 0)
        return false;

    hal_f32 *w = calloc(mesh->vert_count * 4, sizeof(hal_f32));
    hal_i32 *b = malloc(mesh->vert_count * 4 * sizeof(hal_i32));
    if (!w || !b) {
        free(w);
        free(b);
        return false;
    }

    for (size_t v = 0; v < mesh->vert_count; v++) {
        /* Distance to every bone segment; keep the two strongest. */
        int best[2] = { -1, -1 };
        hal_f32 best_d[2] = { 1e30f, 1e30f };
        for (size_t k = 0; k < arm->count; k++) {
            /* Point-segment distance in rest space. */
            hal_vec3f a = arm->bones[k].head;
            hal_vec3f ab = hal_vec3f_sub(arm->bones[k].tail, a);
            hal_f32 len2 = hal_vec3f_length_sq(ab);
            hal_f32 t = 0.0f;
            if (len2 > 1e-9f)
                t = hal_vec3f_dot(hal_vec3f_sub(mesh->verts[v], a), ab) /
                    len2;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            hal_vec3f closest =
                hal_vec3f_add(a, hal_vec3f_mul(ab, t));
            hal_f32 d = hal_vec3f_distance(mesh->verts[v], closest) +
                        1e-4f; /* avoid divide-by-zero */
            if (d < best_d[0]) {
                best_d[1] = best_d[0];
                best[1] = best[0];
                best_d[0] = d;
                best[0] = (int)k;
            } else if (d < best_d[1]) {
                best_d[1] = d;
                best[1] = (int)k;
            }
        }

        hal_f32 w0 = best[0] >= 0 ? 1.0f / (best_d[0] * best_d[0]) : 0.0f;
        hal_f32 w1 = best[1] >= 0 ? 1.0f / (best_d[1] * best_d[1]) : 0.0f;
        hal_f32 sum = w0 + w1;
        if (sum <= 0.0f) {
            w0 = 1.0f;
            sum = 1.0f;
        }
        for (size_t i = 0; i < 4; i++) {
            b[v * 4 + i] = -1;
            w[v * 4 + i] = 0.0f;
        }
        b[v * 4] = best[0];
        w[v * 4] = w0 / sum;
        if (best[1] >= 0) {
            b[v * 4 + 1] = best[1];
            w[v * 4 + 1] = w1 / sum;
        }
    }

    free(mesh->skin_weights);
    free(mesh->skin_bones);
    mesh->skin_weights = w;
    mesh->skin_bones = b;
    return true;
}

bool hal_mesh_apply_pose_ex(const hal_mesh *mesh,
                            const hal_armature *arm,
                            hal_mat4f mesh_to_armature,
                            hal_vec3f *out) {
    if (!mesh || !out)
        return false;
    if (!arm || !mesh->skin_weights || !mesh->skin_bones ||
        arm->count == 0) {
        for (size_t v = 0; v < mesh->vert_count; v++)
            out[v] = mesh->verts[v];
        return true;
    }

    hal_bone_pose *posed = malloc(arm->count * sizeof(*posed));
    hal_bone_pose *rest = malloc(arm->count * sizeof(*rest));
    hal_mat4f *bind = malloc(arm->count * sizeof(*bind));
    if (!posed || !rest || !bind) {
        free(posed);
        free(rest);
        free(bind);
        return false;
    }
    hal_armature_pose(arm, posed);
    hal_armature_rest_pose(arm, rest);
    for (size_t k = 0; k < arm->count; k++)
        bind[k] = hal_mat4f_invert(rest[k].matrix);
    hal_mat4f back = hal_mat4f_invert(mesh_to_armature);

    for (size_t v = 0; v < mesh->vert_count; v++) {
        hal_vec3f p_arm = hal_mat4f_transform_point(
            mesh_to_armature, mesh->verts[v]);
        hal_vec3f acc = hal_vec3f_zero();
        hal_f32 wsum = 0.0f;
        for (size_t i = 0; i < 4; i++) {
            hal_i32 bi = mesh->skin_bones[v * 4 + i];
            hal_f32 wt = mesh->skin_weights[v * 4 + i];
            if (bi < 0 || bi >= (hal_i32)arm->count || wt <= 0.0f)
                continue;
            hal_vec3f bound =
                hal_mat4f_transform_point(bind[bi], p_arm);
            acc = hal_vec3f_add(
                acc,
                hal_vec3f_mul(
                    hal_mat4f_transform_point(posed[bi].matrix, bound),
                    wt));
            wsum += wt;
        }
        if (wsum > 1e-6f)
            acc = hal_vec3f_mul(acc, 1.0f / wsum);
        else
            acc = p_arm;
        out[v] = hal_mat4f_transform_point(back, acc);
    }

    free(posed);
    free(rest);
    free(bind);
    return true;
}

bool hal_mesh_apply_pose(const hal_mesh *mesh, const hal_armature *arm,
                         hal_vec3f *out) {
    /* Same-space binding: the mesh and armature share their local
     * space (the common bind setup; the render path uses _ex). */
    return hal_mesh_apply_pose_ex(mesh, arm, hal_mat4f_identity(), out);
}

/* ---- IK (analytic 2-bone) ---- */

bool hal_armature_ik_2bone(hal_armature *arm, int tip, hal_vec3f target) {
    if (!arm || tip < 0 || (size_t)tip >= arm->count)
        return false;
    int upper_i = arm->bones[tip].parent;
    if (upper_i < 0)
        return false; /* needs a 2-bone chain */

    hal_bone *upper = &arm->bones[upper_i];
    hal_bone *lower = &arm->bones[tip];

    hal_vec3f root = upper->head;
    hal_f32 l1 = hal_vec3f_distance(upper->head, upper->tail);
    hal_f32 l2 = hal_vec3f_distance(lower->head, lower->tail);
    if (l1 < 1e-6f || l2 < 1e-6f)
        return false;

    /* Clamp the reach to the annulus [|l1-l2|, l1+l2] (documented). */
    hal_vec3f to_target = hal_vec3f_sub(target, root);
    hal_f32 d = hal_vec3f_length(to_target);
    if (d < 1e-6f)
        return false;
    if (d > l1 + l2)
        d = l1 + l2;
    if (d < fabsf(l1 - l2) + 1e-6f)
        d = fabsf(l1 - l2) + 1e-6f;
    hal_vec3f dir_d = hal_vec3f_normalize(to_target);

    /* Law of cosines: the angle at the root between the target
     * direction and the upper bone. */
    hal_f32 cos_a = (l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d);
    if (cos_a > 1.0f)
        cos_a = 1.0f;
    if (cos_a < -1.0f)
        cos_a = -1.0f;
    hal_f32 angle_a = acosf(cos_a);

    /* Bend plane: spanned by the target direction and the upper
     * bone's rest direction (falls back to any perpendicular when
     * the chain is straight — the original construction used a
     * degenerate cross and never bent). */
    hal_vec3f rest_upper = hal_vec3f_normalize(
        hal_vec3f_sub(upper->tail, upper->head));
    hal_vec3f rest_lower = hal_vec3f_normalize(
        hal_vec3f_sub(lower->tail, lower->head));
    hal_vec3f axis_n = hal_vec3f_cross(rest_upper, dir_d);
    if (hal_vec3f_length_sq(axis_n) < 1e-8f)
        axis_n = hal_vec3f_cross(dir_d, hal_vec3f_axis_z());
    if (hal_vec3f_length_sq(axis_n) < 1e-8f)
        axis_n = hal_vec3f_cross(dir_d, hal_vec3f_axis_x());
    axis_n = hal_vec3f_normalize(axis_n);

    /* Upper: the target direction tilted by the elbow angle inside
     * the bend plane. */
    hal_vec3f side = hal_vec3f_cross(axis_n, dir_d);
    hal_vec3f desired_upper = hal_vec3f_add(
        hal_vec3f_mul(dir_d, cosf(angle_a)),
        hal_vec3f_mul(side, sinf(angle_a)));

    /* Lower: from the elbow straight at the target. */
    hal_vec3f elbow = hal_vec3f_add(root,
                                    hal_vec3f_mul(desired_upper, l1));
    hal_vec3f desired_lower =
        hal_vec3f_normalize(hal_vec3f_sub(target, elbow));

    upper->pose = hal_quatf_between(rest_upper, desired_upper);
    /* The lower bone's POSE is local: it must cancel the upper's
     * rotation and then aim at the target (the classic local-vs-world
     * rotation trap). Assumes the upper is the only posed ancestor
     * (documented limitation). */
    hal_quatf q_lower = hal_quatf_between(rest_lower, desired_lower);
    lower->pose =
        hal_quatf_mul(hal_quatf_conjugate(upper->pose), q_lower);
    return true;
}

