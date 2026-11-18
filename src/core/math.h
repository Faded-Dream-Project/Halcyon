/*
 * math.h — Halcyon's math library
 *
 * Written from scratch: 2/3/4-component vectors (f32 + i32), 4x4
 * matrices, unit quaternions. No external math dependency, ever.
 *
 * Conventions (decision D-005 — matches glTF 2.0, Halcyon's primary
 * export target):
 *   - Right-handed coordinates, Y up, +Z forward.
 *   - Matrices are hal_mat4f, COLUMN-major: element (row, col) lives at
 *     m[col * 4 + row]. Points transform as v' = M * v (column vector).
 *   - Quaternions are (x, y, z, w), unit length after normalize;
 *     identity is (0, 0, 0, 1).
 *   - Perspective depth maps to NDC z in [0, 1] (glTF/D3D-style), not
 *     [-1, 1]; Halcyon's rasterizer (M3) is the consumer.
 *
 * Small operations are static inline (they are the inner loop of the
 * rasterizer and the animation solver); the non-trivial decompositions
 * live in math.c.
 */

#ifndef HAL_MATH_H
#define HAL_MATH_H

#include "../platform/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---- */

#define HAL_PI_f32 3.14159265358979323846f
#define HAL_TAU_f32 6.28318530717958647692f
#define HAL_DEG2RAD_f32 0.01745329251994329577f
#define HAL_RAD2DEG_f32 57.29577951308232088f

/* Epsilon for "close enough" float comparisons in tests and picking. */
#define HAL_EPS_f32 1e-5f

static inline bool hal_f32_eq(hal_f32 a, hal_f32 b) {
    hal_f32 d = a - b;
    return d < HAL_EPS_f32 && d > -HAL_EPS_f32;
}

static inline hal_f32 hal_f32_min(hal_f32 a, hal_f32 b) { return a < b ? a : b; }
static inline hal_f32 hal_f32_max(hal_f32 a, hal_f32 b) { return a > b ? a : b; }
static inline hal_f32 hal_f32_clamp(hal_f32 v, hal_f32 lo, hal_f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline hal_f32 hal_f32_lerp(hal_f32 a, hal_f32 b, hal_f32 t) {
    return a + (b - a) * t;
}

/* ---- vec2f ---- */

typedef struct hal_vec2f {
    hal_f32 x, y;
} hal_vec2f;

static inline hal_vec2f hal_vec2f_make(hal_f32 x, hal_f32 y) {
    hal_vec2f v = { x, y };
    return v;
}
static inline hal_vec2f hal_vec2f_add(hal_vec2f a, hal_vec2f b) {
    return hal_vec2f_make(a.x + b.x, a.y + b.y);
}
static inline hal_vec2f hal_vec2f_sub(hal_vec2f a, hal_vec2f b) {
    return hal_vec2f_make(a.x - b.x, a.y - b.y);
}
static inline hal_vec2f hal_vec2f_mul(hal_vec2f a, hal_f32 s) {
    return hal_vec2f_make(a.x * s, a.y * s);
}
static inline hal_vec2f hal_vec2f_div(hal_vec2f a, hal_f32 s) {
    return hal_vec2f_make(a.x / s, a.y / s);
}
static inline hal_f32 hal_vec2f_dot(hal_vec2f a, hal_vec2f b) {
    return a.x * b.x + a.y * b.y;
}
static inline hal_f32 hal_vec2f_length_sq(hal_vec2f a) {
    return hal_vec2f_dot(a, a);
}
static inline hal_f32 hal_vec2f_length(hal_vec2f a) {
    /* sqrtf would need -lm everywhere math.h is included; the compiler
     * folds the double round-trip fine and it stays exact for floats
     * with SSE2 sqrt. */
    return (hal_f32)__builtin_sqrt((double)hal_vec2f_length_sq(a));
}
static inline hal_vec2f hal_vec2f_normalize(hal_vec2f a) {
    hal_f32 len = hal_vec2f_length(a);
    if (len < HAL_EPS_f32)
        return hal_vec2f_make(0.0f, 0.0f);
    return hal_vec2f_div(a, len);
}
static inline hal_vec2f hal_vec2f_lerp(hal_vec2f a, hal_vec2f b, hal_f32 t) {
    return hal_vec2f_make(hal_f32_lerp(a.x, b.x, t),
                          hal_f32_lerp(a.y, b.y, t));
}

/* ---- vec3f ---- */

typedef struct hal_vec3f {
    hal_f32 x, y, z;
} hal_vec3f;

static inline hal_vec3f hal_vec3f_make(hal_f32 x, hal_f32 y, hal_f32 z) {
    hal_vec3f v = { x, y, z };
    return v;
}
static inline hal_vec3f hal_vec3f_zero(void) {
    return hal_vec3f_make(0.0f, 0.0f, 0.0f);
}
static inline hal_vec3f hal_vec3f_one(void) {
    return hal_vec3f_make(1.0f, 1.0f, 1.0f);
}
static inline hal_vec3f hal_vec3f_add(hal_vec3f a, hal_vec3f b) {
    return hal_vec3f_make(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline hal_vec3f hal_vec3f_sub(hal_vec3f a, hal_vec3f b) {
    return hal_vec3f_make(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline hal_vec3f hal_vec3f_mul(hal_vec3f a, hal_f32 s) {
    return hal_vec3f_make(a.x * s, a.y * s, a.z * s);
}
static inline hal_vec3f hal_vec3f_mulv(hal_vec3f a, hal_vec3f b) {
    return hal_vec3f_make(a.x * b.x, a.y * b.y, a.z * b.z);
}
static inline hal_vec3f hal_vec3f_div(hal_vec3f a, hal_f32 s) {
    return hal_vec3f_make(a.x / s, a.y / s, a.z / s);
}
static inline hal_vec3f hal_vec3f_neg(hal_vec3f a) {
    return hal_vec3f_make(-a.x, -a.y, -a.z);
}
static inline hal_f32 hal_vec3f_dot(hal_vec3f a, hal_vec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline hal_vec3f hal_vec3f_cross(hal_vec3f a, hal_vec3f b) {
    return hal_vec3f_make(a.y * b.z - a.z * b.y,
                          a.z * b.x - a.x * b.z,
                          a.x * b.y - a.y * b.x);
}
static inline hal_f32 hal_vec3f_length_sq(hal_vec3f a) {
    return hal_vec3f_dot(a, a);
}
static inline hal_f32 hal_vec3f_length(hal_vec3f a) {
    return (hal_f32)__builtin_sqrt((double)hal_vec3f_length_sq(a));
}
static inline hal_f32 hal_vec3f_distance(hal_vec3f a, hal_vec3f b) {
    return hal_vec3f_length(hal_vec3f_sub(a, b));
}
/* Zero-length input normalizes to zero (never NaN). */
static inline hal_vec3f hal_vec3f_normalize(hal_vec3f a) {
    hal_f32 len = hal_vec3f_length(a);
    if (len < HAL_EPS_f32)
        return hal_vec3f_zero();
    return hal_vec3f_div(a, len);
}
static inline hal_vec3f hal_vec3f_lerp(hal_vec3f a, hal_vec3f b, hal_f32 t) {
    return hal_vec3f_make(hal_f32_lerp(a.x, b.x, t),
                          hal_f32_lerp(a.y, b.y, t),
                          hal_f32_lerp(a.z, b.z, t));
}
static inline bool hal_vec3f_eq(hal_vec3f a, hal_vec3f b) {
    return hal_f32_eq(a.x, b.x) && hal_f32_eq(a.y, b.y) &&
           hal_f32_eq(a.z, b.z);
}

/* Axis helpers (the D-005 basis). */
static inline hal_vec3f hal_vec3f_axis_x(void) { return hal_vec3f_make(1.0f, 0.0f, 0.0f); }
static inline hal_vec3f hal_vec3f_axis_y(void) { return hal_vec3f_make(0.0f, 1.0f, 0.0f); }
static inline hal_vec3f hal_vec3f_axis_z(void) { return hal_vec3f_make(0.0f, 0.0f, 1.0f); }

/* ---- vec4f ---- */

typedef struct hal_vec4f {
    hal_f32 x, y, z, w;
} hal_vec4f;

static inline hal_vec4f hal_vec4f_make(hal_f32 x, hal_f32 y, hal_f32 z,
                                       hal_f32 w) {
    hal_vec4f v = { x, y, z, w };
    return v;
}
static inline hal_vec4f hal_vec4f_add(hal_vec4f a, hal_vec4f b) {
    return hal_vec4f_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}
static inline hal_f32 hal_vec4f_dot(hal_vec4f a, hal_vec4f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
static inline hal_vec4f hal_vec4f_lerp(hal_vec4f a, hal_vec4f b, hal_f32 t) {
    return hal_vec4f_make(hal_f32_lerp(a.x, b.x, t),
                          hal_f32_lerp(a.y, b.y, t),
                          hal_f32_lerp(a.z, b.z, t),
                          hal_f32_lerp(a.w, b.w, t));
}

/* ---- Integer vectors (rasterizer pixels, mesh indices) ---- */

typedef struct hal_vec2i {
    hal_i32 x, y;
} hal_vec2i;

static inline hal_vec2i hal_vec2i_make(hal_i32 x, hal_i32 y) {
    hal_vec2i v = { x, y };
    return v;
}

/* ---- mat4f (column-major, v' = M * v) ---- */

typedef struct hal_mat4f {
    hal_f32 m[16]; /* m[col * 4 + row] */
} hal_mat4f;

static inline hal_mat4f hal_mat4f_identity(void) {
    hal_mat4f r;
    for (int i = 0; i < 16; i++)
        r.m[i] = 0.0f;
    r.m[0] = 1.0f;
    r.m[5] = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
}

/* out = a * b ("apply b first, then a" for column vectors). */
static inline hal_mat4f hal_mat4f_mul(hal_mat4f a, hal_mat4f b) {
    hal_mat4f r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            hal_f32 sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

static inline hal_vec4f hal_mat4f_mul_vec4(hal_mat4f m, hal_vec4f v) {
    return hal_vec4f_make(
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w);
}

/* Point (w=1) then divide by w — the projective transform. */
static inline hal_vec3f hal_mat4f_transform_point(hal_mat4f m, hal_vec3f p) {
    hal_vec4f r = hal_mat4f_mul_vec4(
        m, hal_vec4f_make(p.x, p.y, p.z, 1.0f));
    if (r.w != 1.0f && r.w > HAL_EPS_f32)
        return hal_vec3f_make(r.x / r.w, r.y / r.w, r.z / r.w);
    return hal_vec3f_make(r.x, r.y, r.z);
}

/* Direction (w=0, no translation, no divide). */
static inline hal_vec3f hal_mat4f_transform_dir(hal_mat4f m, hal_vec3f d) {
    return hal_vec3f_make(
        m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
        m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
        m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z);
}

static inline hal_mat4f hal_mat4f_translate(hal_vec3f t) {
    hal_mat4f r = hal_mat4f_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

static inline hal_mat4f hal_mat4f_scale(hal_vec3f s) {
    hal_mat4f r = hal_mat4f_identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

static inline hal_mat4f hal_mat4f_transpose(hal_mat4f m) {
    hal_mat4f r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r.m[row * 4 + c] = m.m[c * 4 + row];
    return r;
}

/* ---- Quaternion (x, y, z, w; unit after normalize) ---- */

typedef struct hal_quatf {
    hal_f32 x, y, z, w;
} hal_quatf;

static inline hal_quatf hal_quatf_identity(void) {
    hal_quatf q = { 0.0f, 0.0f, 0.0f, 1.0f };
    return q;
}
static inline hal_quatf hal_quatf_make(hal_f32 x, hal_f32 y, hal_f32 z,
                                       hal_f32 w) {
    hal_quatf q = { x, y, z, w };
    return q;
}
static inline hal_f32 hal_quatf_dot(hal_quatf a, hal_quatf b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
static inline hal_f32 hal_quatf_length(hal_quatf a) {
    return (hal_f32)__builtin_sqrt((double)hal_quatf_dot(a, a));
}
static inline hal_quatf hal_quatf_normalize(hal_quatf a) {
    hal_f32 len = hal_quatf_length(a);
    if (len < HAL_EPS_f32)
        return hal_quatf_identity();
    hal_f32 inv = 1.0f / len;
    return hal_quatf_make(a.x * inv, a.y * inv, a.z * inv, a.w * inv);
}
static inline hal_quatf hal_quatf_conjugate(hal_quatf a) {
    return hal_quatf_make(-a.x, -a.y, -a.z, a.w);
}
static inline hal_quatf hal_quatf_mul(hal_quatf a, hal_quatf b) {
    return hal_quatf_make(
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

/* Rotation by `radians` around the (normalized) `axis`. */
static inline hal_quatf hal_quatf_from_axis_angle(hal_vec3f axis,
                                                  hal_f32 radians) {
    axis = hal_vec3f_normalize(axis);
    hal_f32 h = radians * 0.5f;
    hal_f32 s = (hal_f32)__builtin_sin((double)h);
    hal_f32 c = (hal_f32)__builtin_cos((double)h);
    return hal_quatf_make(axis.x * s, axis.y * s, axis.z * s, c);
}

/* Euler angles (radians), intrinsic XYZ: q = qx * qy * qz. */
static inline hal_quatf hal_quatf_from_euler_xyz(hal_vec3f euler) {
    hal_quatf qx = hal_quatf_from_axis_angle(hal_vec3f_axis_x(), euler.x);
    hal_quatf qy = hal_quatf_from_axis_angle(hal_vec3f_axis_y(), euler.y);
    hal_quatf qz = hal_quatf_from_axis_angle(hal_vec3f_axis_z(), euler.z);
    return hal_quatf_mul(hal_quatf_mul(qx, qy), qz);
}

/* Rotates v by q (q v q*). q must be unit for exact rotation. */
static inline hal_vec3f hal_quatf_rotate_vec3(hal_quatf q, hal_vec3f v) {
    hal_vec3f qv = hal_vec3f_make(q.x, q.y, q.z);
    hal_vec3f t = hal_vec3f_mul(hal_vec3f_cross(qv, v), 2.0f);
    return hal_vec3f_add(
        v, hal_vec3f_add(hal_vec3f_mul(t, q.w), hal_vec3f_cross(qv, t)));
}

/* Rotation-only matrix from a unit quaternion (column-major). */
static inline hal_mat4f hal_quatf_to_mat4(hal_quatf q) {
    hal_mat4f r = hal_mat4f_identity();
    hal_f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    hal_f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    hal_f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

/* ---- Non-trivial math (implemented in math.c) ---- */

/* Translation * Rotation * Scale composite. */
hal_mat4f hal_mat4f_from_trs(hal_vec3f location, hal_quatf rotation,
                             hal_vec3f scale);

/* General 4x4 inverse (cofactor expansion). Degenerate input (zero
 * determinant) returns identity — callers never NaN. */
hal_mat4f hal_mat4f_invert(hal_mat4f m);

hal_f32 hal_mat4f_determinant(hal_mat4f m);

/* Right-handed projection, NDC z in [0, 1] (see the header preamble).
 * fovy in radians; aspect = width / height; near/far > 0. */
hal_mat4f hal_mat4f_perspective(hal_f32 fovy_radians, hal_f32 aspect,
                                hal_f32 near_z, hal_f32 far_z);

/* Right-handed orthographic box; NDC z in [0, 1]. */
hal_mat4f hal_mat4f_ortho(hal_f32 left, hal_f32 right, hal_f32 bottom,
                          hal_f32 top, hal_f32 near_z, hal_f32 far_z);

/* Camera view matrix: looks from `eye` toward `target`, `up` hints the
 * +Y direction (world up (0,1,0) is the usual). */
hal_mat4f hal_mat4f_look_at(hal_vec3f eye, hal_vec3f target, hal_vec3f up);

/* Decomposes a T*R*S matrix back into its parts (rotation normalized).
 * Negative scale is reported as a negative axis (the largest |scale|).
 * Returns false when the matrix is not a T*R*S composite (shear) —
 * parts are still a best effort. */
bool hal_mat4f_decompose_trs(hal_mat4f m, hal_vec3f *out_location,
                             hal_quatf *out_rotation, hal_vec3f *out_scale);

/* Euler angles (radians, intrinsic XYZ) matching
 * hal_quatf_from_euler_xyz for non-degenerate rotations. */
hal_vec3f hal_quatf_to_euler_xyz(hal_quatf q);

/* Spherical interpolation (M7 animation curves). t in [0, 1]; takes
 * the short arc. */
hal_quatf hal_quatf_slerp(hal_quatf a, hal_quatf b, hal_f32 t);

/* The minimal rotation taking unit vector `from` to unit vector `to`
 * (identity when parallel; any perpendicular axis when opposed). */
hal_quatf hal_quatf_between(hal_vec3f from, hal_vec3f to);

#ifdef __cplusplus
}
#endif

#endif /* HAL_MATH_H */
