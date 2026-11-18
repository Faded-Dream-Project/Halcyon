/*
 * math.c — Halcyon math: the non-trivial implementations
 *
 * Composites, inversions, camera matrices, decompositions, slerp. The
 * small vector/matrix operations live as static inline in math.h.
 */

#include "math.h"

hal_mat4f hal_mat4f_from_trs(hal_vec3f location, hal_quatf rotation,
                             hal_vec3f scale) {
    /* T * R * S: rotation applied to scaled basis vectors, then moved. */
    hal_mat4f r = hal_quatf_to_mat4(rotation);
    r.m[0] *= scale.x;
    r.m[1] *= scale.x;
    r.m[2] *= scale.x;
    r.m[4] *= scale.y;
    r.m[5] *= scale.y;
    r.m[6] *= scale.y;
    r.m[8] *= scale.z;
    r.m[9] *= scale.z;
    r.m[10] *= scale.z;
    r.m[12] = location.x;
    r.m[13] = location.y;
    r.m[14] = location.z;
    return r;
}

hal_f32 hal_mat4f_determinant(hal_mat4f m) {
    const hal_f32 *a = m.m;
    hal_f32 s0 = a[0] * a[5] - a[1] * a[4];
    hal_f32 s1 = a[0] * a[6] - a[2] * a[4];
    hal_f32 s2 = a[0] * a[7] - a[3] * a[4];
    hal_f32 s3 = a[1] * a[6] - a[2] * a[5];
    hal_f32 s4 = a[1] * a[7] - a[3] * a[5];
    hal_f32 s5 = a[2] * a[7] - a[3] * a[6];

    hal_f32 c0 = a[8] * a[13] - a[9] * a[12];
    hal_f32 c1 = a[8] * a[14] - a[10] * a[12];
    hal_f32 c2 = a[8] * a[15] - a[11] * a[12];
    hal_f32 c3 = a[9] * a[14] - a[10] * a[13];
    hal_f32 c4 = a[9] * a[15] - a[11] * a[13];
    hal_f32 c5 = a[10] * a[15] - a[11] * a[14];

    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

hal_mat4f hal_mat4f_invert(hal_mat4f m) {
    const hal_f32 *a = m.m;
    hal_mat4f r;

    hal_f32 s0 = a[0] * a[5] - a[1] * a[4];
    hal_f32 s1 = a[0] * a[6] - a[2] * a[4];
    hal_f32 s2 = a[0] * a[7] - a[3] * a[4];
    hal_f32 s3 = a[1] * a[6] - a[2] * a[5];
    hal_f32 s4 = a[1] * a[7] - a[3] * a[5];
    hal_f32 s5 = a[2] * a[7] - a[3] * a[6];

    hal_f32 c0 = a[8] * a[13] - a[9] * a[12];
    hal_f32 c1 = a[8] * a[14] - a[10] * a[12];
    hal_f32 c2 = a[8] * a[15] - a[11] * a[12];
    hal_f32 c3 = a[9] * a[14] - a[10] * a[13];
    hal_f32 c4 = a[9] * a[15] - a[11] * a[13];
    hal_f32 c5 = a[10] * a[15] - a[11] * a[14];

    hal_f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (det > -HAL_EPS_f32 && det < HAL_EPS_f32)
        return hal_mat4f_identity(); /* documented degenerate fallback */

    hal_f32 inv_det = 1.0f / det;

    /* Adjugate / det, transposed into column-major slots. */
    r.m[0] = (a[5] * c5 - a[6] * c4 + a[7] * c3) * inv_det;
    r.m[1] = (-a[1] * c5 + a[2] * c4 - a[3] * c3) * inv_det;
    r.m[2] = (a[13] * s5 - a[14] * s4 + a[15] * s3) * inv_det;
    r.m[3] = (-a[9] * s5 + a[10] * s4 - a[11] * s3) * inv_det;

    r.m[4] = (-a[4] * c5 + a[6] * c2 - a[7] * c1) * inv_det;
    r.m[5] = (a[0] * c5 - a[2] * c2 + a[3] * c1) * inv_det;
    r.m[6] = (-a[12] * s5 + a[14] * s2 - a[15] * s1) * inv_det;
    r.m[7] = (a[8] * s5 - a[10] * s2 + a[11] * s1) * inv_det;

    r.m[8] = (a[4] * c4 - a[5] * c2 + a[7] * c0) * inv_det;
    r.m[9] = (-a[0] * c4 + a[1] * c2 - a[3] * c0) * inv_det;
    r.m[10] = (a[12] * s4 - a[13] * s2 + a[15] * s0) * inv_det;
    r.m[11] = (-a[8] * s4 + a[9] * s2 - a[11] * s0) * inv_det;

    r.m[12] = (-a[4] * c3 + a[5] * c1 - a[6] * c0) * inv_det;
    r.m[13] = (a[0] * c3 - a[1] * c1 + a[2] * c0) * inv_det;
    r.m[14] = (-a[12] * s3 + a[13] * s1 - a[14] * s0) * inv_det;
    r.m[15] = (a[8] * s3 - a[9] * s1 + a[10] * s0) * inv_det;

    return r;
}

hal_mat4f hal_mat4f_perspective(hal_f32 fovy_radians, hal_f32 aspect,
                                hal_f32 near_z, hal_f32 far_z) {
    hal_mat4f r;
    for (int i = 0; i < 16; i++)
        r.m[i] = 0.0f;

    hal_f32 f = (hal_f32)(1.0 / __builtin_tan((double)fovy_radians * 0.5));
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = far_z / (near_z - far_z); /* z=-near -> 0, z=-far -> 1 */
    r.m[11] = -1.0f;
    r.m[14] = near_z * far_z / (near_z - far_z);
    return r;
}

hal_mat4f hal_mat4f_ortho(hal_f32 left, hal_f32 right, hal_f32 bottom,
                          hal_f32 top, hal_f32 near_z, hal_f32 far_z) {
    hal_mat4f r;
    for (int i = 0; i < 16; i++)
        r.m[i] = 0.0f;
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = 1.0f / (near_z - far_z); /* z=-near -> 0, z=-far -> 1 */
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = near_z / (near_z - far_z);
    r.m[15] = 1.0f;
    return r;
}

hal_mat4f hal_mat4f_look_at(hal_vec3f eye, hal_vec3f target, hal_vec3f up) {
    hal_vec3f f = hal_vec3f_normalize(hal_vec3f_sub(target, eye));
    hal_vec3f s = hal_vec3f_normalize(hal_vec3f_cross(f, up));
    hal_vec3f u = hal_vec3f_cross(s, f);

    hal_mat4f r = hal_mat4f_identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -hal_vec3f_dot(s, eye);
    r.m[13] = -hal_vec3f_dot(u, eye);
    r.m[14] = hal_vec3f_dot(f, eye);
    return r;
}

bool hal_mat4f_decompose_trs(hal_mat4f m, hal_vec3f *out_location,
                             hal_quatf *out_rotation, hal_vec3f *out_scale) {
    if (out_location) {
        *out_location = hal_vec3f_make(m.m[12], m.m[13], m.m[14]);
    }

    /* Column lengths are the scale magnitudes. */
    hal_vec3f c0 = hal_vec3f_make(m.m[0], m.m[1], m.m[2]);
    hal_vec3f c1 = hal_vec3f_make(m.m[4], m.m[5], m.m[6]);
    hal_vec3f c2 = hal_vec3f_make(m.m[8], m.m[9], m.m[10]);
    hal_f32 sx = hal_vec3f_length(c0);
    hal_f32 sy = hal_vec3f_length(c1);
    hal_f32 sz = hal_vec3f_length(c2);

    /* Negative scale: the largest axis carries the determinant's sign. */
    hal_f32 det = hal_mat4f_determinant(m);
    if (det < 0.0f) {
        hal_f32 mx = sx, pick = 0.0f;
        if (sy > mx) { mx = sy; pick = 1.0f; }
        if (sz > mx) { mx = sz; pick = 2.0f; }
        if (pick < 0.5f) sx = -sx;
        else if (pick < 1.5f) sy = -sy;
        else sz = -sz;
    }
    if (out_scale)
        *out_scale = hal_vec3f_make(sx, sy, sz);

    if (out_rotation) {
        /* Orthonormalize the basis, then quaternion from matrix. */
        hal_f32 inv_x = sx > HAL_EPS_f32 || sx < -HAL_EPS_f32 ? 1.0f / sx
                                                              : 0.0f;
        hal_f32 inv_y = sy > HAL_EPS_f32 || sy < -HAL_EPS_f32 ? 1.0f / sy
                                                              : 0.0f;
        hal_f32 inv_z = sz > HAL_EPS_f32 || sz < -HAL_EPS_f32 ? 1.0f / sz
                                                              : 0.0f;
        hal_mat4f rot = hal_mat4f_identity();
        rot.m[0] = m.m[0] * inv_x;
        rot.m[1] = m.m[1] * inv_x;
        rot.m[2] = m.m[2] * inv_x;
        rot.m[4] = m.m[4] * inv_y;
        rot.m[5] = m.m[5] * inv_y;
        rot.m[6] = m.m[6] * inv_y;
        rot.m[8] = m.m[8] * inv_z;
        rot.m[9] = m.m[9] * inv_z;
        rot.m[10] = m.m[10] * inv_z;

        /* Shear detection: are the basis vectors orthogonal? */
        bool orthogonal =
            hal_f32_eq(hal_vec3f_dot(c0, c1), 0.0f) &&
            hal_f32_eq(hal_vec3f_dot(c0, c2), 0.0f) &&
            hal_f32_eq(hal_vec3f_dot(c1, c2), 0.0f);

        /* Standard rotation-matrix -> quaternion (Shepperd's method). */
        hal_f32 tr = rot.m[0] + rot.m[5] + rot.m[10];
        hal_quatf q;
        if (tr > 0.0f) {
            hal_f32 s = (hal_f32)__builtin_sqrt((double)(tr + 1.0f)) * 2.0f;
            q.w = 0.25f * s;
            q.x = (rot.m[6] - rot.m[9]) / s;
            q.y = (rot.m[8] - rot.m[2]) / s;
            q.z = (rot.m[1] - rot.m[4]) / s;
        } else if (rot.m[0] > rot.m[5] && rot.m[0] > rot.m[10]) {
            hal_f32 s =
                (hal_f32)__builtin_sqrt((double)(1.0f + rot.m[0] -
                                                 rot.m[5] - rot.m[10])) *
                2.0f;
            q.w = (rot.m[6] - rot.m[9]) / s;
            q.x = 0.25f * s;
            q.y = (rot.m[4] + rot.m[1]) / s;
            q.z = (rot.m[8] + rot.m[2]) / s;
        } else if (rot.m[5] > rot.m[10]) {
            hal_f32 s =
                (hal_f32)__builtin_sqrt((double)(1.0f + rot.m[5] -
                                                 rot.m[0] - rot.m[10])) *
                2.0f;
            q.w = (rot.m[8] - rot.m[2]) / s;
            q.x = (rot.m[4] + rot.m[1]) / s;
            q.y = 0.25f * s;
            q.z = (rot.m[9] + rot.m[6]) / s;
        } else {
            hal_f32 s =
                (hal_f32)__builtin_sqrt((double)(1.0f + rot.m[10] -
                                                 rot.m[0] - rot.m[5])) *
                2.0f;
            q.w = (rot.m[1] - rot.m[4]) / s;
            q.x = (rot.m[8] + rot.m[2]) / s;
            q.y = (rot.m[9] + rot.m[6]) / s;
            q.z = 0.25f * s;
        }
        *out_rotation = hal_quatf_normalize(q);
        return orthogonal;
    }
    return true;
}

hal_vec3f hal_quatf_to_euler_xyz(hal_quatf q) {
    /* Matches hal_quatf_from_euler_xyz (q = qx * qy * qz, intrinsic
     * XYZ): extract from the rotation matrix
     *   R = Rx*Ry*Rz  ->  x = atan2(-R12, R22),
     *                     y = asin(R02),
     *                     z = atan2(-R01, R00)
     * with R expressed through the quaternion's components. arcsin is
     * clamped to survive float noise at the poles. */
    hal_vec3f e;
    hal_f32 sinp = 2.0f * (q.x * q.z + q.w * q.y);
    sinp = hal_f32_clamp(sinp, -1.0f, 1.0f);
    e.x = (hal_f32)__builtin_atan2(
        (double)(2.0f * (q.w * q.x - q.y * q.z)),
        (double)(1.0f - 2.0f * (q.x * q.x + q.y * q.y)));
    e.y = (hal_f32)__builtin_asin((double)sinp);
    e.z = (hal_f32)__builtin_atan2(
        (double)(2.0f * (q.w * q.z - q.x * q.y)),
        (double)(1.0f - 2.0f * (q.y * q.y + q.z * q.z)));
    return e;
}

hal_quatf hal_quatf_slerp(hal_quatf a, hal_quatf b, hal_f32 t) {
    hal_f32 d = hal_quatf_dot(a, b);

    /* Shortest arc: negate b when the dot is negative. */
    if (d < 0.0f) {
        b = hal_quatf_make(-b.x, -b.y, -b.z, -b.w);
        d = -d;
    }

    /* Close enough: plain lerp (the division below would explode). */
    if (d > 0.9995f) {
        hal_quatf r = hal_quatf_make(
            hal_f32_lerp(a.x, b.x, t), hal_f32_lerp(a.y, b.y, t),
            hal_f32_lerp(a.z, b.z, t), hal_f32_lerp(a.w, b.w, t));
        return hal_quatf_normalize(r);
    }

    hal_f32 theta_0 = (hal_f32)__builtin_acos((double)d);
    hal_f32 sin_theta_0 = (hal_f32)__builtin_sin((double)theta_0);
    hal_f32 s0 = (hal_f32)__builtin_sin((double)(theta_0 * (1.0f - t))) /
                 sin_theta_0;
    hal_f32 s1 = (hal_f32)__builtin_sin((double)(theta_0 * t)) / sin_theta_0;

    return hal_quatf_make(a.x * s0 + b.x * s1, a.y * s0 + b.y * s1,
                          a.z * s0 + b.z * s1, a.w * s0 + b.w * s1);
}
