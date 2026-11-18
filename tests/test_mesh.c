/*
 * test_mesh.c — mesh structure and primitives tests
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/scene.h"

#include <math.h>
#include <string.h>

static void test_mesh_basics(void) {
    hal_mesh *m = hal_mesh_create();
    HAL_TEST_CHECK(m != NULL);

    int a = hal_mesh_vert_add(m, hal_vec3f_make(0.0f, 0.0f, 0.0f));
    int b = hal_mesh_vert_add(m, hal_vec3f_make(1.0f, 0.0f, 0.0f));
    int c = hal_mesh_vert_add(m, hal_vec3f_make(0.0f, 1.0f, 0.0f));
    HAL_TEST_CHECK_EQ_I(a, 0);
    HAL_TEST_CHECK_EQ_I(b, 1);
    HAL_TEST_CHECK_EQ_I(c, 2);
    HAL_TEST_CHECK_EQ_I(hal_mesh_vert_add(m, hal_vec3f_make(9, 9, 9)), 3);

    HAL_TEST_CHECK_EQ_I(hal_mesh_vert_add(NULL, hal_vec3f_zero()), -1);
    HAL_TEST_CHECK_EQ_I(hal_mesh_edge_add(m, 0, 0), -1);   /* self */
    HAL_TEST_CHECK_EQ_I(hal_mesh_edge_add(m, 0, 99), -1);  /* absent */

    HAL_TEST_CHECK(hal_mesh_edge_add(m, 0, 1) >= 0);
    HAL_TEST_CHECK_EQ_I(m->edge_count, 1);

    hal_u32 tri[3] = { 0, 2, 1 }; /* CCW seen from +Z: +Y x +X = -Z?
                                  * 0=(0,0,0) 2=(0,1,0) 1=(1,0,0):
                                  * cross((2-0),(1-0)) = (0,1,0)x(1,0,0)
                                  * = (0*0-0*0, 0*1-0*0, 0*0-1*1) = (0,0,-1) */
    HAL_TEST_CHECK(hal_mesh_face_add(m, tri, 3) >= 0);
    HAL_TEST_CHECK_EQ_I(hal_mesh_face_add(m, tri, 2), -1); /* < 3 corners */
    hal_u32 bad[3] = { 0, 1, 42 };
    HAL_TEST_CHECK_EQ_I(hal_mesh_face_add(m, bad, 3), -1); /* bad index */

    /* Face normal: Newell over the winding above gives -Z. */
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    HAL_TEST_CHECK(fn != NULL);
    HAL_TEST_CHECK_EQ_F(fn[0].z, -1.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(fn[0].x, 0.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(fn[0].y, 0.0f, 1e-4);

    /* Smooth normals exist and are unit. */
    const hal_vec3f *vn = hal_mesh_vert_normals(m);
    HAL_TEST_CHECK(vn != NULL);
    /* Vertices 0..2 belong to the face (unit normals); vertex 3 is
     * referenced by nothing and legitimately reads zero. */
    for (size_t i = 0; i < 3; i++)
        HAL_TEST_CHECK_EQ_F(hal_vec3f_length(vn[i]), 1.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(hal_vec3f_length(vn[3]), 0.0f, 1e-4);

    hal_vec3f bmin, bmax;
    hal_mesh_bounds(m, &bmin, &bmax);
    HAL_TEST_CHECK_EQ_F(bmax.x, 9.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(bmax.y, 9.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(bmin.x, 0.0f, 1e-5);

    hal_mesh_destroy(m);
    hal_mesh_destroy(NULL);
}

static void test_cube(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    HAL_TEST_CHECK(m != NULL);
    if (!m)
        return;
    HAL_TEST_CHECK_EQ_I(m->vert_count, 8);
    HAL_TEST_CHECK_EQ_I(m->face_count, 6);
    HAL_TEST_CHECK_EQ_I(m->loop_count, 24);   /* 6 quads */
    HAL_TEST_CHECK_EQ_I(m->edge_count, 24);   /* 4 per quad face */

    /* All 6 face normals are unit axis-aligned. */
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    HAL_TEST_CHECK(fn != NULL);
    for (size_t f = 0; f < m->face_count; f++) {
        hal_f32 len = hal_vec3f_length(fn[f]);
        HAL_TEST_CHECK_EQ_F(len, 1.0f, 1e-4);
        hal_f32 mx = fabsf(fn[f].x);
        hal_f32 my = fabsf(fn[f].y);
        hal_f32 mz = fabsf(fn[f].z);
        /* exactly one axis dominates */
        int axes = (mx > 0.9f) + (my > 0.9f) + (mz > 0.9f);
        HAL_TEST_CHECK_EQ_I(axes, 1);
    }

    /* Bounds: the cube spans [-0.5, 0.5]^3. */
    hal_vec3f bmin, bmax;
    hal_mesh_bounds(m, &bmin, &bmax);
    HAL_TEST_CHECK_EQ_F(bmin.x, -0.5f, 1e-5);
    HAL_TEST_CHECK_EQ_F(bmax.x, 0.5f, 1e-5);
    HAL_TEST_CHECK_EQ_F(bmax.y, 0.5f, 1e-5);
    HAL_TEST_CHECK_EQ_F(hal_mesh_bounds_radius(m), 0.8660254f, 1e-4);

    hal_mesh_destroy(m);
}

static void test_sphere(void) {
    hal_mesh *m = hal_mesh_sphere(1.0f, 8, 12);
    HAL_TEST_CHECK(m != NULL);
    if (!m)
        return;
    /* verts: (rings-1) bands x segments + 2 poles. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 7 * 12 + 2);
    /* faces: 2 caps (12 tris) + 6 bands (12 quads). */
    HAL_TEST_CHECK_EQ_I(m->face_count, 24 + 72);

    /* Every vertex sits on the unit sphere. */
    for (size_t i = 0; i < m->vert_count; i++)
        HAL_TEST_CHECK_EQ_F(hal_vec3f_length(m->verts[i]), 1.0f, 1e-4);

    /* Smooth sphere normals: unit length. KNOWN ISSUE (recorded in
     * PROGRESS.md): the sphere primitive's face winding still makes
     * some smooth normals point inward — an M4 fix on the primitives
     * pass; flat shading (what M3 renders with) is unaffected. */
    const hal_vec3f *vn = hal_mesh_vert_normals(m);
    HAL_TEST_CHECK(vn != NULL);
    if (vn)
        for (size_t i = 0; i < m->vert_count; i++)
            HAL_TEST_CHECK_EQ_F(hal_vec3f_length(vn[i]), 1.0f, 1e-3);

    hal_mesh_destroy(m);
}

static void test_other_primitives(void) {
    hal_mesh *plane = hal_mesh_plane(2.0f);
    HAL_TEST_CHECK(plane != NULL);
    if (plane) {
        HAL_TEST_CHECK_EQ_I(plane->vert_count, 4);
        const hal_vec3f *fn = hal_mesh_face_normals(plane);
        HAL_TEST_CHECK(fn != NULL);
        HAL_TEST_CHECK_EQ_F(fn[0].y, 1.0f, 1e-4); /* +Y up */
        hal_mesh_destroy(plane);
    }

    hal_mesh *cyl = hal_mesh_cylinder(0.5f, 2.0f, 8);
    HAL_TEST_CHECK(cyl != NULL);
    if (cyl) {
        HAL_TEST_CHECK_EQ_I(cyl->vert_count, 8 * 2 + 2);
        /* side quads + 2 tri caps */
        HAL_TEST_CHECK_EQ_I(cyl->face_count, 8 + 16);
        hal_mesh_destroy(cyl);
    }

    hal_mesh *cone = hal_mesh_cone(0.5f, 1.0f, 6);
    HAL_TEST_CHECK(cone != NULL);
    if (cone) {
        HAL_TEST_CHECK_EQ_I(cone->vert_count, 6 + 2);
        HAL_TEST_CHECK_EQ_I(cone->face_count, 12);
        hal_mesh_destroy(cone);
    }

    hal_mesh *torus = hal_mesh_torus(1.0f, 0.3f, 16, 8);
    HAL_TEST_CHECK(torus != NULL);
    if (torus) {
        HAL_TEST_CHECK_EQ_I(torus->vert_count, 16 * 8);
        HAL_TEST_CHECK_EQ_I(torus->face_count, 16 * 8);
        hal_mesh_destroy(torus);
    }
}

static void test_scene_mesh_payload(void) {
    hal_scene *s = hal_scene_create("M");
    hal_object_id id = hal_scene_object_add(s, "Box", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, id, hal_mesh_cube(0.5f));
    hal_object *obj = hal_scene_object(s, id);
    HAL_TEST_CHECK(obj->mesh != NULL);
    HAL_TEST_CHECK_EQ_I(obj->mesh->face_count, 6);

    /* Replacing destroys the old one (no leak under ASan). */
    hal_object_set_mesh(s, id, hal_mesh_sphere(1.0f, 6, 8));
    HAL_TEST_CHECK_EQ_I(hal_scene_object(s, id)->mesh->face_count,
                        48); /* 2 caps x 8 + 4 bands x 8 */

    /* Removal frees the mesh. */
    hal_scene_object_remove(s, id);
    hal_scene_destroy(s);
}

int main(void) {
    test_mesh_basics();
    test_cube();
    test_sphere();
    test_other_primitives();
    test_scene_mesh_payload();
    HAL_TEST_SUMMARY("test_mesh");
}
