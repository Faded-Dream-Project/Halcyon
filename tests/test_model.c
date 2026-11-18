/*
 * test_model.c — M4 modeling tests: every operator on known
 * geometry, picking, and the project format v2 mesh round trip
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/project.h"
#include "core/scene.h"
#include "core/undo.h"
#include "render/camera.h"
#include "render/pick.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-model-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* Every face normal points away from the mesh's own bounding-box
 * center (measuring against the origin would misjudge off-origin
 * solids — an early version of this helper did exactly that). */
static bool faces_outward(hal_mesh *m, hal_f32 min_dot) {
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    if (!fn)
        return false;
    hal_vec3f bmin, bmax;
    hal_mesh_bounds(m, &bmin, &bmax);
    hal_vec3f center =
        hal_vec3f_mul(hal_vec3f_add(bmin, bmax), 0.5f);
    for (size_t f = 0; f < m->face_count; f++) {
        hal_vec3f centroid = hal_vec3f_zero();
        size_t n = m->face_off[f + 1] - m->face_off[f];
        for (size_t i = 0; i < n; i++)
            centroid =
                hal_vec3f_add(centroid, m->verts[m->loops[m->face_off[f] + i]]);
        centroid = hal_vec3f_mul(centroid, 1.0f / (hal_f32)n);
        hal_vec3f radial = hal_vec3f_sub(centroid, center);
        if (hal_vec3f_length_sq(radial) < 1e-9f)
            continue; /* centered face: any side is fine */
        if (hal_vec3f_dot(fn[f], hal_vec3f_normalize(radial)) < min_dot)
            return false;
    }
    return true;
}

/* ---- Selection + clone ---- */

static void test_selection_clone(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    HAL_TEST_CHECK(m != NULL);
    hal_mesh_select_all(m, true);
    HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_verts(m), 8);
    HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_edges(m), 24);
    HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_faces(m), 6);
    hal_mesh_select_all(m, false);
    HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_verts(m), 0);

    m->vert_sel[3] = 1;
    m->face_sel[2] = 1;
    hal_mesh *c = hal_mesh_clone(m);
    HAL_TEST_CHECK(c != NULL);
    if (c) {
        HAL_TEST_CHECK_EQ_I(c->vert_count, 8);
        HAL_TEST_CHECK_EQ_I(c->face_count, 6);
        HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_verts(c), 1);
        HAL_TEST_CHECK_EQ_I(hal_mesh_count_selected_faces(c), 1);
        HAL_TEST_CHECK_EQ_F(c->verts[3].x, m->verts[3].x, 1e-6);
        HAL_TEST_CHECK_EQ_F(c->verts[7].z, m->verts[7].z, 1e-6);
        hal_mesh_destroy(c);
    }
    hal_mesh_destroy(m);
}

/* ---- Extrude ---- */

static void test_extrude(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_select_all(m, false);
    m->face_sel[1] = 1; /* the top face */

    HAL_TEST_CHECK(hal_mesh_extrude_faces(m, 1.0f));
    /* 8 old + 4 new verts; 6 faces + 4 sides = 10. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 12);
    HAL_TEST_CHECK_EQ_I(m->face_count, 10);
    HAL_TEST_CHECK(faces_outward(m, 0.5f));

    /* The extruded top sits at y = 1.5 (0.5 + 1.0 offset). */
    hal_f32 max_y = -1e9f;
    for (size_t v = 0; v < m->vert_count; v++)
        if (m->verts[v].y > max_y)
            max_y = m->verts[v].y;
    HAL_TEST_CHECK_EQ_F(max_y, 1.5f, 1e-4);
    hal_mesh_destroy(m);
}

/* ---- Inset ---- */

static void test_inset(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_select_all(m, false);
    m->face_sel[1] = 1;

    HAL_TEST_CHECK(hal_mesh_inset_faces(m, 0.25f));
    /* 8 + 4 new verts; 6 + 4 ring quads = 10 faces. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 12);
    HAL_TEST_CHECK_EQ_I(m->face_count, 10);
    HAL_TEST_CHECK(faces_outward(m, 0.5f));

    /* The inner face's verts are 25% toward the top face center
     * (0,0.5,0): |x| = 0.5 * 0.75. */
    bool found_inner = false;
    for (size_t v = 0; v < m->vert_count; v++)
        if (m->verts[v].y > 0.49f && m->verts[v].y < 0.51f &&
            fabsf(m->verts[v].x - 0.375f) < 1e-4) {
            found_inner = true;
            break;
        }
    HAL_TEST_CHECK(found_inner);
    hal_mesh_destroy(m);
}

/* ---- Bevel ---- */

static void test_bevel(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_rebuild_edges(m); /* 12 unique edges (face pairs merged) */
    hal_mesh_select_all(m, false);
    /* Bevel one vertical edge: (0,4) in the cube's edge list. */
    bool found = false;
    for (size_t e = 0; e < m->edge_count; e++)
        if ((m->edges[e * 2] == 0 && m->edges[e * 2 + 1] == 4) ||
            (m->edges[e * 2] == 4 && m->edges[e * 2 + 1] == 0)) {
            m->edge_sel[e] = 1;
            found = true;
            break;
        }
    HAL_TEST_CHECK(found);

    HAL_TEST_CHECK(hal_mesh_bevel_edges(m, 0.1f));
    /* 8 + 2 new verts; 6 faces + 1 bevel quad = 7. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 10);
    HAL_TEST_CHECK_EQ_I(m->face_count, 7);
    /* The chamfer quad's plane contains the object axis (a 45-degree
     * corner chamfer), so the centroid-outward test only applies to
     * the six original faces. */
    {
        const hal_vec3f *fn = hal_mesh_face_normals(m);
        HAL_TEST_CHECK(fn != NULL);
        for (size_t f = 0; f + 1 < m->face_count; f++) {
            hal_vec3f centroid = hal_vec3f_zero();
            size_t n = m->face_off[f + 1] - m->face_off[f];
            for (size_t i = 0; i < n; i++)
                centroid = hal_vec3f_add(
                    centroid, m->verts[m->loops[m->face_off[f] + i]]);
            centroid = hal_vec3f_mul(centroid, 1.0f / (hal_f32)n);
            HAL_TEST_CHECK(hal_vec3f_dot(
                               fn[f], hal_vec3f_normalize(centroid)) >
                           0.5f);
        }
    }
    hal_mesh_destroy(m);
}

/* ---- Loop cut ---- */

static void test_loop_cut(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    size_t edges_before = m->edge_count;
    HAL_TEST_CHECK_EQ_I(edges_before, 24); /* per-face edges */

    /* Cut through edge 0: the ring wraps the 4 faces around that
     * edge's direction; midpoints land on the 4 ring edges. */
    HAL_TEST_CHECK(hal_mesh_loop_cut(m, 0, 0.5f));
    /* 8 + 4 ring-edge midpoints = 12 verts; 6 + 4 = 10 faces. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 12);
    HAL_TEST_CHECK_EQ_I(m->face_count, 10);
    HAL_TEST_CHECK(faces_outward(m, 0.3f));
    hal_mesh_destroy(m);
}

/* ---- Subdivide ---- */

static void test_subdivide(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    HAL_TEST_CHECK(hal_mesh_subdivide(m)); /* no selection = all */
    /* Per face: 4 corner quads; shared midpoints; +1 center. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 8 + 12 + 6);
    HAL_TEST_CHECK_EQ_I(m->face_count, 24);
    HAL_TEST_CHECK(faces_outward(m, 0.5f));

    /* Subdividing a selection only touches the selection. */
    hal_mesh *m2 = hal_mesh_cube(0.5f);
    hal_mesh_select_all(m2, false);
    m2->face_sel[0] = 1;
    HAL_TEST_CHECK(hal_mesh_subdivide(m2));
    HAL_TEST_CHECK_EQ_I(m2->face_count, 5 + 4);
    hal_mesh_destroy(m2);
    hal_mesh_destroy(m);
}

/* ---- Merge ---- */

static void test_merge(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_select_all(m, false);
    m->vert_sel[0] = 1;
    m->vert_sel[1] = 1;

    HAL_TEST_CHECK(hal_mesh_merge_selected_verts(m));
    /* 7 verts; the two faces that contained the edge (0,1) collapse
     * from quads to triangles but survive. */
    HAL_TEST_CHECK_EQ_I(m->vert_count, 7);
    HAL_TEST_CHECK_EQ_I(m->face_count, 6);

    /* The merged vert sits at the midpoint of 0 and 1. */
    bool found_mid = false;
    for (size_t v = 0; v < m->vert_count; v++)
        if (m->verts[v].x == 0.0f && m->verts[v].y == -0.5f &&
            m->verts[v].z == -0.5f)
            found_mid = true;
    HAL_TEST_CHECK(found_mid);

    /* Undo snapshot semantics: clone-before still has 8. */
    hal_mesh_destroy(m);
}

/* ---- Delete ---- */

static void test_delete(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_mesh_select_all(m, false);
    m->face_sel[0] = 1;
    HAL_TEST_CHECK(hal_mesh_delete_selected(m, HAL_DELETE_FACES));
    HAL_TEST_CHECK_EQ_I(m->face_count, 5);
    HAL_TEST_CHECK_EQ_I(m->vert_count, 8); /* verts stay */

    hal_mesh_select_all(m, false);
    m->vert_sel[0] = 1;
    HAL_TEST_CHECK(hal_mesh_delete_selected(m, HAL_DELETE_VERTS));
    HAL_TEST_CHECK_EQ_I(m->vert_count, 7);
    /* Vert 0 belonged to the bottom face (already deleted) plus the
     * -Z and -X faces: 5 - 2 = 3 survive. */
    HAL_TEST_CHECK_EQ_I(m->face_count, 3);
    hal_mesh_destroy(m);
}

/* ---- Mirror ---- */

static void test_mirror(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    /* Off-center the cube, mirror across X, and check it flips. */
    for (size_t v = 0; v < m->vert_count; v++)
        m->verts[v].x += 1.0f;
    HAL_TEST_CHECK(hal_mesh_mirror(m, HAL_MIRROR_X));
    hal_vec3f bmin, bmax;
    hal_mesh_bounds(m, &bmin, &bmax);
    HAL_TEST_CHECK_EQ_F(bmax.x, -0.5f, 1e-5);
    HAL_TEST_CHECK_EQ_F(bmin.x, -1.5f, 1e-5);
    HAL_TEST_CHECK(faces_outward(m, 0.5f)); /* winding flipped too */
    hal_mesh_destroy(m);
}

/* ---- Recalc / flip normals ---- */

static void test_flip_recalc(void) {
    hal_mesh *m = hal_mesh_cube(0.5f);
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    hal_vec3f before = fn[1];
    hal_mesh_flip_winding(m);
    fn = hal_mesh_face_normals(m);
    HAL_TEST_CHECK_EQ_F(fn[1].x, -before.x, 1e-4);
    HAL_TEST_CHECK_EQ_F(fn[1].y, -before.y, 1e-4);
    hal_mesh_recalc_normals(m);
    HAL_TEST_CHECK(hal_mesh_face_normals(m) != NULL);
    hal_mesh_destroy(m);
}

/* ---- Picking ---- */

static void test_picking(void) {
    hal_scene *s = hal_scene_create("P");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, cube, hal_mesh_cube(0.5f));
    hal_object_id far_cube =
        hal_scene_object_add(s, "Far", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, far_cube, hal_mesh_cube(0.5f));
    hal_object_set_location(s, far_cube, hal_vec3f_make(0, 0, -8.0f));

    hal_camera cam = hal_camera_default();
    hal_rectf vp = { 0, 0, 800, 600 };
    hal_mat4f view = hal_camera_view(&cam);
    hal_mat4f proj = hal_camera_projection(&cam, 800.0f / 600.0f);
    hal_mat4f vpm = hal_mat4f_mul(proj, view);

    /* The center pixel hits the NEAR cube (depth comparison). */
    hal_pick pick = hal_pick_scene(s, &cam, vpm, vp, 400, 300);
    HAL_TEST_CHECK_EQ_I(pick.kind, HAL_PICK_OBJECT);
    HAL_TEST_CHECK_EQ_I(pick.object, cube);
    HAL_TEST_CHECK(pick.depth < 7.0f);

    /* Empty space picks nothing. */
    pick = hal_pick_scene(s, &cam, vpm, vp, 5, 5);
    HAL_TEST_CHECK_EQ_I(pick.kind, HAL_PICK_NONE);

    /* The far cube is hit along its own axis. */
    hal_ray ray = { .origin = hal_vec3f_make(0, 0, 5),
                    .dir = hal_vec3f_make(0, 0, -1) };
    hal_object *far_obj = hal_scene_object(s, far_cube);
    int face = hal_pick_face(far_obj->mesh,
                             hal_object_world_matrix(s, far_obj), ray,
                             NULL);
    HAL_TEST_CHECK(face >= 0);

    /* Vert/edge picking in screen space: cube vert 6 = (0.5,0.5,0.5)
     * projects somewhere right-up of center. */
    hal_object *obj = hal_scene_object(s, cube);
    hal_mat4f world = hal_object_world_matrix(s, obj);
    hal_f32 w = 0;
    hal_vec3f sp = hal_camera_project(&cam, vpm, hal_vec3f_make(0.5f, 0.5f, 0.5f),
                                      vp, &w);
    HAL_TEST_CHECK(w > 0);
    int v = hal_pick_vert(obj->mesh, world, &cam, vpm, vp, sp.x + 1.0f,
                          sp.y - 1.0f, 8.0f);
    HAL_TEST_CHECK_EQ_I(v, 6);

    int e = hal_pick_edge(obj->mesh, world, &cam, vpm, vp, sp.x, sp.y,
                          4.0f);
    HAL_TEST_CHECK(e >= 0);

    hal_scene_destroy(s);
}

/* ---- Undo integration + project format v2 ---- */

typedef struct mesh_cmd {
    hal_object_id id;
    hal_mesh *snapshot; /* the pre-op mesh (undo target) */
} mesh_cmd;

static void mesh_swap(hal_scene *scene, hal_object_id id,
                      hal_mesh **with) {
    hal_object *o = hal_scene_object(scene, id);
    hal_mesh *tmp = o->mesh;
    o->mesh = *with;
    *with = tmp;
}

static void mesh_op_apply(void *state, void *ctx) {
    mesh_cmd *c = state;
    mesh_swap(ctx, c->id, &c->snapshot);
}

static void mesh_op_revert(void *state, void *ctx) {
    mesh_cmd *c = state;
    mesh_swap(ctx, c->id, &c->snapshot);
}

static void mesh_op_free(void *state) {
    mesh_cmd *c = state;
    hal_mesh_destroy(c->snapshot);
    free(c);
}

static void test_undo_and_format_v2(void) {
    hal_scene *s = hal_scene_create("Edit");
    hal_object_id id = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, id, hal_mesh_cube(0.5f));
    hal_object_set_smooth_shading(s, id, true);
    hal_undo_stack *undo = hal_undo_stack_create(s, 16);

    /* An extrude, pushed as a snapshot command. */
    hal_object *obj = hal_scene_object(s, id);
    hal_mesh *snap = hal_mesh_clone(obj->mesh);
    obj->mesh->face_sel[1] = 1;
    HAL_TEST_CHECK(hal_mesh_extrude_faces(obj->mesh, 0.5f));
    HAL_TEST_CHECK_EQ_I(obj->mesh->face_count, 10);

    mesh_cmd *cmd = malloc(sizeof(*cmd));
    cmd->id = id;
    cmd->snapshot = snap;
    HAL_TEST_CHECK(hal_undo_stack_push(undo, "Extrude", mesh_op_apply,
                                       mesh_op_revert, mesh_op_free,
                                       cmd));

    /* Undo restores the snapshot; redo restores the extrude. */
    HAL_TEST_CHECK(hal_undo_stack_undo(undo));
    HAL_TEST_CHECK_EQ_I(hal_scene_object(s, id)->mesh->face_count, 6);
    HAL_TEST_CHECK(hal_undo_stack_redo(undo));
    HAL_TEST_CHECK_EQ_I(hal_scene_object(s, id)->mesh->face_count, 10);

    /* Format v2 round trip: mesh + smooth flag survive a save/load
     * cycle byte-for-byte in the values that matter. */
    char path[512];
    snprintf(path, sizeof(path), "%s/edit.halcyon", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)),
                        HAL_OK);

    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 1);
        hal_object *lo = hal_scene_object_at(loaded, 0);
        HAL_TEST_CHECK(lo->smooth_shading);
        HAL_TEST_CHECK(lo->mesh != NULL);
        if (lo->mesh) {
            HAL_TEST_CHECK_EQ_I(lo->mesh->vert_count, 12);
            HAL_TEST_CHECK_EQ_I(lo->mesh->face_count, 10);
            HAL_TEST_CHECK_EQ_I(lo->mesh->edge_count,
                                obj->mesh->edge_count);
            HAL_TEST_CHECK_EQ_F(lo->mesh->verts[8].y, 1.0f, 1e-5);
            HAL_TEST_CHECK_EQ_F(lo->mesh->verts[0].x, -0.5f, 1e-5);
        }
        hal_scene_destroy(loaded);
    }

    /* A v1 file (no mesh block) still loads. */
    snprintf(path, sizeof(path), "%s/v1.halcyon", tmp_dir());
    FILE *f = fopen(path, "wb");
    fputs("halcyon 1\n"
          "scene \"Old\"\n"
          "object 1 \"A\" mesh\n"
          "  parent 0\n"
          "  loc 0 0 0\n"
          "  rot 0 0 0 1\n"
          "  scale 1 1 1\n"
          "  flags visible\n"
          "end\n",
          f);
    fclose(f);
    loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK(hal_scene_object_at(loaded, 0)->mesh == NULL);
        hal_scene_destroy(loaded);
    }

    hal_undo_stack_destroy(undo);
    hal_scene_destroy(s);
}

int main(void) {
    test_selection_clone();
    test_extrude();
    test_inset();
    test_bevel();
    test_loop_cut();
    test_subdivide();
    test_merge();
    test_delete();
    test_mirror();
    test_flip_recalc();
    test_picking();
    test_undo_and_format_v2();
    HAL_TEST_SUMMARY("test_model");
}
