/*
 * test_math.c — math library tests
 *
 * Pins the invariants the rest of Halcyon leans on: quaternion
 * rotation of known vectors, TRS compose/decompose round trips,
 * projection depth mapping, matrix inversion, slerp endpoints.
 */

#include "hal_test.h"
#include "core/math.h"

static void test_vec3(void) {
    hal_vec3f a = hal_vec3f_make(1.0f, 2.0f, 3.0f);
    hal_vec3f b = hal_vec3f_make(-4.0f, 0.5f, 2.0f);

    HAL_TEST_CHECK_EQ_F(hal_vec3f_dot(a, b), -4.0f + 1.0f + 6.0f, 1e-6);

    hal_vec3f c = hal_vec3f_cross(a, b);
    /* a x b = (2*2-3*0.5, 3*(-4)-1*2, 1*0.5-2*(-4)) = (2.5, -14, 8.5) */
    HAL_TEST_CHECK_EQ_F(c.x, 2.5f, 1e-6);
    HAL_TEST_CHECK_EQ_F(c.y, -14.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(c.z, 8.5f, 1e-6);

    /* Cross product anti-commutes. */
    hal_vec3f cb = hal_vec3f_cross(b, a);
    HAL_TEST_CHECK_EQ_F(cb.x, -c.x, 1e-6);

    hal_vec3f n = hal_vec3f_normalize(hal_vec3f_make(0.0f, 0.0f, 5.0f));
    HAL_TEST_CHECK_EQ_F(n.z, 1.0f, 1e-6);

    /* Zero vector normalizes to zero, never NaN. */
    hal_vec3f z = hal_vec3f_normalize(hal_vec3f_zero());
    HAL_TEST_CHECK_EQ_F(z.x, 0.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(z.y, 0.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(z.z, 0.0f, 1e-6);

    HAL_TEST_CHECK_EQ_F(hal_vec3f_length(hal_vec3f_make(3.0f, 4.0f, 0.0f)),
                        5.0f, 1e-6);
}

static void test_quat_rotate(void) {
    /* 90 degrees around Y: +X should map to -Z (right-handed, Y-up). */
    hal_quatf q = hal_quatf_from_axis_angle(hal_vec3f_axis_y(),
                                            90.0f * HAL_DEG2RAD_f32);
    hal_vec3f v = hal_quatf_rotate_vec3(q, hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(v.x, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(v.y, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(v.z, -1.0f, 1e-5);

    /* 90 degrees around Z: +X maps to +Y. */
    q = hal_quatf_from_axis_angle(hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    v = hal_quatf_rotate_vec3(q, hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(v.x, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(v.y, 1.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(v.z, 0.0f, 1e-5);

    /* Identity leaves everything alone. */
    v = hal_quatf_rotate_vec3(hal_quatf_identity(),
                              hal_vec3f_make(1.5f, -2.0f, 0.25f));
    HAL_TEST_CHECK_EQ_F(v.x, 1.5f, 1e-6);
    HAL_TEST_CHECK_EQ_F(v.y, -2.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(v.z, 0.25f, 1e-6);

    /* quat->mat4 agrees with quat-rotate for a non-axis vector. */
    q = hal_quatf_from_axis_angle(hal_vec3f_normalize(hal_vec3f_make(1.0f, 1.0f, 0.0f)),
                                  0.7f);
    hal_vec3f src = hal_vec3f_make(0.3f, -0.8f, 1.1f);
    hal_vec3f vq = hal_quatf_rotate_vec3(q, src);
    hal_vec3f vm = hal_mat4f_transform_dir(hal_quatf_to_mat4(q), src);
    HAL_TEST_CHECK_EQ_F(vq.x, vm.x, 1e-5);
    HAL_TEST_CHECK_EQ_F(vq.y, vm.y, 1e-5);
    HAL_TEST_CHECK_EQ_F(vq.z, vm.z, 1e-5);
}

static void test_euler_round_trip(void) {
    /* from_euler -> to_euler -> from_euler must be the identity for
     * rotations away from the gimbal singularity (|x| < 90 deg). */
    hal_vec3f e = hal_vec3f_make(0.3f, -0.8f, 1.2f);
    hal_quatf q = hal_quatf_from_euler_xyz(e);
    hal_vec3f e2 = hal_quatf_to_euler_xyz(q);
    HAL_TEST_CHECK_EQ_F(e2.x, e.x, 1e-4);
    HAL_TEST_CHECK_EQ_F(e2.y, e.y, 1e-4);
    HAL_TEST_CHECK_EQ_F(e2.z, e.z, 1e-4);

    hal_quatf q2 = hal_quatf_from_euler_xyz(e2);
    HAL_TEST_CHECK_EQ_F(q.x, q2.x, 1e-4);
    HAL_TEST_CHECK_EQ_F(q.y, q2.y, 1e-4);
    HAL_TEST_CHECK_EQ_F(q.z, q2.z, 1e-4);
    HAL_TEST_CHECK_EQ_F(q.w, q2.w, 1e-4);
}

static void test_trs_round_trip(void) {
    hal_vec3f loc = hal_vec3f_make(1.0f, -2.0f, 3.0f);
    hal_quatf rot = hal_quatf_from_axis_angle(
        hal_vec3f_normalize(hal_vec3f_make(0.3f, 0.9f, -0.2f)), 1.1f);
    hal_vec3f scale = hal_vec3f_make(2.0f, 0.5f, 1.5f);

    hal_mat4f m = hal_mat4f_from_trs(loc, rot, scale);

    hal_vec3f loc2;
    hal_quatf rot2;
    hal_vec3f scale2;
    HAL_TEST_CHECK(hal_mat4f_decompose_trs(m, &loc2, &rot2, &scale2));

    HAL_TEST_CHECK_EQ_F(loc2.x, loc.x, 1e-5);
    HAL_TEST_CHECK_EQ_F(loc2.y, loc.y, 1e-5);
    HAL_TEST_CHECK_EQ_F(loc2.z, loc.z, 1e-5);
    HAL_TEST_CHECK_EQ_F(scale2.x, scale.x, 1e-5);
    HAL_TEST_CHECK_EQ_F(scale2.y, scale.y, 1e-5);
    HAL_TEST_CHECK_EQ_F(scale2.z, scale.z, 1e-5);

    /* Quaternions double-cover: compare through rotation, not raw. */
    hal_vec3f probe = hal_vec3f_make(0.4f, 0.6f, -0.7f);
    hal_vec3f r1 = hal_quatf_rotate_vec3(rot, probe);
    hal_vec3f r2 = hal_quatf_rotate_vec3(rot2, probe);
    HAL_TEST_CHECK_EQ_F(r1.x, r2.x, 1e-4);
    HAL_TEST_CHECK_EQ_F(r1.y, r2.y, 1e-4);
    HAL_TEST_CHECK_EQ_F(r1.z, r2.z, 1e-4);
}

static void test_projection_depth(void) {
    /* NDC z in [0,1]: -near maps to 0, -far maps to 1. */
    hal_mat4f proj =
        hal_mat4f_perspective(60.0f * HAL_DEG2RAD_f32, 16.0f / 9.0f, 0.1f, 100.0f);

    hal_vec3f near_pt = hal_mat4f_transform_point(proj, hal_vec3f_make(0.0f, 0.0f, -0.1f));
    HAL_TEST_CHECK_EQ_F(near_pt.z, 0.0f, 1e-4);

    hal_vec3f far_pt = hal_mat4f_transform_point(proj, hal_vec3f_make(0.0f, 0.0f, -100.0f));
    HAL_TEST_CHECK_EQ_F(far_pt.z, 1.0f, 1e-4);

    /* A point in front of the camera has w > 0. */
    hal_vec4f mid = hal_mat4f_mul_vec4(
        proj, hal_vec4f_make(0.0f, 0.0f, -5.0f, 1.0f));
    HAL_TEST_CHECK(mid.w > 0.0f);
}

static void test_look_at(void) {
    hal_mat4f view = hal_mat4f_look_at(hal_vec3f_make(0.0f, 0.0f, 5.0f),
                                       hal_vec3f_zero(),
                                       hal_vec3f_axis_y());
    /* The origin is five units in front (+ view -Z = 5). */
    hal_vec3f o = hal_mat4f_transform_point(view, hal_vec3f_zero());
    HAL_TEST_CHECK_EQ_F(o.x, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(o.y, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(o.z, -5.0f, 1e-5);

    /* World +Y stays view +Y (no roll). */
    hal_vec3f up = hal_mat4f_transform_dir(view, hal_vec3f_axis_y());
    HAL_TEST_CHECK_EQ_F(up.y, 1.0f, 1e-5);
}

static void test_invert(void) {
    hal_mat4f m = hal_mat4f_from_trs(
        hal_vec3f_make(2.0f, -1.0f, 0.5f),
        hal_quatf_from_axis_angle(hal_vec3f_normalize(hal_vec3f_make(0.2f, 1.0f, -0.4f)), 0.9f),
        hal_vec3f_make(1.5f, 2.0f, 0.75f));

    hal_mat4f inv = hal_mat4f_invert(m);
    hal_mat4f prod = hal_mat4f_mul(m, inv);
    for (int i = 0; i < 16; i++) {
        hal_f32 want = (i % 5 == 0) ? 1.0f : 0.0f;
        HAL_TEST_CHECK_EQ_F(prod.m[i], want, 1e-4);
    }

    /* Degenerate (zero scale) returns identity, never NaN. */
    hal_mat4f deg = hal_mat4f_scale(hal_vec3f_zero());
    hal_mat4f d_inv = hal_mat4f_invert(deg);
    hal_mat4f ident = hal_mat4f_identity();
    for (int i = 0; i < 16; i++)
        HAL_TEST_CHECK_EQ_F(d_inv.m[i], ident.m[i], 1e-6);
}

static void test_slerp(void) {
    hal_quatf a = hal_quatf_identity();
    hal_quatf b = hal_quatf_from_axis_angle(hal_vec3f_axis_z(), HAL_PI_f32 * 0.5f);

    HAL_TEST_CHECK_EQ_F(hal_quatf_slerp(a, b, 0.0f).w, a.w, 1e-5);
    HAL_TEST_CHECK_EQ_F(hal_quatf_slerp(a, b, 0.0f).z, a.z, 1e-5);
    HAL_TEST_CHECK_EQ_F(hal_quatf_slerp(a, b, 1.0f).z, b.z, 1e-5);
    HAL_TEST_CHECK_EQ_F(hal_quatf_slerp(a, b, 1.0f).w, b.w, 1e-5);

    /* Halfway is a 45-degree rotation about Z. */
    hal_quatf h = hal_quatf_slerp(a, b, 0.5f);
    hal_vec3f v = hal_quatf_rotate_vec3(h, hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(v.x, 0.70710678f, 1e-4);
    HAL_TEST_CHECK_EQ_F(v.y, 0.70710678f, 1e-4);

    /* Short arc: slerping to the negated twin takes the same path. */
    hal_quatf nb = hal_quatf_make(-b.x, -b.y, -b.z, -b.w);
    hal_quatf hn = hal_quatf_slerp(a, nb, 0.5f);
    hal_vec3f vn = hal_quatf_rotate_vec3(hn, hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(vn.x, 0.70710678f, 1e-4);
}

int main(void) {
    test_vec3();
    test_quat_rotate();
    test_euler_round_trip();
    test_trs_round_trip();
    test_projection_depth();
    test_look_at();
    test_invert();
    test_slerp();
    HAL_TEST_SUMMARY("test_math");
}
