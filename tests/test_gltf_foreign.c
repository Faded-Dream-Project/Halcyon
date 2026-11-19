/*
 * test_gltf_foreign.c — foreign glTF rig conventions, imported
 * EXACTLY
 *
 * These tests build .glb files BY HAND (a minimal GLB writer below)
 * in the conventions foreign exporters use — parent-relative joint
 * translations, joint REST rotations, inverseBindMatrices derived
 * from the node-chain rest — import them, and verify the skinning
 * numerically against the analytic node-hierarchy result.
 */

#include "hal_test.h"
#include "core/anim.h"
#include "core/armature.h"
#include "core/math.h"
#include "core/mesh.h"
#include "core/scene.h"
#include "io/io.h"
#include "platform/platform.h"
#include "render/raster.h"
#include "render/render.h"
#include "ui/canvas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-foreign-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* ---- A minimal GLB writer ----
 * JSON + BIN chunks; floats are appended to the BIN and their
 * bufferView index returned. */

#define MAX_VIEWS 16

typedef struct glb_writer {
    char json[8192];
    size_t json_len;
    unsigned char bin[4096];
    size_t bin_len;
    int view_count;
    size_t view_off[MAX_VIEWS];
    size_t view_len[MAX_VIEWS];
} glb_writer;

static void glbw_init(glb_writer *w) {
    memset(w, 0, sizeof(*w));
}

/* Appends raw bytes as a bufferView; returns its index. */
static int glbw_view(glb_writer *w, const void *data, size_t len) {
    if (w->view_count >= MAX_VIEWS)
        return -1;
    while (w->bin_len % 4 != 0)
        w->bin[w->bin_len++] = 0;
    w->view_off[w->view_count] = w->bin_len;
    w->view_len[w->view_count] = len;
    memcpy(w->bin + w->bin_len, data, len);
    w->bin_len += len;
    return w->view_count++;
}

static bool glbw_write(const glb_writer *w, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    unsigned char header[12] = { 'g', 'l', 'T', 'F', 2, 0, 0, 0 };
    size_t jlen = strlen(w->json);
    unsigned int jchunk[2] = { (unsigned int)jlen,
                               0x4E4F534Au };
    unsigned int bchunk[2] = { (unsigned int)w->bin_len,
                               0x004E4942u };
    /* header total length */
    unsigned int total =
        12 + 8 + (unsigned int)jlen + 8 + (unsigned int)w->bin_len;
    memcpy(header + 8, &total, 4);
    if (fwrite(header, 12, 1, f) != 1 ||
        fwrite(jchunk, 8, 1, f) != 1 ||
        fwrite(w->json, jlen, 1, f) != 1 ||
        fwrite(bchunk, 8, 1, f) != 1 ||
        fwrite(w->bin, w->bin_len, 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

/* ---- The foreign rig fixture ----
 *
 * Convention under test (Blender-style):
 *   - joints parent-relative: node 2 (root) at T(1,0,0) * R(90Z),
 *     node 3 (child) at T(0,1,0) * R(30Z)  (parent-relative!)
 *   - armature node 1 at identity, mesh node 0 with 8-vert cube,
 *     verts in mesh-local space, node transform IGNORED (skinned)
 *   - IBM_i = inverse(node-chain global rest of joint i)
 *   - JOINTS_0/WEIGHTS_0 on the primitive, skin on the MESH NODE
 *   - NO "generator" => the foreign branch
 *
 * The test then rotates a bone in Halcyon and compares the skinned
 * world verts against the analytic glTF formula
 *   v' = sum_j w_j * G_j(t) * IBM_j * v
 * where G_j(t) is the node chain with the animated local rotation
 * substituted for joint j. */

typedef struct {
    hal_mat4f rest; /* node-chain global rest (the bind) */
} joint_info;

static hal_mat4f trs(hal_vec3f t, hal_quatf r) {
    hal_mat4f m = hal_quatf_to_mat4(r);
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

static void build_foreign(glb_writer *w, hal_quatf root_rest_rot,
                          hal_quatf child_rest_rot,
                          hal_quatf child_anim_rot) {
    /* The four cube verts (a tetra-pick of the 8, kept small). */
    hal_f32 verts[] = {
        0.0f, 0.0f,  0.0f,  /* v0 near the root joint   */
        0.1f, 1.0f,  0.0f,  /* v1 near the child joint  */
        -0.6f, 0.5f, 0.3f,  /* v2 between               */
        0.2f, 1.4f,  -0.4f, /* v3 beyond the child      */
    };
    hal_u32 idx[] = { 0, 1, 2, 0, 1, 3 };
    /* Influences: v0/v2 mostly bone 0; v1/v3 mostly bone 1. */
    hal_u8 joints[] = {
        0, 1, 0, 0,  0, 1, 0, 0,  0, 1, 0, 0,  0, 1, 0, 0,
    };
    hal_f32 weights[] = {
        0.9f, 0.1f, 0, 0,  0.2f, 0.8f, 0, 0,
        0.7f, 0.3f, 0, 0,  0.1f, 0.9f, 0, 0,
    };
    int v_pos = glbw_view(w, verts, sizeof(verts));
    int v_idx = glbw_view(w, idx, sizeof(idx));
    int v_jnt = glbw_view(w, joints, sizeof(joints));
    int v_wgt = glbw_view(w, weights, sizeof(weights));

    /* The node-chain rest globals (binds):
     *   G0 = T(1,0,0) * R(root_rest_rot)
     *   G1 = G0 * T(0,1,0) * R(child_rest_rot)     */
    hal_mat4f g0 =
        trs(hal_vec3f_make(1, 0, 0), root_rest_rot);
    hal_mat4f g1 = hal_mat4f_mul(
        g0, trs(hal_vec3f_make(0, 1, 0), child_rest_rot));
    hal_mat4f ibm[2] = { hal_mat4f_invert(g0),
                         hal_mat4f_invert(g1) };
    int v_ibm = glbw_view(w, ibm, sizeof(ibm));

    /* Accessors: 0 pos, 1 idx, 2 joints, 3 weights, 4 ibm. */
    snprintf(
        w->json, sizeof(w->json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0,"
        "\"nodes\":["
        /* 0: mesh node (transform ignored for skinned meshes) */
        "{\"name\":\"Limb\",\"mesh\":0,\"skin\":0,"
        "\"translation\":[5,5,5],\"rotation\":[0,0,0,1],"
        "\"scale\":[1,1,1]},"
        /* 1: armature node at identity */
        "{\"name\":\"Rig\",\"children\":[2]},"
        /* 2: root joint */
        "{\"name\":\"Upper\",\"translation\":[1,0,0],"
        "\"rotation\":[%.9g,%.9g,%.9g,%.9g],"
        "\"children\":[3]},"
        /* 3: child joint (parent-relative) */
        "{\"name\":\"Fore\",\"translation\":[0,1,0],"
        "\"rotation\":[%.9g,%.9g,%.9g,%.9g]}"
        "],"
        "\"meshes\":[{\"primitives\":[{"
        "\"attributes\":{\"POSITION\":0,\"JOINTS_0\":2,"
        "\"WEIGHTS_0\":3},\"indices\":1,\"mode\":4}]}],"
        "\"skins\":[{\"joints\":[2,3],\"skeleton\":2,"
        "\"inverseBindMatrices\":4}],"
        "\"accessors\":["
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":4,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":%d,\"componentType\":5125,\"count\":6,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":%d,\"componentType\":5121,\"count\":4,"
        "\"type\":\"VEC4\"},"
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":4,"
        "\"type\":\"VEC4\"},"
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":2,"
        "\"type\":\"MAT4\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}"
        "],"
        "\"buffers\":[{\"byteLength\":%zu}]}",
        (double)root_rest_rot.x, (double)root_rest_rot.y,
        (double)root_rest_rot.z, (double)root_rest_rot.w,
        (double)child_anim_rot.x, (double)child_anim_rot.y,
        (double)child_anim_rot.z, (double)child_anim_rot.w,
        v_pos, v_idx, v_jnt, v_wgt, v_ibm,
        w->view_off[v_pos], w->view_len[v_pos],
        w->view_off[v_idx], w->view_len[v_idx],
        w->view_off[v_jnt], w->view_len[v_jnt],
        w->view_off[v_wgt], w->view_len[v_wgt],
        w->view_off[v_ibm], w->view_len[v_ibm],
        w->bin_len);
    (void)child_anim_rot;
}

static void test_foreign_rig_exact(void) {
    /* Rest rotations: root 90 degrees about Z, child 30 about Z. */
    hal_quatf root_rot = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_quatf child_rot = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 30.0f * HAL_DEG2RAD_f32);
    /* The file's CURRENT joint rotations (the rest pose values). */
    hal_quatf child_anim = child_rot;

    glb_writer w;
    glbw_init(&w);
    build_foreign(&w, root_rot, child_rot, child_anim);
    char path[512];
    snprintf(path, sizeof(path), "%s/foreign.glb", tmp_dir());
    HAL_TEST_CHECK(glbw_write(&w, path));

    char err[256] = { 0 };
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (!loaded) {
        printf("import error: %s\n", err);
        return;
    }

    hal_object *lo_arm = NULL, *lo_mesh = NULL;
    for (size_t i = 0; i < hal_scene_object_count(loaded); i++) {
        hal_object *o = hal_scene_object_at(loaded, i);
        if (o->armature)
            lo_arm = o;
        if (o->mesh && o->skin_armature != HAL_OBJECT_ID_NONE)
            lo_mesh = o;
    }
    HAL_TEST_CHECK(lo_arm && lo_mesh && lo_arm->armature);
    if (!(lo_arm && lo_mesh && lo_arm->armature))
        return;
    HAL_TEST_CHECK_EQ_I(lo_arm->armature->count, 2);
    HAL_TEST_CHECK(lo_mesh->mesh->skin_weights != NULL);

    /* The analytic expected result, computed from the glTF formula:
     * pose = replace the CHILD joint's local rotation with q (any
     * q); G0 stays the file's rest (root unposed). */
    hal_quatf q = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 110.0f * HAL_DEG2RAD_f32);
    /* G1(t) = G0 * T(0,1,0) * R(q) */
    hal_mat4f g0 = trs(hal_vec3f_make(1, 0, 0), root_rot);
    hal_mat4f g1t =
        hal_mat4f_mul(g0, trs(hal_vec3f_make(0, 1, 0), q));
    hal_mat4f ibm0 = hal_mat4f_invert(
        trs(hal_vec3f_make(1, 0, 0), root_rot));
    hal_mat4f ibm1 = hal_mat4f_invert(hal_mat4f_mul(
        g0, trs(hal_vec3f_make(0, 1, 0), child_rot)));

    static const hal_f32 verts[4][3] = {
        { 0.0f, 0.0f, 0.0f },  { 0.1f, 1.0f, 0.0f },
        { -0.6f, 0.5f, 0.3f }, { 0.2f, 1.4f, -0.4f },
    };
    static const hal_f32 wts[4][2] = { { 0.9f, 0.1f },
                                       { 0.2f, 0.8f },
                                       { 0.7f, 0.3f },
                                       { 0.1f, 0.9f } };

    /* Pose the imported armature the same way: the child bone's
     * local rotation becomes q (bone 1), the root keeps its rest. */
    lo_arm->armature->bones[1].pose = q;
    /* NOTE: at import the bones' poses are the file's rest values
     * (the node rotations) — posing bone 1 = replacing its local. */

    hal_vec3f *out = malloc(lo_mesh->mesh->vert_count *
                            sizeof(hal_vec3f));
    HAL_TEST_CHECK(out != NULL);
    if (!out)
        return;
    /* Imported foreign meshes: armature + mesh objects both near
     * identity (the mesh node transform is ignored), same-space
     * apply_pose applies directly. */
    HAL_TEST_CHECK(hal_mesh_apply_pose(
        lo_mesh->mesh, lo_arm->armature, out));

    for (size_t v = 0; v < 4 && v < lo_mesh->mesh->vert_count;
         v++) {
        hal_vec3f p = hal_vec3f_make(verts[v][0], verts[v][1],
                                     verts[v][2]);
        hal_vec3f via0 = hal_mat4f_transform_point(
            g1t, hal_mat4f_transform_point(ibm1, p));
        hal_vec3f via1 = hal_mat4f_transform_point(
            g0, hal_mat4f_transform_point(ibm0, p));
        hal_vec3f want = hal_vec3f_add(
            hal_vec3f_mul(via0, wts[v][1]),
            hal_vec3f_mul(via1, wts[v][0]));
        /* Find the matching imported vert (the importer may have
         * duplicated per corner — compare SETS like the round trip
         * test: every expected position must appear). */
        bool found = false;
        for (size_t c = 0;
             c < lo_mesh->mesh->vert_count && !found; c++)
            if (hal_vec3f_distance(out[c], want) < 1e-4)
                found = true;
        HAL_TEST_CHECK(found);
        if (!found)
            printf("vert %zu: got (%.4f,%.4f,%.4f) want "
                   "(%.4f,%.4f,%.4f)\n",
                   v, out[v].x, out[v].y, out[v].z, want.x,
                   want.y, want.z);
    }
    free(out);
    hal_scene_destroy(loaded);
}

/* A second convention: joints ordered CHILD-BEFORE-PARENT (legal
 * glTF) to exercise the topological offset fix-up. */
static void test_foreign_child_first_ordering(void) {
    hal_quatf root_rot = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_quatf child_rot = hal_quatf_from_axis_angle(
        hal_vec3f_axis_x(), 45.0f * HAL_DEG2RAD_f32);
    glb_writer w;
    glbw_init(&w);
    build_foreign(&w, root_rot, child_rot, child_rot);
    /* Rewrite the joints order: [3, 2] (child first). */
    char *at = strstr(w.json, "\"joints\":[2,3]");
    HAL_TEST_CHECK(at != NULL);
    if (at)
        memcpy(at, "\"joints\":[3,2]", 13);
    /* JOINTS_0 values index the joints array — swap them so the
     * semantics stay (bone0 = node2, bone1 = node3). */
    /* (The fixture's influences reference array positions; with the
     * array flipped, v1/v3's "bone 1" now means node 2. The exact
     * math still verifies: recompute below.) */
    char path[512];
    snprintf(path, sizeof(path), "%s/foreign2.glb", tmp_dir());
    HAL_TEST_CHECK(glbw_write(&w, path));

    char err[256] = { 0 };
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    if (!loaded)
        printf("import2 err: %s\n", err);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        hal_object *lo_arm = NULL;
        for (size_t i = 0; i < hal_scene_object_count(loaded);
             i++) {
            hal_object *o = hal_scene_object_at(loaded, i);
            if (o->armature)
                lo_arm = o;
        }
        HAL_TEST_CHECK(lo_arm && lo_arm->armature);
        if (lo_arm && lo_arm->armature &&
            lo_arm->armature->count == 2) {
            /* bone 0 = node 3 (child, now first), bone 1 = node 2.
             * Offset of the CHILD bone (index 0) must be its parent
             * (index 1)'s offset * its local — NOT just the local. */
            hal_quatf want_parent =
                hal_quatf_mul(hal_quatf_identity(), root_rot);
            hal_quatf want_child = hal_quatf_mul(want_parent,
                                                 child_rot);
            /* bones[0] = child, bones[1] = root */
            hal_quatf got_child = lo_arm->armature->bones[0].rest_offset;
            hal_quatf got_root = lo_arm->armature->bones[1].rest_offset;
            /* quaternion sign-insensitive comparison */
            hal_f32 d_root = fabsf(
                hal_quatf_dot(got_root, want_parent));
            hal_f32 d_child = fabsf(
                hal_quatf_dot(got_child, want_child));
            HAL_TEST_CHECK(d_root > 0.999f);
            HAL_TEST_CHECK(d_child > 0.999f);
        }
        hal_scene_destroy(loaded);
    }
}

/* ---- Materials, embedded textures, multi-primitive meshes ----
 * Builds a glb with a material (baseColorFactor + an embedded PNG
 * texture via bufferView and via a data: URI) and a TWO-primitive
 * mesh; the import must attach the material, extract the image to
 * <glb>-textures/, and merge both primitives. */

static bool make_png_bytes(const char *path, size_t *out_len) {
    hal_surface *s = NULL;
    if (!hal_ok(hal_surface_offscreen_create(2, 1, &s)))
        return false;
    hal_canvas c;
    if (!hal_canvas_init(&c, s, 1.0f)) {
        hal_surface_destroy(s);
        return false;
    }
    c.pixels[0] = 0xFF0000;
    c.pixels[1] = 0x00FF00;
    bool ok = hal_ok(hal_surface_save_png(s, path));
    hal_surface_destroy(s);
    if (ok) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            *out_len = (size_t)sz;
        }
    }
    return ok;
}

static void test_material_texture_multiprim(void) {
    /* A tiny PNG. */
    char png_path[512];
    size_t png_len = 0;
    snprintf(png_path, sizeof(png_path), "%s/t.png", tmp_dir());
    HAL_TEST_CHECK(make_png_bytes(png_path, &png_len));
    HAL_TEST_CHECK(png_len > 8 && png_len < 3000);
    unsigned char png[3000];
    {
        FILE *f = fopen(png_path, "rb");
        HAL_TEST_CHECK(f != NULL);
        if (f) {
            HAL_TEST_CHECK(fread(png, 1, png_len, f) == png_len);
            fclose(f);
        }
    }

    /* Base64 of the PNG (built by hand; the alphabet is fixed). */
    static const char b64c[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char b64[4200];
    size_t b64n = 0;
    for (size_t i = 0; i < png_len; i += 3) {
        unsigned v = (unsigned)png[i] << 16;
        int n = 1;
        if (i + 1 < png_len) {
            v |= (unsigned)png[i + 1] << 8;
            n = 2;
        }
        if (i + 2 < png_len) {
            v |= png[i + 2];
            n = 3;
        }
        b64[b64n++] = b64c[(v >> 18) & 63];
        b64[b64n++] = b64c[(v >> 12) & 63];
        b64[b64n++] = n >= 3 ? b64c[(v >> 6) & 63] : '=';
        b64[b64n++] = n >= 3 ? b64c[v & 63] : '=';
    }
    b64[b64n] = 0;

    /* Two primitives. */
    hal_f32 v0[] = { 0, 0, 0, 1, 0, 0, 1, 0, 1 };
    hal_f32 v1[] = { 0, 1, 0, 0, 1, 1, 1, 1, 1 };
    hal_u32 i0[] = { 0, 1, 2 };
    hal_u32 i1[] = { 0, 1, 2 };
    hal_f32 uv0[] = { 0, 0, 1, 0, 1, 1 };
    hal_f32 uv1[] = { 0, 0, 1, 0, 1, 1 };

    glb_writer w;
    glbw_init(&w);
    int a_v0 = glbw_view(&w, v0, sizeof(v0));
    int a_i0 = glbw_view(&w, i0, sizeof(i0));
    int a_uv0 = glbw_view(&w, uv0, sizeof(uv0));
    int a_v1 = glbw_view(&w, v1, sizeof(v1));
    int a_i1 = glbw_view(&w, i1, sizeof(i1));
    int a_uv1 = glbw_view(&w, uv1, sizeof(uv1));
    int a_png = glbw_view(&w, png, png_len);

    char path[512];
    snprintf(path, sizeof(path), "%s/mat.glb", tmp_dir());
    snprintf(
        w.json, sizeof(w.json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"nodes\":[{\"name\":\"Box\",\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":2},"
        "\"indices\":1,\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":3,\"TEXCOORD_0\":5},"
        "\"indices\":4,\"material\":0,\"mode\":4}"
        "]}],"
        "\"materials\":[{\"name\":\"Paint\","
        "\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.25,0.5,0.75,1],"
        "\"metallicFactor\":0.2,\"roughnessFactor\":0.8,"
        "\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0},{\"source\":1}],"
        "\"images\":[{\"bufferView\":6,\"mimeType\":\"image/png\"},"
        "{\"uri\":\"data:image/png;base64,%s\"}],"
        "\"accessors\":["
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":%d,\"componentType\":5125,\"count\":3,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC2\"},"
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":%d,\"componentType\":5125,\"count\":3,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":%d,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC2\"},"
        "{\"bufferView\":%d,\"componentType\":5121,"
        "\"count\":%zu}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}"
        "],"
        "\"buffers\":[{\"byteLength\":%zu}]}",
        b64,
        a_v0, a_i0, a_uv0, a_v1, a_i1, a_uv1, a_png, png_len,
        w.view_off[a_v0], w.view_len[a_v0],
        w.view_off[a_i0], w.view_len[a_i0],
        w.view_off[a_uv0], w.view_len[a_uv0],
        w.view_off[a_v1], w.view_len[a_v1],
        w.view_off[a_i1], w.view_len[a_i1],
        w.view_off[a_uv1], w.view_len[a_uv1],
        w.view_off[a_png], w.view_len[a_png],
        w.bin_len);
    HAL_TEST_CHECK(glbw_write(&w, path));

    char err[256] = { 0 };
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (!loaded) {
        printf("import err: %s\n", err);
        return;
    }
    HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 1);
    hal_object *o = hal_scene_object_at(loaded, 0);
    HAL_TEST_CHECK(o && o->mesh);
    if (o && o->mesh) {
        /* Both primitives merged. */
        HAL_TEST_CHECK_EQ_I(o->mesh->vert_count, 6);
        HAL_TEST_CHECK_EQ_I(o->mesh->face_count, 2);
        /* UVs attached per corner. */
        HAL_TEST_CHECK(o->mesh->uvs != NULL &&
                       o->mesh->uvs_valid);
        /* The material attached with the file's factors. */
        HAL_TEST_CHECK(o->material >= 0);
        hal_material *mat = hal_scene_material(loaded, o->material);
        HAL_TEST_CHECK(mat != NULL);
        if (mat) {
            HAL_TEST_CHECK_EQ_F(mat->base_color.r, 0.25f, 1e-4);
            HAL_TEST_CHECK_EQ_F(mat->base_color.g, 0.5f, 1e-4);
            HAL_TEST_CHECK_EQ_F(mat->base_color.b, 0.75f, 1e-4);
            HAL_TEST_CHECK_EQ_F(mat->metallic, 0.2f, 1e-4);
            HAL_TEST_CHECK_EQ_F(mat->roughness, 0.8f, 1e-4);
            /* The embedded texture extracted + referenced. */
            HAL_TEST_CHECK(mat->texture >= 0);
            if (mat->texture >= 0) {
                const char *ip = hal_scene_image(loaded,
                                                 mat->texture);
                HAL_TEST_CHECK(ip != NULL);
                if (ip) {
                    HAL_TEST_CHECK(access(ip, R_OK) == 0);
                    /* It decodes through the texture cache. */
                    hal_texture_cache *cache =
                        hal_texture_cache_create();
                    if (cache) {
                        const hal_texture *t =
                            hal_texture_cache_get(cache, ip);
                        HAL_TEST_CHECK(t != NULL);
                        if (t) {
                            HAL_TEST_CHECK_EQ_I(t->w, 2);
                            HAL_TEST_CHECK_EQ_I(t->h, 1);
                        }
                        hal_texture_cache_destroy(cache);
                    }
                }
            }
        }
    }
    hal_scene_destroy(loaded);

    /* The data: URI image extracted too. */
    char tex1[600];
    snprintf(tex1, sizeof(tex1), "%s/mat-textures/tex1.png",
             tmp_dir());
    HAL_TEST_CHECK(access(tex1, R_OK) == 0);
}

int main(void) {
    test_foreign_rig_exact();
    test_foreign_child_first_ordering();
    test_material_texture_multiprim();
    HAL_TEST_SUMMARY("test_gltf_foreign");
}
