/*
 * test_modeling.c — modeling quality (v0.2 items): region-aware
 * extrude, angle-based unwrap, knife cuts
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/uv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* --- Region-aware extrude --- */

static void select_all_faces(hal_mesh *m) {
    hal_mesh_select_all(m, false);
    for (size_t f = 0; f < m->face_count; f++)
        m->face_sel[f] = 1;
}

static void test_region_extrude(void) {
    /* A subdivided plane: extruding the WHOLE thing as a region
     * produces walls ONLY on the outer boundary — interior shared
     * edges stay seamless. */
    hal_mesh *m = hal_mesh_plane(1.0f);
    hal_mesh_subdivide(m);
    select_all_faces(m);
    size_t faces_before = m->face_count;
    size_t verts_before = m->vert_count;
    HAL_TEST_CHECK(hal_mesh_extrude_region(m, 0.5f));

    /* The top region has faces_before faces; walls = boundary edges
     * of the plane = 4 per original quad strip... the plane after
     * one subdivide is a 2x2 grid of quads: boundary edges = 8. */
    hal_u8 *was_top = malloc(m->face_count);
    for (size_t f = 0; f < m->face_count; f++)
        was_top[f] = f < faces_before;
    (void)was_top;
    /* Total faces = region (4) + walls (8). */
    HAL_TEST_CHECK_EQ_I(m->face_count, faces_before + 8);
    /* Every region vert duplicated exactly once. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, verts_before * 2);
    free(was_top);

    /* A SINGLE selected face behaves like the per-face extrude. */
    hal_mesh *c = hal_mesh_cube(1.0f);
    hal_mesh_select_all(c, false);
    c->face_sel[0] = 1; /* the top face (cube winding) */
    size_t cf = c->face_count;
    HAL_TEST_CHECK(hal_mesh_extrude_region(c, 0.5f));
    /* One wall quad per boundary edge of the quad face = 4. */
    HAL_TEST_CHECK_EQ_I(c->face_count, cf + 4);
    hal_mesh_destroy(c);
    hal_mesh_destroy(m);

    /* Region extrude keeps interior edges seamless: after extruding
     * the 2x2 region, NO wall face exists between two interior
     * columns (verified by face count above). */
}

/* --- Angle-based unwrap --- */

static void test_angle_unwrap(void) {
    /* A cube: every edge is a 90-degree crease > 45 threshold, so
     * six charts; every UV lands inside [0,1]. */
    hal_mesh *m = hal_mesh_cube(1.0f);
    HAL_TEST_CHECK(hal_mesh_unwrap_angle(m, 45.0f));
    bool in_range = true;
    for (size_t l = 0; l < m->loop_count; l++)
        if (m->uvs[l].x < -1e-4f || m->uvs[l].x > 1.0f + 1e-4f ||
            m->uvs[l].y < -1e-4f || m->uvs[l].y > 1.0f + 1e-4f)
            in_range = false;
    HAL_TEST_CHECK(in_range);
    /* Each face's UVs span a real area (not collapsed). */
    bool spans = true;
    for (size_t f = 0; f < m->face_count; f++) {
        hal_f32 minx = 1e30f, maxx = -1e30f;
        for (size_t i = 0;
             i < m->face_off[f + 1] - m->face_off[f]; i++) {
            hal_vec2f uv = m->uvs[m->face_off[f] + i];
            minx = fminf(minx, uv.x);
            maxx = fmaxf(maxx, uv.x);
        }
        if (maxx - minx < 0.05f)
            spans = false; /* charts are scaled to 0.45 max */
    }
    HAL_TEST_CHECK(spans);
    hal_mesh_destroy(m);

    /* A flat plane: no creases at all -> ONE chart (all UVs share
     * the same normalized box). */
    hal_mesh *p = hal_mesh_plane(1.0f);
    HAL_TEST_CHECK(hal_mesh_unwrap_angle(p, 45.0f));
    hal_f32 minx = 1e30f, maxx = -1e30f;
    for (size_t l = 0; l < p->loop_count; l++) {
        minx = fminf(minx, p->uvs[l].x);
        maxx = fmaxf(maxx, p->uvs[l].x);
    }
    /* One chart normalized to 0.45 wide: the spread is exactly the
     * chart scale. */
    HAL_TEST_CHECK_EQ_F(maxx - minx, 0.45f, 1e-4f);
    hal_mesh_destroy(p);
}

/* --- Knife --- */

static void test_knife(void) {
    /* Cut a quad straight across: two faces, two new verts. */
    hal_mesh *m = hal_mesh_plane(1.0f); /* one quad */
    size_t faces0 = m->face_count, verts0 = m->vert_count;
    size_t cuts = hal_mesh_cut_segment(
        m, hal_mat4f_identity(),
        hal_vec3f_make(0.0f, 0.0f, -2.0f),
        hal_vec3f_make(0.0f, 0.0f, 2.0f));
    HAL_TEST_CHECK_EQ_I(cuts, 1);
    HAL_TEST_CHECK_EQ_I(m->face_count, faces0 + 1);
    HAL_TEST_CHECK_EQ_I(m->vert_count, verts0 + 2);
    /* The cut verts sit ON the cut line (x=0). */
    for (size_t v = verts0; v < m->vert_count; v++)
        HAL_TEST_CHECK_EQ_F(m->verts[v].x, 0.0f, 1e-4f);

    /* A segment that misses everything cuts nothing. */
    size_t cuts2 = hal_mesh_cut_segment(
        m, hal_mat4f_identity(),
        hal_vec3f_make(5.0f, 0.0f, -2.0f),
        hal_vec3f_make(5.0f, 0.0f, 2.0f));
    HAL_TEST_CHECK_EQ_I(cuts2, 0);
    hal_mesh_destroy(m);

    /* A knife stroke ON the cube's top face (in its plane, crossing
     * two of its edges): that face splits, the count is exact. (A
     * line through the cube's interior crosses no face EDGES — the
     * knife is a surface stroke, not a volume bisector.) */
    hal_mesh *c = hal_mesh_cube(1.0f);
    size_t faces_c = c->face_count;
    size_t cuts3 = hal_mesh_cut_segment(
        c, hal_mat4f_identity(),
        hal_vec3f_make(-2.0f, 1.0f, 0.0f),
        hal_vec3f_make(2.0f, 1.0f, 0.0f));
    HAL_TEST_CHECK_EQ_I(cuts3, 1);
    HAL_TEST_CHECK_EQ_I(c->face_count, faces_c + 1);
    hal_mesh_destroy(c);
}

int main(void) {
    test_region_extrude();
    test_angle_unwrap();
    test_knife();
    HAL_TEST_SUMMARY("test_modeling");
}
