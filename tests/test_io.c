/*
 * test_io.c — export/import round trips: glTF 2.0 GLB, OBJ, STL
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/scene.h"
#include "io/io.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-io-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* A scene with a transformed cube and a child sphere. */
static hal_scene *make_scene(void) {
    hal_scene *s = hal_scene_create("RoundTrip");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, cube, hal_mesh_cube(0.5f));
    hal_object_set_location(s, cube, hal_vec3f_make(1.0f, 0.0f, 0.0f));
    hal_object_set_rotation_euler(
        s, cube, hal_vec3f_make(0.0f, 0.7f, 0.0f));

    hal_object_id ball =
        hal_scene_object_add(s, "Ball", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, ball, hal_mesh_sphere(0.5f, 8, 12));
    hal_object_set_parent(s, ball, cube);
    hal_object_set_location(s, ball, hal_vec3f_make(0.0f, 1.2f, 0.0f));
    hal_object_set_smooth_shading(s, ball, true);
    return s;
}

static void test_gltf_round_trip(void) {
    hal_scene *s = make_scene();
    char path[512];
    snprintf(path, sizeof(path), "%s/rt.glb", tmp_dir());
    char err[256] = { 0 };

    HAL_TEST_CHECK_EQ_I(hal_io_gltf_export(s, path, err, sizeof(err)),
                        HAL_OK);

    /* Structural check: GLB magic, version, chunk layout. */
    FILE *f = fopen(path, "rb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        hal_u32 hdr[3];
        HAL_TEST_CHECK(fread(hdr, sizeof(hdr), 1, f) == 1);
        HAL_TEST_CHECK_EQ_I(hdr[0], 0x46546C67u);
        HAL_TEST_CHECK_EQ_I(hdr[1], 2u);
        struct stat st;
        stat(path, &st);
        HAL_TEST_CHECK_EQ_I(hdr[2], (hal_u32)st.st_size);
        hal_u32 chunk[2];
        HAL_TEST_CHECK(fread(chunk, sizeof(chunk), 1, f) == 1);
        HAL_TEST_CHECK_EQ_I(chunk[1], 0x4E4F534Au); /* "JSON" */
        fclose(f);
    }

    /* Round trip. */
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        /* Two mesh objects; the hierarchy is flattened to world
         * transforms on import. */
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 2);

        /* Find by name: Cube first (export order preserved). */
        hal_object *o0 = hal_scene_object_at(loaded, 0);
        hal_object *o1 = hal_scene_object_at(loaded, 1);
        HAL_TEST_CHECK(o0 && o1);
        if (o0 && o1) {
            HAL_TEST_CHECK(strcmp(o0->name, "Cube") == 0);
            HAL_TEST_CHECK(strcmp(o1->name, "Ball") == 0);
            /* The cube is FLAT shaded: the exporter duplicates
             * positions per corner (12 tris x 3 = 36 verts, 12 tri
             * faces). The smooth sphere keeps its 86 verts, its 96
             * faces triangulating to 168. */
            HAL_TEST_CHECK_EQ_I(o0->mesh->vert_count, 36);
            HAL_TEST_CHECK_EQ_I(o0->mesh->face_count, 12);
            HAL_TEST_CHECK_EQ_I(o1->mesh->vert_count, 7 * 12 + 2);
            HAL_TEST_CHECK_EQ_I(o1->mesh->face_count, 24 + 144);

            /* Cube location round-trips exactly. */
            HAL_TEST_CHECK_EQ_F(o0->location.x, 1.0f, 1e-5);
            HAL_TEST_CHECK_EQ_F(o0->location.y, 0.0f, 1e-5);

            /* Ball: imported with the BAKED world transform (parent
             * rotation + own offset). Its world position must match
             * the source scene's world matrix. */
            hal_object *src_ball = hal_scene_object_at(s, 1);
            hal_mat4f src_world =
                hal_object_world_matrix(s, src_ball);
            hal_vec3f src_pos =
                hal_mat4f_transform_point(src_world, hal_vec3f_zero());
            hal_mat4f out_world =
                hal_object_world_matrix(loaded, o1);
            hal_vec3f out_pos =
                hal_mat4f_transform_point(out_world, hal_vec3f_zero());
            HAL_TEST_CHECK(hal_vec3f_distance(src_pos, out_pos) < 1e-4);

            /* Geometry round-trip through the duplicated stream:
             * compare bounds (per-corner order is unspecified). */
            hal_vec3f smin, smax, imin, imax;
            hal_object *src_cube = hal_scene_object_at(s, 0);
            hal_mesh_bounds(src_cube->mesh, &smin, &smax);
            hal_mesh_bounds(o0->mesh, &imin, &imax);
            HAL_TEST_CHECK_EQ_F(imin.x, smin.x, 1e-5);
            HAL_TEST_CHECK_EQ_F(imax.y, smax.y, 1e-5);
            HAL_TEST_CHECK_EQ_F(imax.z, smax.z, 1e-5);
        }
        hal_scene_destroy(loaded);
    }

    /* A truncated file refuses loudly. */
    snprintf(path, sizeof(path), "%s/bad.glb", tmp_dir());
    f = fopen(path, "wb");
    fwrite("not a glb at all....", 1, 20, f);
    fclose(f);
    loaded = NULL;
    err[0] = '\0';
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)),
        HAL_ERR_INVALID_ARGUMENT);
    HAL_TEST_CHECK(loaded == NULL);
    HAL_TEST_CHECK(err[0] != '\0');

    hal_scene_destroy(s);
}

static void test_obj_round_trip(void) {
    hal_scene *s = make_scene();
    char path[512];
    snprintf(path, sizeof(path), "%s/rt.obj", tmp_dir());
    char err[256] = { 0 };

    HAL_TEST_CHECK_EQ_I(hal_io_obj_export(s, path, err, sizeof(err)),
                        HAL_OK);

    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_obj_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 2);
        hal_object *o0 = hal_scene_object_at(loaded, 0);
        hal_object *o1 = hal_scene_object_at(loaded, 1);
        HAL_TEST_CHECK(o0 && o1);
        if (o0 && o1) {
            /* OBJ stores faces as polygons: the cube keeps its 6
             * quads, the sphere keeps quads + tri caps. */
            HAL_TEST_CHECK_EQ_I(o0->mesh->vert_count, 8);
            HAL_TEST_CHECK_EQ_I(o0->mesh->face_count, 6);
            HAL_TEST_CHECK_EQ_I(o1->mesh->vert_count, 7 * 12 + 2);
            HAL_TEST_CHECK_EQ_I(o1->mesh->face_count, 96);

            /* A position survives. */
            HAL_TEST_CHECK_EQ_F(o0->mesh->verts[0].x, -0.5f, 1e-5);

            /* Relative (negative) indices: hand-write a tiny OBJ. */
            snprintf(path, sizeof(path), "%s/rel.obj", tmp_dir());
            FILE *f = fopen(path, "wb");
            fputs("o T\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n",
                  f);
            fclose(f);
            hal_scene *rel = NULL;
            HAL_TEST_CHECK_EQ_I(
                hal_io_obj_import(path, &rel, err, sizeof(err)), HAL_OK);
            HAL_TEST_CHECK(rel != NULL);
            if (rel) {
                hal_object *t = hal_scene_object_at(rel, 0);
                HAL_TEST_CHECK(t && t->mesh);
                if (t && t->mesh) {
                    HAL_TEST_CHECK_EQ_I(t->mesh->face_count, 1);
                    HAL_TEST_CHECK_EQ_I(t->mesh->loops[0], 0);
                    HAL_TEST_CHECK_EQ_I(t->mesh->loops[2], 2);
                }
                hal_scene_destroy(rel);
            }
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

static void test_stl_export(void) {
    hal_scene *s = make_scene();
    char path[512];
    snprintf(path, sizeof(path), "%s/rt.stl", tmp_dir());
    char err[256] = { 0 };

    HAL_TEST_CHECK_EQ_I(hal_io_stl_export(s, path, err, sizeof(err)),
                        HAL_OK);

    FILE *f = fopen(path, "rb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        char header[80];
        hal_u32 count = 0;
        HAL_TEST_CHECK(fread(header, 80, 1, f) == 1);
        HAL_TEST_CHECK(fread(&count, 4, 1, f) == 1);
        /* Cube: 6 quads = 12 tris; sphere: 96 faces (24 tris + 72
         * quads -> 24 + 144) = 168. Total 180. */
        HAL_TEST_CHECK_EQ_I(count, 180u);
        struct stat st;
        stat(path, &st);
        HAL_TEST_CHECK_EQ_I((hal_u32)st.st_size,
                            84u + count * 50u);

        /* The first facet's normal is unit and the first vertex
         * matches the cube's rotated corner. */
        float facet[12];
        HAL_TEST_CHECK(fread(facet, 4, 12, f) == 12);
        hal_f32 nlen = (hal_f32)sqrt(facet[0] * facet[0] +
                                     facet[1] * facet[1] +
                                     facet[2] * facet[2]);
        HAL_TEST_CHECK_EQ_F(nlen, 1.0f, 1e-3);
        fclose(f);
    }
    hal_scene_destroy(s);
}

int main(void) {
    test_gltf_round_trip();
    test_obj_round_trip();
    test_stl_export();
    HAL_TEST_SUMMARY("test_io");
}
