/*
 * test_gameflow.c — M9 game-asset workflow: LOD decimation, glTF
 * units/axis export options, and the box-collider pipeline
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
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
        snprintf(dir, sizeof(dir), "/tmp/halcyon-gameflow-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* No face has consecutive duplicate corners or fewer than 3. */
static bool mesh_faces_valid(const hal_mesh *m) {
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = m->face_off[f + 1] - m->face_off[f];
        if (n < 3)
            return false;
        for (size_t i = 0; i < n; i++) {
            hal_u32 a = m->loops[m->face_off[f] + i];
            hal_u32 b = m->loops[m->face_off[f] + (i + 1) % n];
            if (a == b)
                return false;
            if (a >= m->vert_count || b >= m->vert_count)
                return false;
        }
    }
    return true;
}

static void test_decimate(void) {
    /* A sphere decimated to a coarse lattice: strictly fewer verts,
     * valid faces. */
    hal_mesh *m = hal_mesh_sphere(1.0f, 24, 32);
    size_t before = m->vert_count;
    HAL_TEST_CHECK(hal_mesh_decimate_cluster(m, 0.45f));
    HAL_TEST_CHECK(m->vert_count < before);
    HAL_TEST_CHECK(m->vert_count >= 4);
    HAL_TEST_CHECK(mesh_faces_valid(m));

    /* A huge cell collapses everything into one cluster: no faces
     * survive, and the mesh stays consistent (no crash, no loops
     * referencing missing verts). The cube is offset AWAY from the
     * origin first — lattice cells split at 0, and a mesh straddling
     * a cell boundary never merges (a real property of clustering,
     * not a bug). */
    hal_mesh *c = hal_mesh_cube(0.5f);
    for (size_t v = 0; v < c->vert_count; v++)
        c->verts[v] = hal_vec3f_add(c->verts[v],
                                    hal_vec3f_make(1050, 1050, 1050));
    HAL_TEST_CHECK(hal_mesh_decimate_cluster(c, 100.0f));
    HAL_TEST_CHECK_EQ_I(c->vert_count, 1);
    HAL_TEST_CHECK_EQ_I(c->face_count, 0);
    HAL_TEST_CHECK(mesh_faces_valid(c));
    hal_mesh_destroy(c);

    /* Tiny cell: nothing changes. */
    hal_mesh *p = hal_mesh_cube(0.5f);
    HAL_TEST_CHECK(!hal_mesh_decimate_cluster(p, 1e-9f));
    HAL_TEST_CHECK_EQ_I(p->vert_count, 8);
    hal_mesh_destroy(p);
    hal_mesh_destroy(m);
}

static void test_export_options(void) {
    /* A root cube at (0,0,1), a child at local (0,1,0). */
    hal_scene *s = hal_scene_create("O");
    hal_object_id root =
        hal_scene_object_add(s, "Root", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, root, hal_mesh_cube(0.3f));
    hal_object_set_location(s, root, hal_vec3f_make(0, 0, 1));
    hal_object_id child =
        hal_scene_object_add(s, "Child", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, child, hal_mesh_cube(0.2f));
    hal_object_set_parent(s, child, root);
    hal_object_set_location(s, child, hal_vec3f_make(0, 1, 0));

    char path[512];
    char err[256] = { 0 };

    /* Z-up + centimeters: roots absorb rotX(+90) and x100. */
    hal_gltf_export_opts opts = hal_gltf_export_opts_default();
    opts.z_up = true;
    opts.unit_scale = 100.0f;
    snprintf(path, sizeof(path), "%s/zup.glb", tmp_dir());
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_export_ex(s, path, &opts, err, sizeof(err)),
        HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        /* Find the imported objects (flat, world-decomposed). */
        hal_object *lo_root = NULL, *lo_child = NULL;
        for (size_t i = 0; i < hal_scene_object_count(loaded); i++) {
            hal_object *o = hal_scene_object_at(loaded, i);
            if (strcmp(o->name, "Root") == 0)
                lo_root = o;
            if (strcmp(o->name, "Child") == 0)
                lo_child = o;
        }
        HAL_TEST_CHECK(lo_root && lo_child);
        if (lo_root && lo_child) {
            /* Root: (0,0,1) -> rotX90 -> (0,-1,0) -> x100. */
            hal_vec3f want = hal_vec3f_make(0.0f, -100.0f, 0.0f);
            HAL_TEST_CHECK(hal_vec3f_distance(lo_root->location,
                                              want) < 1e-3);
            /* Child world: (0,1,1) -> rotX90 -> (0,-1,1) -> x100;
             * the importer flattens world transforms. */
            hal_mat4f w = hal_object_world_matrix(loaded, lo_child);
            hal_vec3f want_c = hal_vec3f_make(0.0f, -100.0f, 100.0f);
            hal_vec3f got_c = hal_mat4f_transform_point(
                w, hal_vec3f_zero());
            HAL_TEST_CHECK(hal_vec3f_distance(got_c, want_c) < 0.2);
        }
        hal_scene_destroy(loaded);
    }

    /* Defaults unchanged: Y-up meters. */
    snprintf(path, sizeof(path), "%s/def.glb", tmp_dir());
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_export_ex(s, path, NULL, err, sizeof(err)),
        HAL_OK);
    loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    if (loaded) {
        for (size_t i = 0; i < hal_scene_object_count(loaded); i++) {
            hal_object *o = hal_scene_object_at(loaded, i);
            if (strcmp(o->name, "Root") == 0)
                HAL_TEST_CHECK(hal_vec3f_distance(
                                   o->location,
                                   hal_vec3f_make(0, 0, 1)) < 1e-4);
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

static void test_collider_pipeline(void) {
    /* The collider flow the shell uses: an object's world AABB
     * becomes a box object, exported and re-imported — the imported
     * box's world AABB must match the source's. */
    hal_scene *src = hal_scene_create("Src");
    hal_object_id id =
        hal_scene_object_add(src, "Prop", HAL_OBJECT_MESH);
    hal_object_set_mesh(src, id, hal_mesh_cube(0.5f));
    hal_object_set_scale(src, id, hal_vec3f_make(2.0f, 1.0f, 3.0f));
    hal_object_set_location(src, id, hal_vec3f_make(1.0f, 0.5f, -2.0f));
    hal_object *obj = hal_scene_object(src, id);
    hal_mat4f world = hal_object_world_matrix(src, obj);
    hal_vec3f bmin = hal_vec3f_make(1e30f, 1e30f, 1e30f);
    hal_vec3f bmax = hal_vec3f_make(-1e30f, -1e30f, -1e30f);
    for (size_t v = 0; v < obj->mesh->vert_count; v++) {
        hal_vec3f p =
            hal_mat4f_transform_point(world, obj->mesh->verts[v]);
        bmin.x = fminf(bmin.x, p.x);
        bmin.y = fminf(bmin.y, p.y);
        bmin.z = fminf(bmin.z, p.z);
        bmax.x = fmaxf(bmax.x, p.x);
        bmax.y = fmaxf(bmax.y, p.y);
        bmax.z = fmaxf(bmax.z, p.z);
    }

    /* The collider scene: a cube with the AABB's size/location. */
    hal_scene *col = hal_scene_create("Collider");
    hal_object_id cid =
        hal_scene_object_add(col, "Prop", HAL_OBJECT_MESH);
    hal_object_set_mesh(col, cid, hal_mesh_cube(0.5f));
    hal_object_set_scale(
        col, cid,
        hal_vec3f_make(bmax.x - bmin.x, bmax.y - bmin.y,
                       bmax.z - bmin.z));
    hal_object_set_location(
        col, cid, hal_vec3f_mul(hal_vec3f_add(bmin, bmax), 0.5f));

    char path[512];
    char err[256] = { 0 };
    snprintf(path, sizeof(path), "%s/collider.glb", tmp_dir());
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_export(col, path, err, sizeof(err)), HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 1);
        hal_object *lo = hal_scene_object_at(loaded, 0);
        HAL_TEST_CHECK(lo && lo->mesh);
        if (lo && lo->mesh) {
            hal_mat4f w = hal_object_world_matrix(loaded, lo);
            hal_vec3f cmin = hal_vec3f_make(1e30f, 1e30f, 1e30f);
            hal_vec3f cmax = hal_vec3f_make(-1e30f, -1e30f, -1e30f);
            for (size_t v = 0; v < lo->mesh->vert_count; v++) {
                hal_vec3f p = hal_mat4f_transform_point(
                    w, lo->mesh->verts[v]);
                cmin.x = fminf(cmin.x, p.x);
                cmin.y = fminf(cmin.y, p.y);
                cmin.z = fminf(cmin.z, p.z);
                cmax.x = fmaxf(cmax.x, p.x);
                cmax.y = fmaxf(cmax.y, p.y);
                cmax.z = fmaxf(cmax.z, p.z);
            }
            HAL_TEST_CHECK(hal_vec3f_distance(cmin, bmin) < 1e-3);
            HAL_TEST_CHECK(hal_vec3f_distance(cmax, bmax) < 1e-3);
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(col);
    hal_scene_destroy(src);
}

int main(void) {
    test_decimate();
    test_export_options();
    test_collider_pipeline();
    HAL_TEST_SUMMARY("test_gameflow");
}
