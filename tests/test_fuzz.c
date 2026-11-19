/*
 * test_fuzz.c — parser robustness (fuzz) + large-scene stress
 *
 * Deterministic (fixed seed): random bytes, truncations of valid
 * files, and targeted mutations must NEVER crash the project, OBJ
 * and glTF loaders — only clean error returns. Run under the usual
 * ASan+UBSan build. Also: deep hierarchies, absurd declared counts,
 * and a big-scene save/load/render/hull round trip.
 */

#include "hal_test.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/project.h"
#include "core/scene.h"
#include "io/io.h"
#include "render/camera.h"
#include "render/raster.h"
#include "render/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-fuzz-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* ---- A tiny deterministic PRNG (splitmix64) ---- */

static hal_u64 rng_state = 0x9e3779b97f4a7c15ull;

static hal_u64 rng_next(void) {
    rng_state += 0x9e3779b97f4a7c15ull;
    hal_u64 z = rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static hal_u32 rng_below(hal_u32 n) {
    return n ? (hal_u32)(rng_next() % (hal_u64)n) : 0;
}

/* ---- Feed a file to every loader; no crash = pass ---- */

static void feed_loaders(const char *path, int *clean_rejects,
                         int *accepted) {
    char err[256];
    hal_scene *s = NULL;

    err[0] = 0;
    hal_result r = hal_project_load(path, &s, err, sizeof(err));
    if (r == HAL_OK && s) {
        (*accepted)++;
        hal_scene_destroy(s);
    } else {
        (*clean_rejects)++;
    }

    s = NULL;
    err[0] = 0;
    r = hal_io_obj_import(path, &s, err, sizeof(err));
    if (r == HAL_OK && s) {
        (*accepted)++;
        hal_scene_destroy(s);
    } else {
        (*clean_rejects)++;
    }

    s = NULL;
    err[0] = 0;
    r = hal_io_gltf_import(path, &s, err, sizeof(err));
    if (r == HAL_OK && s) {
        (*accepted)++;
        hal_scene_destroy(s);
    } else {
        (*clean_rejects)++;
    }
}

static bool write_bytes(const char *path, const unsigned char *data,
                        size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* Build one valid file of each format (small scenes). */
static void make_valid_files(char *proj, size_t proj_cap,
                             char *obj, size_t obj_cap,
                             char *glb, size_t glb_cap) {
    snprintf(proj, proj_cap, "%s/valid.halcyon", tmp_dir());
    snprintf(obj, obj_cap, "%s/valid.obj", tmp_dir());
    snprintf(glb, glb_cap, "%s/valid.glb", tmp_dir());

    hal_scene *s = hal_scene_create("F");
    hal_object_id id =
        hal_scene_object_add(s, "Box", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, id, hal_mesh_cube(0.5f));
    hal_material m = hal_material_default();
    hal_object_set_material(
        s, id, hal_scene_material_add(s, &m));
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(
        hal_project_save(s, proj, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK_EQ_I(hal_io_obj_export(s, obj, err,
                                          sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_export(s, glb, err, sizeof(err)), HAL_OK);
    hal_scene_destroy(s);
}

static void read_file(const char *path, unsigned char **out,
                      size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }
    *out = malloc((size_t)sz);
    if (*out && fread(*out, 1, (size_t)sz, f) == (size_t)sz)
        *out_len = (size_t)sz;
    else {
        free(*out);
        *out = NULL;
    }
    fclose(f);
}

static void test_fuzz_random_and_truncated(void) {
    char proj[512], objf[512], glb[512];
    make_valid_files(proj, sizeof(proj), objf, sizeof(objf), glb,
                     sizeof(glb));

    unsigned char *valid[3] = { NULL, NULL, NULL };
    size_t valid_len[3] = { 0, 0, 0 };
    read_file(proj, &valid[0], &valid_len[0]);
    read_file(objf, &valid[1], &valid_len[1]);
    read_file(glb, &valid[2], &valid_len[2]);
    HAL_TEST_CHECK(valid[0] && valid[1] && valid[2]);

    char path[512];
    unsigned char buf[8192];
    int clean_rejects = 0, accepted = 0;

    /* 1. Pure random bytes (240 cases x 3 loaders). */
    for (int i = 0; i < 240; i++) {
        size_t len = 1 + rng_below(400);
        for (size_t b = 0; b < len; b++)
            buf[b] = (unsigned char)rng_next();
        snprintf(path, sizeof(path), "%s/rnd", tmp_dir());
        HAL_TEST_CHECK(write_bytes(path, buf, len));
        feed_loaders(path, &clean_rejects, &accepted);
    }

    /* 2. Every truncation of the valid files (step 7, bounded). */
    for (int k = 0; k < 3; k++) {
        if (!valid[k])
            continue;
        for (size_t len = 0; len < valid_len[k];
             len += 1 + valid_len[k] / 64) {
            snprintf(path, sizeof(path), "%s/trunc", tmp_dir());
            HAL_TEST_CHECK(
                write_bytes(path, valid[k], len));
            feed_loaders(path, &clean_rejects, &accepted);
        }
    }

    /* 3. Single/multi-byte mutations of the valid files (300). */
    for (int i = 0; i < 300; i++) {
        int k = (int)rng_below(3);
        if (!valid[k] || valid_len[k] == 0)
            continue;
        memcpy(buf, valid[k],
               valid_len[k] < sizeof(buf) ? valid_len[k]
                                          : sizeof(buf));
        size_t len = valid_len[k] < sizeof(buf)
                         ? valid_len[k]
                         : sizeof(buf);
        int flips = 1 + (int)rng_below(8);
        for (int m = 0; m < flips; m++)
            buf[rng_below((hal_u32)len)] ^=
                (unsigned char)(1 + rng_below(255));
        snprintf(path, sizeof(path), "%s/mut", tmp_dir());
        HAL_TEST_CHECK(write_bytes(path, buf, len));
        feed_loaders(path, &clean_rejects, &accepted);
    }

    /* 4. GLB-specific structural garbage: valid magic, absurd
     * lengths and chunk types. */
    for (int i = 0; i < 120; i++) {
        memset(buf, 0, 64);
        memcpy(buf, "glTF", 4);
        buf[4] = 2;
        hal_u32 jl = rng_below(100000);
        hal_u32 jt = rng_below(2) ? 0x4E4F534Au
                              : (hal_u32)rng_next();
        memcpy(buf + 12, &jl, 4);
        memcpy(buf + 16, &jt, 4);
        size_t len = 20 + rng_below(200);
        for (size_t b = 20; b < len && b < sizeof(buf); b++)
            buf[b] = (unsigned char)rng_next();
        snprintf(path, sizeof(path), "%s/glbjunk", tmp_dir());
        HAL_TEST_CHECK(write_bytes(path, buf, len));
        feed_loaders(path, &clean_rejects, &accepted);
    }

    /* Sanity: the machinery actually exercised rejections (a fuzzer
     * that never rejects proves nothing) and the valid files DID
     * load (feed each once through the same path). */
    HAL_TEST_CHECK(clean_rejects > 500);
    feed_loaders(proj, &clean_rejects, &accepted);
    feed_loaders(objf, &clean_rejects, &accepted);
    feed_loaders(glb, &clean_rejects, &accepted);
    HAL_TEST_CHECK(accepted >= 9); /* 3 formats x 3 valid feeds */

    free(valid[0]);
    free(valid[1]);
    free(valid[2]);
}

/* Absurd declared counts in the text formats must be rejected
 * without allocating. */
static void test_hostile_counts(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/huge.halcyon", tmp_dir());
    FILE *f = fopen(path, "w");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        fputs("halcyon 3\nscene \"H\"\nobject 1 \"A\" mesh\n"
              "  mesh 4000000000 0 0\n",
              f);
        fclose(f);
    }
    hal_scene *s = NULL;
    char err[256] = { 0 };
    HAL_TEST_CHECK(hal_project_load(path, &s, err, sizeof(err)) !=
                   HAL_OK);
    HAL_TEST_CHECK(s == NULL);

    /* A negative/huge OBJ face index. */
    snprintf(path, sizeof(path), "%s/huge.obj", tmp_dir());
    f = fopen(path, "w");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 999999999\n", f);
        fclose(f);
    }
    s = NULL;
    HAL_TEST_CHECK(
        hal_io_obj_import(path, &s, err, sizeof(err)) != HAL_OK ||
        s == NULL);
    hal_scene_destroy(s);

    /* A 1000-deep parenting chain: loads, and world matrices
     * resolve without blowing the stack. */
    snprintf(path, sizeof(path), "%s/deep.halcyon", tmp_dir());
    f = fopen(path, "w");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        fputs("halcyon 3\nscene \"D\"\n", f);
        for (int i = 1; i <= 1000; i++) {
            fprintf(f, "object %d \"N%d\" mesh\n", i, i);
            if (i > 1)
                fprintf(f, "  parent %d\n", i - 1);
            fputs("  loc 0.1 0 0\n", f);
            fputs("  end\n", f);
        }
        fclose(f);
    }
    s = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_project_load(path, &s, err, sizeof(err)), HAL_OK);
    if (s) {
        hal_object *last =
            hal_scene_object_at(s, hal_scene_object_count(s) - 1);
        hal_mat4f w = hal_object_world_matrix(s, last);
        /* 1000 x 0.1 along X. */
        hal_vec3f p = hal_mat4f_transform_point(w,
                                                 hal_vec3f_zero());
        HAL_TEST_CHECK_EQ_F(p.x, 100.0f, 0.5f);
        hal_scene_destroy(s);
    }
}

/* Large-scene stress: 200 subdivided cubes, saved, loaded, rendered,
 * hulled — under ASan/UBSan with a bounded workload. */
static void test_large_scene_stress(void) {
    hal_scene *s = hal_scene_create("Big");
    for (int i = 0; i < 200; i++) {
        hal_object_id id =
            hal_scene_object_add(s, "C", HAL_OBJECT_MESH);
        hal_mesh *m = hal_mesh_cube(0.5f);
        hal_mesh_subdivide(m);
        hal_mesh_subdivide(m); /* 96 quads = 192 tris each */
        hal_object_set_mesh(s, id, m);
        hal_object_set_location(
            s, id,
            hal_vec3f_make((hal_f32)(i % 15), (hal_f32)(i / 15),
                           0.0f));
    }
    size_t total_verts = 0;
    for (size_t i = 0; i < hal_scene_object_count(s); i++)
        total_verts +=
            hal_scene_object_at(s, i)->mesh->vert_count;
    /* 200 cubes x 2 subdivides = ~24k verts. */
    HAL_TEST_CHECK(total_verts > 15000);

    char path[512];
    char err[256] = { 0 };
    snprintf(path, sizeof(path), "%s/big.halcyon", tmp_dir());
    HAL_TEST_CHECK_EQ_I(
        hal_project_save(s, path, err, sizeof(err)), HAL_OK);

    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_project_load(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(
            (int)hal_scene_object_count(loaded),
            (int)hal_scene_object_count(s));
        hal_scene_destroy(loaded);
    }

    /* One render pass through the rasterizer. */
    hal_camera cam = hal_camera_default();
    cam.distance = 30.0f;
    hal_raster raster = { 0 };
    hal_surface *surf = NULL;
    if (hal_ok(hal_surface_offscreen_create(320, 240, &surf))) {
        hal_canvas canvas;
        hal_canvas_init(&canvas, surf, 1.0f);
        hal_render_opts opts = hal_render_opts_default();
        opts.draw_gizmo = false;
        hal_rectf vp = { 0, 0, 320, 240 };
        hal_render_view(&raster, s, &cam, &canvas, vp, &opts);
        hal_raster_end(&raster);
        hal_surface_destroy(surf);
    }

    /* A hull over one object (the quickhull under ASan). */
    hal_object *o = hal_scene_object_at(s, 0);
    hal_mesh *hull = hal_mesh_convex_hull(o->mesh);
    HAL_TEST_CHECK(hull != NULL);
    hal_mesh_destroy(hull);
    hal_scene_destroy(s);
}

int main(void) {
    test_fuzz_random_and_truncated();
    test_hostile_counts();
    test_large_scene_stress();
    HAL_TEST_SUMMARY("test_fuzz");
}
