/*
 * gltf.c — glTF 2.0 binary (.glb) export and import
 *
 * Export: every mesh object becomes a glTF node (TRS + hierarchy)
 * with one mesh primitive: FLOAT positions, FLOAT per-corner normals
 * (face normals for flat shading, vertex normals for smooth), and
 * UNSIGNED_INT triangle indices, all in one GLB BIN chunk.
 *
 * Import: GLB files whose JSON matches the common subset — FLOAT
 * POSITION/NORMAL attributes, UNSIGNED_INT/SHORT/BYTE indices, node
 * chains (world transforms accumulated; hierarchy flattened — the
 * geometry lands exactly, the parenting is baked).
 *
 * Numbers are little-endian (glTF requires it; Linux x86-64/ARM64 are
 * both LE — a big-endian port would swap in two helpers).
 */

#include "io.h"

#include "../core/anim.h"
#include "../core/material.h"
#include "../core/mesh_ops.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Growable byte/string buffer ---- */

typedef struct buf {
    char *data;
    size_t len, cap;
} buf;

static bool buf_grow(buf *b, size_t need) {
    if (b->len + need <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + need)
        cap *= 2;
    char *grown = realloc(b->data, cap);
    if (!grown)
        return false;
    b->data = grown;
    b->cap = cap;
    return true;
}

static bool buf_put(buf *b, const void *p, size_t n) {
    if (!buf_grow(b, n))
        return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

static bool buf_puts(buf *b, const char *s) {
    return buf_put(b, s, strlen(s));
}

__attribute__((format(printf, 2, 3)))
static bool buf_putf(buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return buf_put(b, tmp, (size_t)n);
}

/* Appends a JSON string literal (escaping quotes, backslashes and
 * control characters). */
static bool buf_put_json_str(buf *b, const char *s) {
    if (!buf_puts(b, "\""))
        return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            if (!buf_put(b, esc, 2))
                return false;
        } else if (c < 0x20) {
            if (!buf_putf(b, "\\u%04x", (unsigned)c))
                return false;
        } else if (!buf_put(b, p, 1)) {
            return false;
        }
    }
    return buf_puts(b, "\"");
}

static void io_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* =====================================================================
 * Export
 * ===================================================================== */

/* Per-object export data gathered while filling the BIN buffer. */
typedef struct gltf_node_out {
    const hal_object *obj;
    size_t mesh_index; /* glTF mesh index                      */
} gltf_node_out;

static bool gltf_write_mesh(hal_object *obj, buf *bin, size_t *out_pos_off,
                            size_t *out_nrm_off, size_t *out_uv_off,
                            size_t *out_idx_off, bool *out_has_uv,
                            bool *out_has_skin, size_t *out_joint_off,
                            size_t *out_weight_off,
                            size_t *out_tri_count,
                            size_t *out_vert_count, hal_vec3f *out_min,
                            hal_vec3f *out_max) {
    hal_mesh *m = obj->mesh;
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    const hal_vec3f *vn =
        obj->smooth_shading ? hal_mesh_vert_normals(m) : NULL;
    if (!fn)
        return false;

    size_t tri_count = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = m->face_off[f + 1] - m->face_off[f];
        if (n >= 3)
            tri_count += n - 2;
    }

    /* Per-corner UVs or a skin force a duplicated stream even for
     * smooth shading (seams / per-corner joint data must survive);
     * otherwise smooth meshes share the original verts. */
    bool has_uv = m->uvs != NULL && m->uvs_valid;
    bool has_skin = obj->skin_armature != HAL_OBJECT_ID_NONE &&
                    m->skin_weights != NULL && m->skin_bones != NULL;
    bool smooth = vn != NULL && !has_uv && !has_skin;
    size_t out_verts = smooth ? m->vert_count : tri_count * 3;

    while (bin->len % 4 != 0)
        if (!buf_put(bin, "\0", 1))
            return false;

    size_t pos_off = bin->len;
    hal_vec3f bmin = { 1e30f, 1e30f, 1e30f };
    hal_vec3f bmax = { -1e30f, -1e30f, -1e30f };
    hal_u32 corner_idx[3];

    /* One macro-driven walk over triangle corners keeps the three
     * streams in lockstep. */
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = m->face_off[f + 1] - m->face_off[f];
        for (size_t k = 0; k + 2 < n; k++) {
            corner_idx[0] = m->loops[m->face_off[f]];
            corner_idx[1] = m->loops[m->face_off[f] + k + 1];
            corner_idx[2] = m->loops[m->face_off[f] + k + 2];
            for (int c = 0; c < 3; c++) {
                if (smooth)
                    continue; /* positions written once, below */
                hal_vec3f p = m->verts[corner_idx[c]];
                if (!buf_put(bin, &p, sizeof(p)))
                    return false;
                bmin.x = fminf(bmin.x, p.x);
                bmin.y = fminf(bmin.y, p.y);
                bmin.z = fminf(bmin.z, p.z);
                bmax.x = fmaxf(bmax.x, p.x);
                bmax.y = fmaxf(bmax.y, p.y);
                bmax.z = fmaxf(bmax.z, p.z);
            }
        }
    }
    if (smooth) {
        for (size_t v = 0; v < m->vert_count; v++) {
            hal_vec3f p = m->verts[v];
            if (!buf_put(bin, &p, sizeof(p)))
                return false;
            bmin.x = fminf(bmin.x, p.x);
            bmin.y = fminf(bmin.y, p.y);
            bmin.z = fminf(bmin.z, p.z);
            bmax.x = fmaxf(bmax.x, p.x);
            bmax.y = fmaxf(bmax.y, p.y);
            bmax.z = fmaxf(bmax.z, p.z);
        }
    }

    size_t nrm_off = bin->len;
    if (smooth) {
        for (size_t v = 0; v < m->vert_count; v++)
            if (!buf_put(bin, &vn[v], sizeof(hal_vec3f)))
                return false;
    } else {
        for (size_t f = 0; f < m->face_count; f++) {
            size_t n = m->face_off[f + 1] - m->face_off[f];
            for (size_t k = 0; k + 2 < n; k++)
                for (int c = 0; c < 3; c++) {
                    const hal_vec3f *nrm =
                        has_uv && vn ? &vn[m->loops[m->face_off[f] +
                                                    (k == 0 ? 0 : k + 1)]]
                                     : &fn[f];
                    if (!buf_put(bin, nrm, sizeof(hal_vec3f)))
                        return false;
                }
        }
    }

    size_t uv_off = bin->len;
    if (has_uv) {
        for (size_t f = 0; f < m->face_count; f++) {
            size_t n = m->face_off[f + 1] - m->face_off[f];
            for (size_t k = 0; k + 2 < n; k++)
                for (int c = 0; c < 3; c++) {
                    size_t corner = c == 0 ? (size_t)0 : k + (size_t)c;
                    size_t loop = m->face_off[f] + corner;
                    if (!buf_put(bin, &m->uvs[loop], sizeof(hal_vec2f)))
                        return false;
                }
        }
    }

    /* Skin influences: JOINTS_0 (UNSIGNED_BYTE VEC4, the bone index
     * into the skin's joints array) and WEIGHTS_0 (FLOAT VEC4,
     * renormalized). Unused slots carry joint 0 with weight 0. For
     * the duplicated stream a corner->mesh-vert map is built once
     * (the same order the position walk writes). */
    size_t joint_off = bin->len;
    size_t weight_off = bin->len;
    if (has_skin) {
        while (bin->len % 4 != 0)
            if (!buf_put(bin, "\0", 1))
                return false;
        joint_off = bin->len;
        weight_off = bin->len + out_verts * 4; /* u8 vec4 first */

        hal_u32 *corner_verts = NULL;
        if (!smooth) {
            corner_verts = malloc(out_verts * sizeof(hal_u32));
            if (!corner_verts)
                return false;
            size_t seen = 0;
            for (size_t f = 0; f < m->face_count; f++) {
                size_t n = m->face_off[f + 1] - m->face_off[f];
                for (size_t k = 0; k + 2 < n; k++) {
                    corner_verts[seen++] = m->loops[m->face_off[f]];
                    corner_verts[seen++] =
                        m->loops[m->face_off[f] + k + 1];
                    corner_verts[seen++] =
                        m->loops[m->face_off[f] + k + 2];
                }
            }
        }

        for (size_t pass = 0; pass < 2; pass++) {
            for (size_t v = 0; v < out_verts; v++) {
                size_t mv = smooth ? v : corner_verts[v];
                hal_u8 joints[4] = { 0, 0, 0, 0 };
                hal_f32 weights[4] = { 0, 0, 0, 0 };
                hal_f32 sum = 0.0f;
                for (int i = 0; i < 4; i++) {
                    size_t wi = mv * 4 + (size_t)i;
                    hal_i32 b = m->skin_bones[wi];
                    hal_f32 w = m->skin_weights[wi];
                    if (b < 0 || w <= 0.0f)
                        continue;
                    joints[i] = (hal_u8)b;
                    weights[i] = w;
                    sum += w;
                }
                if (sum > 1e-6f)
                    for (int i = 0; i < 4; i++)
                        weights[i] /= sum;
                if (pass == 0) {
                    if (!buf_put(bin, joints, sizeof(joints)))
                        return false;
                } else if (!buf_put(bin, weights, sizeof(weights))) {
                    return false;
                }
            }
        }
        free(corner_verts);
    }

    size_t idx_off = bin->len;
    if (smooth) {
        for (size_t f = 0; f < m->face_count; f++) {
            size_t n = m->face_off[f + 1] - m->face_off[f];
            for (size_t k = 0; k + 2 < n; k++) {
                hal_u32 tri[3] = { m->loops[m->face_off[f]],
                                   m->loops[m->face_off[f] + k + 1],
                                   m->loops[m->face_off[f] + k + 2] };
                if (!buf_put(bin, tri, sizeof(tri)))
                    return false;
            }
        }
    } else {
        for (hal_u32 i = 0; i < (hal_u32)(tri_count * 3); i++)
            if (!buf_put(bin, &i, sizeof(i)))
                return false;
    }

    *out_pos_off = pos_off;
    *out_nrm_off = nrm_off;
    *out_uv_off = uv_off;
    *out_idx_off = idx_off;
    *out_has_uv = has_uv;
    *out_has_skin = has_skin;
    *out_joint_off = joint_off;
    *out_weight_off = weight_off;
    *out_tri_count = tri_count;
    *out_vert_count = out_verts;
    *out_min = bmin;
    *out_max = bmax;
    return true;
}

hal_gltf_export_opts hal_gltf_export_opts_default(void) {
    hal_gltf_export_opts o;
    o.unit_scale = 1.0f;
    o.z_up = false;
    return o;
}

/* Root-node conversion: local' = S(unit) * Rx(90 if z_up) * local.
 * Returns the rotation quaternion (identity when y-up). */
static hal_quatf export_axis_rot(bool z_up) {
    if (!z_up)
        return hal_quatf_identity();
    /* +90 degrees about X maps +Z (up in a Z-up workflow) to +Y. */
    return hal_quatf_make(0.70710678f, 0.0f, 0.0f, 0.70710678f);
}

hal_result hal_io_gltf_export_ex(hal_scene *scene, const char *path,
                                 const hal_gltf_export_opts *opts,
                                 char *err, size_t err_cap) {
    hal_gltf_export_opts def;
    if (!opts) {
        def = hal_gltf_export_opts_default();
        opts = &def;
    }
    const hal_quatf axis_q = export_axis_rot(opts->z_up);
    const hal_f32 unit = opts->unit_scale > 0.0f ? opts->unit_scale
                                                  : 1.0f;
    const bool convert = opts->z_up || unit != 1.0f;
    if (!scene || !path) {
        io_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }

    /* Pass 1: BIN buffer blocks per mesh object. */
    buf bin = { 0 };
    size_t mesh_count = 0;
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].mesh)
            mesh_count++;

    typedef struct mesh_blocks {
        size_t pos_off, nrm_off, uv_off, idx_off;
        bool has_uv;
        size_t tri_count, vert_count;
        hal_vec3f bmin, bmax;
        int material; /* object material index or -1            */
        size_t a_pos, a_nrm, a_uv, a_idx; /* accessor indices    */
        size_t v_pos, v_nrm, v_uv, v_idx; /* bufferView indices  */
        bool has_skin;
        size_t joint_off, weight_off;
        size_t a_jnt, a_wgt; /* JOINTS_0 / WEIGHTS_0 accessors    */
        size_t v_jnt, v_wgt;
        size_t skin_index;   /* into the skins array, when skinned */
        size_t object_index; /* the owning scene object           */
    } mesh_blocks;
    mesh_blocks *blocks =
        calloc(mesh_count ? mesh_count : 1, sizeof(mesh_blocks));
    if (!blocks) {
        io_err(err, err_cap, "out of memory");
        return HAL_ERR_OUT_OF_MEMORY;
    }

    /* Joint nodes: one per bone, appended after the object nodes.
     * joint_node(obj, bone) = scene->count + arm_bone_base[obj] +
     * bone. */
    size_t *arm_bone_base =
        calloc(scene->count ? scene->count : 1, sizeof(size_t));
    if (!arm_bone_base) {
        free(blocks);
        io_err(err, err_cap, "out of memory");
        return HAL_ERR_OUT_OF_MEMORY;
    }
    {
        size_t running = 0;
        for (size_t i = 0; i < scene->count; i++) {
            arm_bone_base[i] = running;
            if (scene->objects[i].armature)
                running += scene->objects[i].armature->count;
        }
    }
#define JOINT_NODE(obj_idx, bone) \
    (scene->count + arm_bone_base[obj_idx] + (size_t)(bone))

    /* Skins: one per skinned mesh (inverse bind matrices depend on
     * the mesh's own world transform). */
    size_t skin_count = 0;

    size_t bi = 0;
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->mesh)
            continue;
        if (!gltf_write_mesh(obj, &bin, &blocks[bi].pos_off,
                             &blocks[bi].nrm_off, &blocks[bi].uv_off,
                             &blocks[bi].idx_off, &blocks[bi].has_uv,
                             &blocks[bi].has_skin, &blocks[bi].joint_off,
                             &blocks[bi].weight_off,
                             &blocks[bi].tri_count,
                             &blocks[bi].vert_count, &blocks[bi].bmin,
                             &blocks[bi].bmax)) {
            free(bin.data);
            free(blocks);
            free(arm_bone_base);
            io_err(err, err_cap, "out of memory while writing buffers");
            return HAL_ERR_OUT_OF_MEMORY;
        }
        blocks[bi].material = obj->material;
        blocks[bi].object_index = i;
        if (blocks[bi].has_skin) {
            blocks[bi].skin_index = skin_count;
            skin_count++;
        }
        bi++;
    }

    /* Sequential accessor / bufferView indices (UV slots exist only
     * for meshes with UVs, so fixed strides would misnumber mixed
     * scenes). */
    size_t mesh_acc_count = 0;
    size_t ibm_acc_base = 0;
    size_t ibm_view_base = 0;
    {
        size_t acc = 0, view = 0;
        for (size_t mi = 0; mi < mesh_count; mi++) {
            blocks[mi].a_pos = acc++;
            blocks[mi].a_nrm = acc++;
            blocks[mi].a_uv = blocks[mi].has_uv ? acc++ : (size_t)-1;
            blocks[mi].a_idx = acc++;
            if (blocks[mi].has_skin) {
                blocks[mi].a_jnt = acc++;
                blocks[mi].a_wgt = acc++;
            }
            blocks[mi].v_pos = view++;
            blocks[mi].v_nrm = view++;
            blocks[mi].v_uv = blocks[mi].has_uv ? view++ : (size_t)-1;
            blocks[mi].v_idx = view++;
            if (blocks[mi].has_skin) {
                blocks[mi].v_jnt = view++;
                blocks[mi].v_wgt = view++;
            }
        }
        mesh_acc_count = acc;
        /* The skins' inverseBindMatrices accessors follow; the
         * animation sampler accessors come after those. */
        ibm_acc_base = acc;
        ibm_view_base = view;
        acc += skin_count;
        view += skin_count;
    }

    /* Image embedding: scene images referenced by any exported
     * material, appended to the BIN buffer. */
    typedef struct image_block {
        int scene_index;
        size_t bin_off, bin_len;
    } image_block;
    image_block *images_out = NULL;
    size_t image_out_count = 0;
    for (int mi = 0; mi < (int)scene->material_count; mi++) {
        int img = scene->materials[mi].texture;
        if (img < 0)
            continue;
        bool already = false;
        for (size_t k = 0; k < image_out_count; k++)
            if (images_out[k].scene_index == img)
                already = true;
        if (already)
            continue;
        const char *ipath = hal_scene_image(scene, img);
        if (!ipath)
            continue;
        FILE *im = fopen(ipath, "rb");
        if (!im)
            continue;
        fseek(im, 0, SEEK_END);
        long isz = ftell(im);
        rewind(im);
        if (isz <= 0 || isz > 64 * 1024 * 1024) {
            fclose(im);
            continue;
        }
        image_block *grown = realloc(
            images_out, (image_out_count + 1) * sizeof(*grown));
        if (!grown) {
            fclose(im);
            continue;
        }
        images_out = grown;
        unsigned char *bytes = malloc((size_t)isz);
        if (!bytes || fread(bytes, 1, (size_t)isz, im) != (size_t)isz) {
            free(bytes);
            fclose(im);
            continue;
        }
        fclose(im);
        images_out[image_out_count].scene_index = img;
        images_out[image_out_count].bin_off = bin.len;
        images_out[image_out_count].bin_len = (size_t)isz;
        image_out_count++;
        buf_put(&bin, bytes, (size_t)isz);
        free(bytes);
    }

    /* Pass 2: JSON. */
    buf js = { 0 };
    if (!buf_puts(&js, "{\"asset\":{\"version\":\"2.0\","
                       "\"generator\":\"Halcyon\"},"))
        goto oom;

    if (!buf_puts(&js, "\"scene\":0,\"scenes\":[{\"nodes\":["))
        goto oom;
    bool first = true;
    for (size_t i = 0; i < scene->count; i++) {
        if (scene->objects[i].parent != HAL_OBJECT_ID_NONE)
            continue;
        if (!buf_putf(&js, "%s%zu", first ? "" : ",", i))
            goto oom;
        first = false;
    }
    if (!buf_puts(&js, "]}],"))
        goto oom;

    if (!buf_puts(&js, "\"nodes\":["))
        goto oom;
    for (size_t i = 0; i < scene->count; i++) {
        const hal_object *o = &scene->objects[i];
        if (!buf_putf(&js, "%s{", i ? "," : ""))
            goto oom;
        if (!buf_puts(&js, "\"name\":") ||
            !buf_put_json_str(&js, o->name))
            goto oom;
        /* children: objects parented here, plus (for armatures) the
         * root bones' joint nodes. */
        {
            bool any_child = false;
            for (size_t c = 0; c < scene->count; c++)
                if (scene->objects[c].parent == o->id)
                    any_child = true;
            bool any_joint =
                o->armature != NULL && o->armature->count > 0;
            if (any_child || any_joint) {
                if (!buf_puts(&js, ",\"children\":["))
                    goto oom;
                bool inner_first = true;
                for (size_t c2 = 0; c2 < scene->count; c2++)
                    if (scene->objects[c2].parent == o->id) {
                        if (!buf_putf(&js, "%s%zu",
                                      inner_first ? "" : ",", c2))
                            goto oom;
                        inner_first = false;
                    }
                if (any_joint)
                    for (size_t b = 0; b < o->armature->count; b++)
                        if (o->armature->bones[b].parent < 0) {
                            if (!buf_putf(&js, "%s%zu",
                                          inner_first ? "" : ",",
                                          JOINT_NODE(i, b)))
                                goto oom;
                            inner_first = false;
                        }
                if (!buf_puts(&js, "]"))
                    goto oom;
            }
        }
        if (o->mesh) {
            size_t j = 0;
            for (size_t k = 0; k <= i; k++)
                if (scene->objects[k].mesh)
                    j++;
            if (!buf_putf(&js, ",\"mesh\":%zu", j - 1))
                goto oom;
            /* glTF puts "skin" on the NODE: find this object's mesh
             * block and reference its skin. */
            for (size_t mb = 0; mb < mesh_count; mb++)
                if (blocks[mb].object_index == i &&
                    blocks[mb].has_skin) {
                    if (!buf_putf(&js, ",\"skin\":%zu",
                                  blocks[mb].skin_index))
                        goto oom;
                    break;
                }
        }
        {
            /* Root nodes absorb the unit/axis conversion (children
             * ride along through the hierarchy). */
            hal_vec3f loc = o->location;
            hal_quatf rot = o->rotation;
            hal_vec3f scl = o->scale;
            if (convert && o->parent == HAL_OBJECT_ID_NONE) {
                loc = hal_vec3f_mul(
                    hal_quatf_rotate_vec3(axis_q, loc), unit);
                rot = hal_quatf_mul(axis_q, rot);
                scl = hal_vec3f_mul(scl, unit);
            }
            if (!buf_putf(&js,
                          ",\"translation\":[%.9g,%.9g,%.9g],"
                          "\"rotation\":[%.9g,%.9g,%.9g,%.9g],"
                          "\"scale\":[%.9g,%.9g,%.9g]",
                          (double)loc.x, (double)loc.y, (double)loc.z,
                          (double)rot.x, (double)rot.y, (double)rot.z,
                          (double)rot.w, (double)scl.x, (double)scl.y,
                          (double)scl.z))
                goto oom;
        }
        if (!buf_puts(&js, "}"))
            goto oom;
    }
    /* Joint nodes (bones). Local transform: translation = the bone's
     * REST head (armature space — a constant offset in the parent's
     * frame, matching Halcyon's pose math), rotation = the local pose
     * (identity at rest). Children: child bones' joints. */
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *ao = &scene->objects[i];
        if (!ao->armature)
            continue;
        for (size_t b = 0; b < ao->armature->count; b++) {
            const hal_bone *bone = &ao->armature->bones[b];
            /* The very first entry of the whole nodes array has no
             * leading comma; every other joint node does (the object
             * nodes came first). */
            bool first_array_entry =
                scene->count == 0 && i == 0 && b == 0;
            if (!buf_putf(&js, "%s{", first_array_entry ? "" : ","))
                goto oom;
            if (!buf_puts(&js, "\"name\":") ||
                !buf_put_json_str(&js, bone->name))
                goto oom;
            if (!buf_putf(&js,
                          ",\"translation\":[%.9g,%.9g,%.9g],"
                          "\"rotation\":[%.9g,%.9g,%.9g,%.9g]",
                          (double)bone->head.x, (double)bone->head.y,
                          (double)bone->head.z, (double)bone->pose.x,
                          (double)bone->pose.y, (double)bone->pose.z,
                          (double)bone->pose.w))
                goto oom;
            bool any_child = false;
            for (size_t c = 0; c < ao->armature->count; c++)
                if (ao->armature->bones[c].parent == (int)b)
                    any_child = true;
            if (any_child) {
                if (!buf_puts(&js, ",\"children\":["))
                    goto oom;
                bool inner_first = true;
                for (size_t c = 0; c < ao->armature->count; c++)
                    if (ao->armature->bones[c].parent == (int)b) {
                        if (!buf_putf(&js, "%s%zu",
                                      inner_first ? "" : ",",
                                      JOINT_NODE(i, c)))
                            goto oom;
                        inner_first = false;
                    }
                if (!buf_puts(&js, "]"))
                    goto oom;
            }
            if (!buf_puts(&js, "}"))
                goto oom;
        }
    }
    if (!buf_puts(&js, "],"))
        goto oom;

    /* Meshes (with material references). */
    if (!buf_puts(&js, "\"meshes\":["))
        goto oom;
    {
        /* Remap object material index -> export material index (only
         * materials actually referenced are emitted). */
        int *mat_map = malloc(scene->material_count *
                              sizeof(int) + sizeof(int));
        if (!mat_map)
            goto oom;
        for (size_t m2 = 0; m2 < scene->material_count; m2++)
            mat_map[m2] = -1;
        int mat_out_count = 0;
        for (size_t mi = 0; mi < mesh_count; mi++)
            if (blocks[mi].material >= 0 &&
                mat_map[blocks[mi].material] < 0)
                mat_map[blocks[mi].material] = mat_out_count++;
        (void)mat_out_count;

        for (size_t mi = 0; mi < mesh_count; mi++) {
            const mesh_blocks *b = &blocks[mi];
            buf attrs = { 0 };
            bool a_ok =
                buf_putf(&attrs, "\"POSITION\":%zu,\"NORMAL\":%zu",
                         b->a_pos, b->a_nrm) &&
                (!b->has_uv ||
                 buf_putf(&attrs, ",\"TEXCOORD_0\":%zu", b->a_uv)) &&
                (!b->has_skin ||
                 buf_putf(&attrs,
                          ",\"JOINTS_0\":%zu,\"WEIGHTS_0\":%zu",
                          b->a_jnt, b->a_wgt)) &&
                buf_put(&attrs, "\0", 1); /* %s needs a C string */
            if (a_ok)
                a_ok = buf_putf(&js,
                                "%s{\"primitives\":[{"
                                "\"attributes\":{%s},"
                                "\"indices\":%zu,\"mode\":4",
                                mi ? "," : "", attrs.data ? attrs.data
                                                          : "",
                                b->a_idx);
            free(attrs.data);
            if (!a_ok)
                goto oom;
            if (b->material >= 0 && mat_map[b->material] >= 0)
                if (!buf_putf(&js, ",\"material\":%d",
                              mat_map[b->material]))
                    goto oom;
            if (!buf_puts(&js, "}]}"))
                goto oom;
        }
        free(mat_map);
    }
    if (!buf_puts(&js, "],"))
        goto oom;

    /* Skins (pass 1.4): inverse bind matrices into the BIN buffer.
     * IBM_i = rest_i^-1 * A^-1 * Wm (rest_i = T(head_i) in armature
     * space; A = armature world; Wm = mesh world), so glTF's
     * sum(w * G_i * IBM_i * v) reproduces Halcyon's skinning exactly
     * (at rest the sum collapses to Wm * v). */
    typedef struct skin_info {
        size_t mesh_block;  /* index into blocks                    */
        size_t arm_obj;     /* the armature object index            */
        size_t ibm_off;
        size_t bone_count;
        size_t ibm_acc, ibm_view;
        size_t root_bone_joint; /* node id, for "skeleton"          */
    } skin_info;
    skin_info *skins_info =
        calloc(skin_count ? skin_count : 1, sizeof(skin_info));
    if (!skins_info)
        goto oom;
    {
        size_t si = 0;
        for (size_t mi = 0; mi < mesh_count; mi++) {
            if (!blocks[mi].has_skin)
                continue;
            hal_object *mesh_obj =
                &scene->objects[blocks[mi].object_index];
            hal_object *arm_obj =
                hal_scene_object(scene, mesh_obj->skin_armature);
            if (!arm_obj || !arm_obj->armature) {
                /* Unresolvable skin: drop the reference. */
                blocks[mi].has_skin = false;
                continue;
            }
            hal_armature *arm = arm_obj->armature;
            hal_bone_pose *rest = malloc(arm->count * sizeof(*rest));
            if (!rest)
                goto oom;
            hal_armature_rest_pose(arm, rest);
            hal_mat4f arm_world_inv = hal_mat4f_invert(
                hal_object_world_matrix(scene, arm_obj));
            hal_mat4f to_armature = hal_mat4f_mul(
                arm_world_inv,
                hal_object_world_matrix(scene, mesh_obj));

            while (bin.len % 4 != 0)
                if (!buf_put(&bin, "\0", 1))
                    goto oom;
            size_t off = bin.len;
            for (size_t b = 0; b < arm->count; b++) {
                hal_mat4f ibm = hal_mat4f_mul(
                    hal_mat4f_invert(rest[b].matrix), to_armature);
                if (!buf_put(&bin, &ibm, sizeof(ibm)))
                    goto oom;
            }
            free(rest);

            skins_info[si].mesh_block = mi;
            skins_info[si].arm_obj = (size_t)-1;
            for (size_t oi = 0; oi < scene->count; oi++)
                if (scene->objects[oi].id == arm_obj->id)
                    skins_info[si].arm_obj = oi;
            skins_info[si].ibm_off = off;
            skins_info[si].bone_count = arm->count;
            skins_info[si].ibm_acc = ibm_acc_base + si;
            skins_info[si].ibm_view = ibm_view_base + si;
            skins_info[si].root_bone_joint = (size_t)-1;
            for (size_t b = 0; b < arm->count; b++)
                if (arm->bones[b].parent < 0) {
                    skins_info[si].root_bone_joint =
                        JOINT_NODE(skins_info[si].arm_obj, b);
                    break;
                }
            si++;
        }
    }

    /* Animations (pass 1.5): bake every clip at 30 fps into the BIN
     * buffer (LINEAR samplers; in-app easing bakes into the samples)
     * and collect the channels/samplers JSON + the accessor data the
     * sections below append. */
    /* Animation sampler accessors follow the mesh accessors AND the
     * skins' IBM accessors. */
    size_t anim_acc_base = ibm_acc_base + skin_count;
    typedef struct sampler_block {
        size_t in_off, out_off, frames;
        int comps;
    } sampler_block;
    sampler_block *anim_samplers = NULL;
    size_t anim_sampler_count = 0;
    buf anims_json = { 0 };
    if (scene->clip_count > 0) {
        if (!buf_puts(&anims_json, ",\"animations\":["))
            goto oom;
        for (size_t ci = 0; ci < scene->clip_count; ci++) {
            const hal_clip *clip = &scene->clips[ci];
            if (ci && !buf_puts(&anims_json, ","))
                goto oom;
            if (!buf_puts(&anims_json, "{\"name\":"))
                goto oom;
            if (!buf_put_json_str(&anims_json, clip->name))
                goto oom;

            hal_f32 span = clip->end - clip->start;
            if (span < 1e-4f)
                span = 1.0f / 30.0f;
            size_t frames = (size_t)(span * 30.0f) + 1;
            if (frames < 2)
                frames = 2;
            if (frames > 100000)
                frames = 100000;

            bool first_ch = true;
            size_t clip_first_sampler = anim_sampler_count;
            if (!buf_puts(&anims_json, ",\"channels\":["))
                goto oom;
            for (size_t oi = 0; oi < scene->count; oi++) {
                const hal_object *o = &scene->objects[oi];
                bool has = false;
                for (size_t c = 0; c < clip->count; c++)
                    if (clip->channels[c].object == o->id &&
                        clip->channels[c].type != HAL_CH_BONE)
                        has = true;
                if (!has)
                    continue;

                hal_vec3f loc = o->location;
                hal_quatf rot = o->rotation;
                hal_vec3f scale = o->scale;
                int mask = hal_clip_sample_object(
                    clip, o->id, clip->start, &loc, &rot, &scale);
                const struct {
                    const char *path;
                    int mask_bit;
                    int comps;
                } paths[3] = {
                    { "translation", 1, 3 },
                    { "rotation", 2, 4 },
                    { "scale", 4, 3 },
                };
                for (int p = 0; p < 3; p++) {
                    if (!(mask & paths[p].mask_bit))
                        continue;
                    while (bin.len % 4 != 0)
                        if (!buf_put(&bin, "\0", 1))
                            goto oom;
                    size_t in_off = bin.len;
                    for (size_t f2 = 0; f2 < frames; f2++) {
                        hal_f32 t = clip->start +
                                    (hal_f32)f2 /
                                        (hal_f32)(frames - 1) * span;
                        if (!buf_put(&bin, &t, sizeof(hal_f32)))
                            goto oom;
                    }
                    size_t out_off = bin.len;
                    for (size_t f2 = 0; f2 < frames; f2++) {
                        hal_f32 t = clip->start +
                                    (hal_f32)f2 /
                                        (hal_f32)(frames - 1) * span;
                        hal_vec3f l = o->location;
                        hal_quatf r = o->rotation;
                        hal_vec3f sc = o->scale;
                        hal_clip_sample_object(clip, o->id, t, &l, &r,
                                               &sc);
                        if (paths[p].comps == 3) {
                            hal_vec3f v = p == 0 ? l : sc;
                            /* Root targets absorb the conversion. */
                            if (convert &&
                                o->parent == HAL_OBJECT_ID_NONE) {
                                if (p == 0)
                                    v = hal_vec3f_mul(
                                        hal_quatf_rotate_vec3(axis_q,
                                                              v),
                                        unit);
                                else
                                    v = hal_vec3f_mul(v, unit);
                            }
                            if (!buf_put(&bin, &v, sizeof(v)))
                                goto oom;
                        } else {
                            hal_quatf rq = r;
                            if (convert &&
                                o->parent == HAL_OBJECT_ID_NONE)
                                rq = hal_quatf_mul(axis_q, rq);
                            if (!buf_put(&bin, &rq, sizeof(rq)))
                                goto oom;
                        }
                    }
                    sampler_block *grown = realloc(
                        anim_samplers,
                        (anim_sampler_count + 1) * sizeof(*grown));
                    if (!grown)
                        goto oom;
                    anim_samplers = grown;
                    anim_samplers[anim_sampler_count].in_off = in_off;
                    anim_samplers[anim_sampler_count].out_off = out_off;
                    anim_samplers[anim_sampler_count].frames = frames;
                    anim_samplers[anim_sampler_count].comps =
                        paths[p].comps;
                    if (!buf_putf(&anims_json,
                                  "%s{\"sampler\":%zu,"
                                  "\"target\":{\"node\":%zu,"
                                  "\"path\":\"%s\"}}",
                                  first_ch ? "" : ",", anim_sampler_count,
                                  oi, paths[p].path))
                        goto oom;
                    first_ch = false;
                    anim_sampler_count++;
                }
            }
            /* Bone channels: rotation paths on joint nodes. */
            for (size_t c = 0; c < clip->count; c++) {
                const hal_anim_channel *ch = &clip->channels[c];
                if (ch->type != HAL_CH_BONE)
                    continue;
                size_t aoi = (size_t)-1;
                for (size_t oi = 0; oi < scene->count; oi++)
                    if (scene->objects[oi].id == ch->object)
                        aoi = oi;
                if (aoi == (size_t)-1)
                    continue;

                while (bin.len % 4 != 0)
                    if (!buf_put(&bin, "\0", 1))
                        goto oom;
                size_t in_off = bin.len;
                for (size_t f2 = 0; f2 < frames; f2++) {
                    hal_f32 t = clip->start +
                                (hal_f32)f2 / (hal_f32)(frames - 1) *
                                    span;
                    if (!buf_put(&bin, &t, sizeof(hal_f32)))
                        goto oom;
                }
                size_t out_off = bin.len;
                for (size_t f2 = 0; f2 < frames; f2++) {
                    hal_f32 t = clip->start +
                                (hal_f32)f2 / (hal_f32)(frames - 1) *
                                    span;
                    hal_quatf r = hal_quatf_identity();
                    hal_clip_sample_bone(clip, ch->object, ch->bone,
                                         t, &r);
                    if (!buf_put(&bin, &r, sizeof(r)))
                        goto oom;
                }
                sampler_block *grown = realloc(
                    anim_samplers,
                    (anim_sampler_count + 1) * sizeof(*grown));
                if (!grown)
                    goto oom;
                anim_samplers = grown;
                anim_samplers[anim_sampler_count].in_off = in_off;
                anim_samplers[anim_sampler_count].out_off = out_off;
                anim_samplers[anim_sampler_count].frames = frames;
                anim_samplers[anim_sampler_count].comps = 4;

                if (!buf_putf(&anims_json,
                              "%s{\"sampler\":%zu,"
                              "\"target\":{\"node\":%zu,"
                              "\"path\":\"rotation\"}}",
                              first_ch ? "" : ",", anim_sampler_count,
                              JOINT_NODE(aoi, (size_t)ch->bone)))
                    goto oom;
                first_ch = false;
                anim_sampler_count++;
            }

            if (!buf_puts(&anims_json, "],\"samplers\":["))
                goto oom;
            for (size_t k = clip_first_sampler; k < anim_sampler_count;
                 k++) {
                size_t acc_in = anim_acc_base + k * 2;
                if (!buf_putf(&anims_json,
                              "%s{\"input\":%zu,\"output\":%zu,"
                              "\"interpolation\":\"LINEAR\"}",
                              k > clip_first_sampler ? "," : "",
                              acc_in, acc_in + 1))
                    goto oom;
            }
            if (!buf_puts(&anims_json, "]}"))
                goto oom;
        }
        if (!buf_puts(&anims_json, "]"))
            goto oom;
    }

    /* Accessors + bufferViews (4 slots per mesh; UV slots valid only
     * when the mesh has UVs). */
    if (!buf_puts(&js, "\"accessors\":["))
        goto oom;
    for (size_t mi = 0; mi < mesh_count; mi++) {
        const mesh_blocks *b = &blocks[mi];
        if (!buf_putf(&js,
                      "%s{\"bufferView\":%zu,\"componentType\":5126,"
                      "\"count\":%zu,\"type\":\"VEC3\","
                      "\"min\":[%.9g,%.9g,%.9g],"
                      "\"max\":[%.9g,%.9g,%.9g]}",
                      mi ? "," : "", b->v_pos, b->vert_count,
                      (double)b->bmin.x, (double)b->bmin.y,
                      (double)b->bmin.z, (double)b->bmax.x,
                      (double)b->bmax.y, (double)b->bmax.z))
            goto oom;
        if (!buf_putf(&js,
                      ",{\"bufferView\":%zu,\"componentType\":5126,"
                      "\"count\":%zu,\"type\":\"VEC3\"}",
                      b->v_nrm, b->vert_count))
            goto oom;
        if (b->has_uv &&
            !buf_putf(&js,
                      ",{\"bufferView\":%zu,\"componentType\":5126,"
                      "\"count\":%zu,\"type\":\"VEC2\"}",
                      b->v_uv, b->vert_count))
            goto oom;
        if (!buf_putf(&js,
                      ",{\"bufferView\":%zu,\"componentType\":5125,"
                      "\"count\":%zu,\"type\":\"SCALAR\"}",
                      b->v_idx, b->tri_count * 3))
            goto oom;
        if (b->has_skin) {
            if (!buf_putf(&js,
                          ",{\"bufferView\":%zu,"
                          "\"componentType\":5121,"
                          "\"count\":%zu,\"type\":\"VEC4\"}",
                          b->v_jnt, b->vert_count))
                goto oom;
            if (!buf_putf(&js,
                          ",{\"bufferView\":%zu,"
                          "\"componentType\":5126,"
                          "\"count\":%zu,\"type\":\"VEC4\"}",
                          b->v_wgt, b->vert_count))
                goto oom;
        }
    }
    /* The skins' inverseBindMatrices accessors. */
    for (size_t si = 0; si < skin_count; si++) {
        if (!buf_putf(&js,
                      ",{\"bufferView\":%zu,"
                      "\"componentType\":5126,\"count\":%zu,"
                      "\"type\":\"MAT4\"}",
                      skins_info[si].ibm_view,
                      skins_info[si].bone_count))
            goto oom;
    }
    for (size_t k = 0; k < anim_sampler_count; k++) {
        const sampler_block *sb = &anim_samplers[k];
        /* Sampler views follow the mesh views, the image views AND
         * the skins' IBM views. */
        size_t view_in =
            mesh_acc_count + image_out_count + skin_count + k * 2;
        if (!buf_putf(&js,
                      ",{\"bufferView\":%zu,\"componentType\":5126,"
                      "\"count\":%zu,\"type\":\"SCALAR\"},"
                      "{\"bufferView\":%zu,\"componentType\":5126,"
                      "\"count\":%zu,\"type\":\"VEC%d\"}",
                      view_in, sb->frames, view_in + 1, sb->frames,
                      sb->comps))
            goto oom;
    }
    if (!buf_puts(&js, "],\"bufferViews\":["))
        goto oom;
    for (size_t mi = 0; mi < mesh_count; mi++) {
        const mesh_blocks *b = &blocks[mi];
        if (!buf_putf(&js,
                      "%s{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu},"
                      "{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu}",
                      mi ? "," : "", b->pos_off, b->vert_count * 12,
                      b->nrm_off, b->vert_count * 12))
            goto oom;
        if (b->has_uv &&
            !buf_putf(&js,
                      ",{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu}",
                      b->uv_off, b->vert_count * 8))
            goto oom;
        if (!buf_putf(&js,
                      ",{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu}",
                      b->idx_off, b->tri_count * 3 * 4))
            goto oom;
        if (b->has_skin) {
            if (!buf_putf(&js,
                          ",{\"buffer\":0,\"byteOffset\":%zu,"
                          "\"byteLength\":%zu},"
                          "{\"buffer\":0,\"byteOffset\":%zu,"
                          "\"byteLength\":%zu}",
                          b->joint_off, b->vert_count * 4,
                          b->weight_off, b->vert_count * 16))
                goto oom;
        }
    }
    /* IBM views. */
    for (size_t si = 0; si < skin_count; si++)
        if (!buf_putf(&js,
                      ",{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu}",
                      skins_info[si].ibm_off,
                      skins_info[si].bone_count * 64))
            goto oom;
    /* Image bufferViews follow the mesh slots. */
    {
        size_t mesh_views = mesh_count * 3;
        for (size_t k = 0; k < image_out_count; k++)
            if (!buf_putf(&js,
                          ",{\"buffer\":0,\"byteOffset\":%zu,"
                          "\"byteLength\":%zu}",
                          images_out[k].bin_off,
                          images_out[k].bin_len)) {
                (void)mesh_views;
                goto oom;
            }
    }
    for (size_t k = 0; k < anim_sampler_count; k++) {
        const sampler_block *sb = &anim_samplers[k];
        if (!buf_putf(&js,
                      ",{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu},"
                      "{\"buffer\":0,\"byteOffset\":%zu,"
                      "\"byteLength\":%zu}",
                      sb->in_off, sb->frames * 4, sb->out_off,
                      sb->frames * 4 * (size_t)sb->comps))
            goto oom;
    }
    if (!buf_putf(&js, "],\"buffers\":[{\"byteLength\":%zu}]",
                  bin.len))
        goto oom;

    /* Materials, textures, samplers, images. */
    if (scene->material_count > 0) {
        if (!buf_puts(&js, ",\"materials\":["))
            goto oom;
        bool mfirst = true;
        for (size_t m2 = 0; m2 < scene->material_count; m2++) {
            /* Only materials referenced by an exported mesh. */
            bool used = false;
            for (size_t mi = 0; mi < mesh_count; mi++)
                if (blocks[mi].material == (int)m2)
                    used = true;
            if (!used)
                continue;
            const hal_material *mat = &scene->materials[m2];
            int tex_idx = -1;
            if (mat->texture >= 0) {
                for (size_t k = 0; k < image_out_count; k++)
                    if (images_out[k].scene_index == mat->texture)
                        tex_idx = (int)k;
            }
            if (!buf_putf(&js,
                          "%s{\"name\":", mfirst ? "" : ","))
                goto oom;
            if (!buf_put_json_str(&js, mat->name))
                goto oom;
            if (!buf_putf(&js,
                          ",\"pbrMetallicRoughness\":{"
                          "\"baseColorFactor\":[%.9g,%.9g,%.9g,%"
                          ".9g],\"metallicFactor\":%.9g,"
                          "\"roughnessFactor\":%.9g",
                          (double)mat->base_color.r,
                          (double)mat->base_color.g,
                          (double)mat->base_color.b,
                          (double)mat->base_color.a,
                          (double)mat->metallic,
                          (double)mat->roughness))
                goto oom;
            if (tex_idx >= 0)
                if (!buf_putf(&js, ",\"baseColorTexture\":{\"index\":"
                                   "%d}",
                              tex_idx))
                    goto oom;
            if (!buf_putf(&js, "},\"emissiveFactor\":[%.9g,%.9g,%.9g]",
                          (double)(mat->emissive.x *
                                   mat->emissive_strength),
                          (double)(mat->emissive.y *
                                   mat->emissive_strength),
                          (double)(mat->emissive.z *
                                   mat->emissive_strength)))
                goto oom;
            if (!buf_puts(&js, "}"))
                goto oom;
            mfirst = false;
        }
        if (!buf_puts(&js, "]"))
            goto oom;

        if (image_out_count > 0) {
            if (!buf_puts(&js, ",\"textures\":["))
                goto oom;
            for (size_t k = 0; k < image_out_count; k++)
                if (!buf_putf(&js, "%s{\"sampler\":0,\"source\":%zu}",
                              k ? "," : "", k))
                    goto oom;
            if (!buf_puts(&js, "],\"samplers\":[{\"magFilter\":9729,"
                               "\"minFilter\":9987,\"wrapS\":33071,"
                               "\"wrapT\":33071}],\"images\":["))
                goto oom;
            size_t mesh_view_total = 0;
            for (size_t mi = 0; mi < mesh_count; mi++)
                mesh_view_total += blocks[mi].has_uv ? 4 : 3;
            for (size_t k = 0; k < image_out_count; k++) {
                const char *ipath =
                    hal_scene_image(scene, images_out[k].scene_index);
                const char *mime = "image/png";
                size_t plen = strlen(ipath ? ipath : "");
                if (plen > 4) {
                    if (strcmp(ipath + plen - 4, ".jpg") == 0 ||
                        (plen > 5 &&
                         strcmp(ipath + plen - 5, ".jpeg") == 0))
                        mime = "image/jpeg";
                }
                size_t view = mesh_view_total + k;
                if (!buf_putf(&js,
                              "%s{\"bufferView\":%zu,\"mimeType\":"
                              "\"%s\"}",
                              k ? "," : "", view, mime))
                    goto oom;
            }
            if (!buf_puts(&js, "]"))
                goto oom;
        }
    }
    /* skins array (before animations; glTF allows any order). */
    if (skin_count > 0) {
        if (!buf_puts(&js, ",\"skins\":["))
            goto oom;
        for (size_t si = 0; si < skin_count; si++) {
            const skin_info *sk = &skins_info[si];
            if (si && !buf_puts(&js, ","))
                goto oom;
            if (!buf_puts(&js, "{\"joints\":["))
                goto oom;
            for (size_t b = 0; b < sk->bone_count; b++) {
                if (!buf_putf(&js, "%s%zu", b ? "," : "",
                              JOINT_NODE(sk->arm_obj, b)))
                    goto oom;
            }
            if (!buf_putf(&js,
                          "],\"inverseBindMatrices\":%zu,"
                          "\"skeleton\":%zu}",
                          sk->ibm_acc, sk->root_bone_joint))
                goto oom;
        }
        if (!buf_puts(&js, "]"))
            goto oom;
    }
    if (anims_json.len &&
        !buf_put(&js, anims_json.data, anims_json.len))
        goto oom;
    free(anim_samplers);
    free(anims_json.data);

    if (!buf_puts(&js, "}"))
        goto oom;

    /* Pass 3: the GLB container. */
    {
        size_t json_len = js.len;
        size_t json_pad = (4 - json_len % 4) % 4;
        size_t bin_pad = (4 - bin.len % 4) % 4;
        size_t total =
            12 + 8 + json_len + json_pad +
            (bin.len ? 8 + bin.len + bin_pad : 0);

        FILE *f = fopen(path, "wb");
        if (!f) {
            io_err(err, err_cap, "cannot write %s", path);
            free(bin.data);
            free(js.data);
            free(blocks);
            free(images_out);
            return HAL_ERR_IO;
        }
        bool ok = true;
        hal_u32 header[3] = { 0x46546C67u, 2u, (hal_u32)total };
        ok = ok && fwrite(header, sizeof(header), 1, f) == 1;
        hal_u32 json_hdr[2] = { (hal_u32)(json_len + json_pad),
                                0x4E4F534Au };
        ok = ok && fwrite(json_hdr, sizeof(json_hdr), 1, f) == 1;
        ok = ok && fwrite(js.data, 1, js.len, f) == js.len;
        for (size_t i = 0; ok && i < json_pad; i++)
            ok = fputc(' ', f) != EOF;
        if (bin.len) {
            hal_u32 bin_hdr[2] = { (hal_u32)(bin.len + bin_pad),
                                   0x004E4942u };
            ok = ok && fwrite(bin_hdr, sizeof(bin_hdr), 1, f) == 1;
            ok = ok && fwrite(bin.data, 1, bin.len, f) == bin.len;
            for (size_t i = 0; ok && i < bin_pad; i++)
                ok = fputc(0, f) != EOF;
        }
        if (fclose(f) != 0)
            ok = false;
        free(bin.data);
        free(js.data);
        free(blocks);
        free(images_out);
        free(skins_info);
        free(arm_bone_base);
        if (!ok) {
            io_err(err, err_cap, "write failed");
            return HAL_ERR_IO;
        }
        return HAL_OK;
    }

oom:
    free(bin.data);
    free(js.data);
    free(blocks);
    free(images_out);
    free(anim_samplers);
    free(anims_json.data);
    free(skins_info);
    free(arm_bone_base);
    io_err(err, err_cap, "out of memory");
    return HAL_ERR_OUT_OF_MEMORY;
}

/* =====================================================================
 * JSON DOM (for import)
 * ===================================================================== */

typedef enum hal_json_type {
    HAL_JSON_NULL,
    HAL_JSON_BOOL,
    HAL_JSON_NUMBER,
    HAL_JSON_STRING,
    HAL_JSON_ARRAY,
    HAL_JSON_OBJECT,
} hal_json_type;

typedef struct hal_json {
    hal_json_type type;
    double number;
    char *string;
    struct hal_json **items; /* array values / object values */
    char **keys;             /* object keys */
    size_t count;
} hal_json;

typedef struct json_parser {
    const char *p;
    const char *end;
    int depth;
} json_parser;

static void hal_json_destroy(hal_json *j) {
    if (!j)
        return;
    free(j->string);
    for (size_t i = 0; i < j->count; i++) {
        hal_json_destroy(j->items[i]);
        if (j->keys)
            free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j);
}

static void json_skip_ws(json_parser *p) {
    while (p->p < p->end &&
           (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' ||
            *p->p == '\r'))
        p->p++;
}

static hal_json *json_parse_value(json_parser *p);

static bool json_parse_string(json_parser *p, char **out) {
    if (p->p >= p->end || *p->p != '"')
        return false;
    p->p++;
    buf b = { 0 };
    while (p->p < p->end && *p->p != '"') {
        char c = *p->p;
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end)
                goto fail;
            char e = *p->p;
            char simple = 0;
            switch (e) {
            case '"': simple = '"'; break;
            case '\\': simple = '\\'; break;
            case '/': simple = '/'; break;
            case 'b': simple = '\b'; break;
            case 'f': simple = '\f'; break;
            case 'n': simple = '\n'; break;
            case 'r': simple = '\r'; break;
            case 't': simple = '\t'; break;
            case 'u': {
                if (p->end - p->p < 5)
                    goto fail;
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = p->p[i];
                    unsigned d;
                    if (h >= '0' && h <= '9')
                        d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        d = (unsigned)(h - 'A' + 10);
                    else
                        goto fail;
                    cp = (cp << 4) | d;
                }
                p->p += 4;
                /* UTF-8 encode (BMP only; surrogate pairs unsupported —
                 * Halcyon's own exporter never emits them). */
                char u8[4];
                int n;
                if (cp < 0x80) {
                    u8[0] = (char)cp;
                    n = 1;
                } else if (cp < 0x800) {
                    u8[0] = (char)(0xC0 | (cp >> 6));
                    u8[1] = (char)(0x80 | (cp & 0x3F));
                    n = 2;
                } else {
                    u8[0] = (char)(0xE0 | (cp >> 12));
                    u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[2] = (char)(0x80 | (cp & 0x3F));
                    n = 3;
                }
                if (!buf_put(&b, u8, (size_t)n))
                    goto fail;
                break;
            }
            default:
                goto fail;
            }
            if (simple && !buf_put(&b, &simple, 1))
                goto fail;
            if (!simple && e != 'u')
                goto fail;
            p->p++;
        } else {
            if (!buf_put(&b, &c, 1))
                goto fail;
            p->p++;
        }
    }
    if (p->p >= p->end)
        goto fail;
    p->p++; /* closing quote */
    if (!buf_put(&b, "\0", 1))
        goto fail;
    *out = b.data;
    return true;

fail:
    free(b.data);
    return false;
}

static hal_json *json_new(hal_json_type type) {
    hal_json *j = calloc(1, sizeof(*j));
    if (j)
        j->type = type;
    return j;
}

static hal_json *json_parse_value(json_parser *p) {
    json_skip_ws(p);
    if (p->p >= p->end || p->depth > 64)
        return NULL;

    char c = *p->p;
    if (c == '{') {
        p->p++;
        p->depth++;
        hal_json *obj = json_new(HAL_JSON_OBJECT);
        if (!obj)
            return NULL;
        json_skip_ws(p);
        if (p->p < p->end && *p->p == '}') {
            p->p++;
            p->depth--;
            return obj;
        }
        for (;;) {
            json_skip_ws(p);
            char *key = NULL;
            if (!json_parse_string(p, &key)) {
                hal_json_destroy(obj);
                return NULL;
            }
            json_skip_ws(p);
            if (p->p >= p->end || *p->p != ':') {
                free(key);
                hal_json_destroy(obj);
                return NULL;
            }
            p->p++;
            hal_json *val = json_parse_value(p);
            if (!val) {
                free(key);
                hal_json_destroy(obj);
                return NULL;
            }
            /* Grow both arrays. */
            hal_json **gi = realloc(obj->items,
                                    (obj->count + 1) * sizeof(*gi));
            char **gk = realloc(obj->keys,
                                (obj->count + 1) * sizeof(*gk));
            if (!gi || !gk) {
                free(gi ? gi : NULL);
                free(gk);
                free(key);
                hal_json_destroy(val);
                hal_json_destroy(obj);
                return NULL;
            }
            obj->items = gi;
            obj->keys = gk;
            obj->items[obj->count] = val;
            obj->keys[obj->count] = key;
            obj->count++;
            json_skip_ws(p);
            if (p->p < p->end && *p->p == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->end && *p->p == '}') {
                p->p++;
                p->depth--;
                return obj;
            }
            hal_json_destroy(obj);
            return NULL;
        }
    }
    if (c == '[') {
        p->p++;
        p->depth++;
        hal_json *arr = json_new(HAL_JSON_ARRAY);
        if (!arr)
            return NULL;
        json_skip_ws(p);
        if (p->p < p->end && *p->p == ']') {
            p->p++;
            p->depth--;
            return arr;
        }
        for (;;) {
            hal_json *val = json_parse_value(p);
            if (!val) {
                hal_json_destroy(arr);
                return NULL;
            }
            hal_json **gi = realloc(arr->items,
                                    (arr->count + 1) * sizeof(*gi));
            if (!gi) {
                hal_json_destroy(val);
                hal_json_destroy(arr);
                return NULL;
            }
            arr->items = gi;
            arr->items[arr->count++] = val;
            json_skip_ws(p);
            if (p->p < p->end && *p->p == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->end && *p->p == ']') {
                p->p++;
                p->depth--;
                return arr;
            }
            hal_json_destroy(arr);
            return NULL;
        }
    }
    if (c == '"') {
        char *str = NULL;
        if (!json_parse_string(p, &str))
            return NULL;
        hal_json *j = json_new(HAL_JSON_STRING);
        if (!j) {
            free(str);
            return NULL;
        }
        j->string = str;
        return j;
    }
    if (c == 't' && p->end - p->p >= 4 &&
        strncmp(p->p, "true", 4) == 0) {
        p->p += 4;
        hal_json *j = json_new(HAL_JSON_BOOL);
        if (j)
            j->number = 1;
        return j;
    }
    if (c == 'f' && p->end - p->p >= 5 &&
        strncmp(p->p, "false", 5) == 0) {
        p->p += 5;
        return json_new(HAL_JSON_BOOL);
    }
    if (c == 'n' && p->end - p->p >= 4 &&
        strncmp(p->p, "null", 4) == 0) {
        p->p += 4;
        return json_new(HAL_JSON_NULL);
    }
    /* Number. */
    {
        char *endp = NULL;
        double v = strtod(p->p, &endp);
        if (endp == p->p || endp > p->end)
            return NULL;
        p->p = endp;
        hal_json *j = json_new(HAL_JSON_NUMBER);
        if (j)
            j->number = v;
        return j;
    }
}

static const hal_json *json_get(const hal_json *obj, const char *key) {
    if (!obj || obj->type != HAL_JSON_OBJECT)
        return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    return NULL;
}

static double json_num(const hal_json *j, size_t index, double dflt) {
    if (!j || j->type != HAL_JSON_ARRAY || index >= j->count)
        return dflt;
    const hal_json *v = j->items[index];
    return v && v->type == HAL_JSON_NUMBER ? v->number : dflt;
}

/* =====================================================================
 * Import
 * ===================================================================== */

typedef struct glb {
    const char *json;
    size_t json_len;
    const unsigned char *bin;
    size_t bin_len;
} glb;

/* Accessor read helpers over the BIN chunk. */
typedef struct acc_info {
    size_t buffer_view;   /* index into bufferViews         */
    size_t byte_offset;   /* accessor-level offset          */
    hal_u32 component_type; /* 5126 float, 5125/5123/5121  */
    size_t count;         /* element count                  */
    int vec_len;          /* 1 scalar, 3 vec3               */
    size_t view_offset;   /* from the bufferView            */
    size_t view_length;
} acc_info;

static bool read_accessor(const hal_json *accessors, size_t index,
                          const hal_json *views, const glb *g,
                          acc_info *out, char *err, size_t err_cap) {
    if (!accessors || accessors->type != HAL_JSON_ARRAY ||
        index >= accessors->count) {
        io_err(err, err_cap, "glTF: accessor %zu out of range", index);
        return false;
    }
    const hal_json *a = accessors->items[index];
    const hal_json *bv = json_get(a, "bufferView");
    const hal_json *ct = json_get(a, "componentType");
    const hal_json *cnt = json_get(a, "count");
    const hal_json *type = json_get(a, "type");
    if (!ct || !cnt || !type || ct->type != HAL_JSON_NUMBER ||
        cnt->type != HAL_JSON_NUMBER || type->type != HAL_JSON_STRING) {
        io_err(err, err_cap, "glTF: malformed accessor %zu", index);
        return false;
    }
    out->component_type = (hal_u32)ct->number;
    out->count = (size_t)cnt->number;
    out->byte_offset = 0;
    const hal_json *bo = json_get(a, "byteOffset");
    if (bo && bo->type == HAL_JSON_NUMBER)
        out->byte_offset = (size_t)bo->number;
    out->vec_len = strcmp(type->string, "SCALAR") == 0
                       ? 1
                       : strcmp(type->string, "VEC2") == 0   ? 2
                       : strcmp(type->string, "VEC3") == 0   ? 3
                       : strcmp(type->string, "VEC4") == 0   ? 4
                       : strcmp(type->string, "MAT4") == 0   ? 16
                                                            : -1;
    if (out->vec_len < 0) {
        io_err(err, err_cap,
               "glTF: unsupported accessor type %s", type->string);
        return false;
    }

    if (!bv) {
        /* Sparse-free no-bufferView accessor: not supported. */
        io_err(err, err_cap, "glTF: accessor without bufferView");
        return false;
    }
    size_t bvi = (size_t)bv->number;
    if (!views || views->type != HAL_JSON_ARRAY ||
        bvi >= views->count) {
        io_err(err, err_cap, "glTF: bufferView %zu out of range", bvi);
        return false;
    }
    const hal_json *v = views->items[bvi];
    const hal_json *vo = json_get(v, "byteOffset");
    const hal_json *vl = json_get(v, "byteLength");
    out->view_offset = vo && vo->type == HAL_JSON_NUMBER
                           ? (size_t)vo->number
                           : 0;
    out->view_length = vl && vl->type == HAL_JSON_NUMBER
                           ? (size_t)vl->number
                           : 0;
    (void)g;
    return true;
}

static size_t acc_comp_size(hal_u32 ct) {
    switch (ct) {
    case 5126: return 4; /* FLOAT        */
    case 5125: return 4; /* UNSIGNED_INT */
    case 5123: return 2; /* UNSIGNED_SHORT */
    case 5121: return 1; /* UNSIGNED_BYTE  */
    default: return 0;
    }
}

/* Reads element `i`, component `c` of an accessor. */
static double acc_read(const glb *g, const acc_info *a, size_t i,
                       int c) {
    size_t comp = acc_comp_size(a->component_type);
    if (!comp)
        return 0.0;
    size_t off = a->view_offset + a->byte_offset +
                 (i * (size_t)a->vec_len + (size_t)c) * comp;
    if (off + comp > g->bin_len)
        return 0.0;
    const unsigned char *p = g->bin + off;
    switch (a->component_type) {
    case 5126: {
        float v;
        memcpy(&v, p, 4);
        return (double)v;
    }
    case 5125: {
        hal_u32 v;
        memcpy(&v, p, 4);
        return (double)v;
    }
    case 5123: {
        hal_u16 v;
        memcpy(&v, p, 2);
        return (double)v;
    }
    case 5121:
        return (double)*p;
    default:
        return 0.0;
    }
}

/* Recursively collects world transforms for nodes with meshes. */


/* Import bookkeeping: which scene object each glTF node produced,
 * and which skin each mesh node referenced. */
typedef struct imp_ctx {
    hal_object_id *node_obj; /* per node index, 0 = none   */
    int *node_skin;          /* per node index, -1 = none  */
    size_t node_count;
} imp_ctx;

static bool collect_nodes(const hal_json *nodes, size_t index,
                          hal_mat4f parent, hal_scene *scene,
                          const hal_json *meshes,
                          const hal_json *accessors,
                          const hal_json *views, const glb *g,
                          imp_ctx *ctx, char *err, size_t err_cap) {
    if (!nodes || nodes->type != HAL_JSON_ARRAY ||
        index >= nodes->count) {
        io_err(err, err_cap, "glTF: node %zu out of range", index);
        return false;
    }
    const hal_json *n = nodes->items[index];

    /* Local TRS. */
    hal_vec3f loc = hal_vec3f_zero();
    hal_quatf rot = hal_quatf_identity();
    hal_vec3f scale = hal_vec3f_one();
    const hal_json *t = json_get(n, "translation");
    if (t)
        loc = hal_vec3f_make((hal_f32)json_num(t, 0, 0),
                             (hal_f32)json_num(t, 1, 0),
                             (hal_f32)json_num(t, 2, 0));
    const hal_json *r = json_get(n, "rotation");
    if (r)
        rot = hal_quatf_normalize(hal_quatf_make(
            (hal_f32)json_num(r, 0, 0), (hal_f32)json_num(r, 1, 0),
            (hal_f32)json_num(r, 2, 0), (hal_f32)json_num(r, 3, 1)));
    const hal_json *sc = json_get(n, "scale");
    if (sc)
        scale = hal_vec3f_make((hal_f32)json_num(sc, 0, 1),
                               (hal_f32)json_num(sc, 1, 1),
                               (hal_f32)json_num(sc, 2, 1));

    hal_mat4f local = hal_mat4f_from_trs(loc, rot, scale);
    hal_mat4f world = hal_mat4f_mul(parent, local);

    const hal_json *mesh_ref = json_get(n, "mesh");
    if (mesh_ref && mesh_ref->type == HAL_JSON_NUMBER && meshes &&
        meshes->type == HAL_JSON_ARRAY) {
        size_t mi = (size_t)mesh_ref->number;
        if (mi >= meshes->count) {
            io_err(err, err_cap, "glTF: mesh %zu out of range", mi);
            return false;
        }
        const hal_json *mesh = meshes->items[mi];
        const hal_json *prims = json_get(mesh, "primitives");
        if (!prims || prims->type != HAL_JSON_ARRAY ||
            prims->count == 0) {
            io_err(err, err_cap, "glTF: mesh %zu has no primitives",
                   mi);
            return false;
        }
        /* First primitive only (Halcyon's own exports have exactly
         * one; multi-primitive meshes import their first). */
        const hal_json *prim = prims->items[0];
        const hal_json *attrs = json_get(prim, "attributes");
        const hal_json *pos_ref = json_get(attrs, "POSITION");
        if (!pos_ref || pos_ref->type != HAL_JSON_NUMBER) {
            io_err(err, err_cap, "glTF: primitive without POSITION");
            return false;
        }

        acc_info pos;
        if (!read_accessor(accessors, (size_t)pos_ref->number, views,
                           g, &pos, err, err_cap))
            return false;
        if (pos.component_type != 5126 || pos.vec_len != 3) {
            io_err(err, err_cap,
                   "glTF: POSITION must be FLOAT VEC3");
            return false;
        }

        const hal_json *name = json_get(n, "name");
        hal_object_id id = hal_scene_object_add(
            scene, name && name->type == HAL_JSON_STRING ? name->string
                                                         : "Imported",
            HAL_OBJECT_MESH);
        hal_mesh *m = hal_mesh_create();
        if (id == HAL_OBJECT_ID_NONE || !m) {
            hal_mesh_destroy(m);
            io_err(err, err_cap, "out of memory");
            return false;
        }
        for (size_t v = 0; v < pos.count; v++)
            hal_mesh_vert_add(m, hal_vec3f_make(
                                     (hal_f32)acc_read(g, &pos, v, 0),
                                     (hal_f32)acc_read(g, &pos, v, 1),
                                     (hal_f32)acc_read(g, &pos, v, 2)));

        const hal_json *idx_ref = json_get(prim, "indices");
        if (idx_ref && idx_ref->type == HAL_JSON_NUMBER) {
            acc_info idx;
            if (!read_accessor(accessors, (size_t)idx_ref->number,
                               views, g, &idx, err, err_cap))
                return false;
            for (size_t t2 = 0; t2 + 2 < idx.count; t2 += 3) {
                hal_u32 tri[3];
                tri[0] = (hal_u32)acc_read(g, &idx, t2, 0);
                tri[1] = (hal_u32)acc_read(g, &idx, t2 + 1, 0);
                tri[2] = (hal_u32)acc_read(g, &idx, t2 + 2, 0);
                hal_mesh_face_add(m, tri, 3);
            }
        } else {
            /* Non-indexed: consecutive triples. */
            for (size_t v = 0; v + 2 < pos.count; v += 3) {
                hal_u32 tri[3] = { (hal_u32)v, (hal_u32)v + 1,
                                   (hal_u32)v + 2 };
                hal_mesh_face_add(m, tri, 3);
            }
        }
        hal_mesh_rebuild_edges(m);

        /* Skin influences (JOINTS_0/WEIGHTS_0): values are positions
         * in the referenced skin's joints array; the skins pass below
         * maps them onto the rebuilt armature's bone indices. */
        const hal_json *jnt_ref = json_get(attrs, "JOINTS_0");
        const hal_json *wgt_ref = json_get(attrs, "WEIGHTS_0");
        if (jnt_ref && wgt_ref && jnt_ref->type == HAL_JSON_NUMBER &&
            wgt_ref->type == HAL_JSON_NUMBER) {
            acc_info jnt, wgt;
            if (read_accessor(accessors, (size_t)jnt_ref->number,
                              views, g, &jnt, err, err_cap) &&
                read_accessor(accessors, (size_t)wgt_ref->number,
                              views, g, &wgt, err, err_cap) &&
                jnt.vec_len == 4 && wgt.vec_len == 4 &&
                jnt.count == wgt.count && jnt.count == pos.count) {
                hal_f32 *w = calloc(pos.count * 4, sizeof(hal_f32));
                hal_i32 *b = malloc(pos.count * 4 * sizeof(hal_i32));
                if (w && b) {
                    for (size_t v = 0; v < pos.count; v++)
                        for (size_t k = 0; k < 4; k++) {
                            b[v * 4 + k] =
                                (hal_i32)acc_read(g, &jnt, v, (int)k);
                            w[v * 4 + k] =
                                (hal_f32)acc_read(g, &wgt, v, (int)k);
                        }
                    free(m->skin_weights);
                    free(m->skin_bones);
                    m->skin_weights = w;
                    m->skin_bones = b;
                } else {
                    free(w);
                    free(b);
                }
            }
        }

        hal_object_set_mesh(scene, id, m);
        if (ctx && index < ctx->node_count)
            ctx->node_obj[index] = id;
        const hal_json *skin_ref = json_get(n, "skin");
        if (ctx && index < ctx->node_count && skin_ref &&
            skin_ref->type == HAL_JSON_NUMBER)
            ctx->node_skin[index] = (int)skin_ref->number;

        /* Bake the world transform back to TRS. */
        hal_vec3f wloc;
        hal_quatf wrot;
        hal_vec3f wscale;
        hal_mat4f_decompose_trs(world, &wloc, &wrot, &wscale);
        hal_object_set_location(scene, id, wloc);
        hal_object_set_rotation(scene, id, wrot);
        hal_object_set_scale(scene, id, wscale);
    }

    const hal_json *children = json_get(n, "children");
    if (children && children->type == HAL_JSON_ARRAY)
        for (size_t c = 0; c < children->count; c++) {
            const hal_json *ci = children->items[c];
            if (!ci || ci->type != HAL_JSON_NUMBER)
                continue;
            if (!collect_nodes(nodes, (size_t)ci->number, world, scene,
                               meshes, accessors, views, g, ctx, err,
                               err_cap))
                return false;
        }
    return true;
}

hal_result hal_io_gltf_import(const char *path, hal_scene **out_scene,
                              char *err, size_t err_cap) {
    if (!path || !out_scene) {
        io_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    *out_scene = NULL;

    /* Read the whole file; the glb struct points into it. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        io_err(err, err_cap, "cannot read %s", path);
        return HAL_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 20) {
        fclose(f);
        io_err(err, err_cap, "%s is not a GLB", path);
        return HAL_ERR_INVALID_ARGUMENT;
    }
    unsigned char *data = malloc((size_t)sz);
    if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        io_err(err, err_cap, "cannot read %s", path);
        return HAL_ERR_IO;
    }
    fclose(f);

    hal_u32 magic, version, length;
    memcpy(&magic, data, 4);
    memcpy(&version, data + 4, 4);
    memcpy(&length, data + 8, 4);
    if (magic != 0x46546C67u || version != 2u ||
        length != (hal_u32)sz) {
        free(data);
        io_err(err, err_cap, "%s is not a glTF 2.0 GLB", path);
        return HAL_ERR_INVALID_ARGUMENT;
    }

    size_t off = 12;
    hal_u32 jlen, jtype;
    memcpy(&jlen, data + off, 4);
    memcpy(&jtype, data + off + 4, 4);
    off += 8;
    if (jtype != 0x4E4F534Au || off + jlen > (size_t)sz) {
        free(data);
        io_err(err, err_cap, "GLB: bad JSON chunk");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    glb g;
    g.json = (const char *)data + off;
    g.json_len = jlen;
    g.bin = NULL;
    g.bin_len = 0;
    off += jlen;
    if (off + 8 <= (size_t)sz) {
        hal_u32 blen, btype;
        memcpy(&blen, data + off, 4);
        memcpy(&btype, data + off + 4, 4);
        off += 8;
        if (btype == 0x004E4942u && off + blen <= (size_t)sz) {
            g.bin = data + off;
            g.bin_len = blen;
        }
    }

    json_parser jp = { .p = g.json, .end = g.json + g.json_len,
                       .depth = 0 };
    hal_json *root = json_parse_value(&jp);
    if (!root) {
        free(data);
        io_err(err, err_cap, "glTF: JSON parse failed");
        return HAL_ERR_INVALID_ARGUMENT;
    }

    hal_scene *scene = hal_scene_create("Imported");
    if (!scene) {
        hal_json_destroy(root);
        free(data);
        io_err(err, err_cap, "out of memory");
        return HAL_ERR_OUT_OF_MEMORY;
    }

    const hal_json *nodes = json_get(root, "nodes");
    const hal_json *meshes = json_get(root, "meshes");
    const hal_json *accessors = json_get(root, "accessors");
    const hal_json *views = json_get(root, "bufferViews");
    const hal_json *skins = json_get(root, "skins");
    const hal_json *anims = json_get(root, "animations");
    const hal_json *scenes = json_get(root, "scenes");
    const hal_json *scene0 = scenes ? json_get(scenes->type ==
                                                       HAL_JSON_ARRAY
                                                   ? scenes->items[0]
                                                   : NULL,
                                               "nodes")
                                    : NULL;

    /* Import bookkeeping (node -> object / skin). */
    imp_ctx ictx = { 0 };
    size_t node_count = nodes && nodes->type == HAL_JSON_ARRAY
                            ? nodes->count
                            : 0;
    ictx.node_count = node_count;
    ictx.node_obj =
        calloc(node_count ? node_count : 1, sizeof(hal_object_id));
    ictx.node_skin = malloc((node_count ? node_count : 1) * sizeof(int));
    bool ctx_ok = ictx.node_obj && ictx.node_skin;
    if (ctx_ok)
        for (size_t i = 0; i < node_count; i++) {
            ictx.node_obj[i] = HAL_OBJECT_ID_NONE;
            ictx.node_skin[i] = -1;
        }

    bool ok = ctx_ok;
    if (nodes && nodes->type == HAL_JSON_ARRAY) {
        if (scene0 && scene0->type == HAL_JSON_ARRAY) {
            for (size_t i = 0; ok && i < scene0->count; i++) {
                const hal_json *ni = scene0->items[i];
                if (ni && ni->type == HAL_JSON_NUMBER)
                    ok = collect_nodes(nodes, (size_t)ni->number,
                                       hal_mat4f_identity(), scene,
                                       meshes, accessors, views, &g,
                                       &ictx, err, err_cap);
            }
        } else {
            /* No scene declaration: import every node. */
            for (size_t i = 0; ok && i < nodes->count; i++)
                ok = collect_nodes(nodes, i, hal_mat4f_identity(),
                                   scene, meshes, accessors, views,
                                   &g, &ictx, err, err_cap);
        }
    }

    /* ---- Skins pass: rebuild armatures from joint nodes and attach
     * the imported weights. Joint world translations become bone
     * heads (glTF joints carry no tails — a +Y stub is synthesized);
     * joint local rotations become bone poses. NOTE (honest): the
     * file's inverseBindMatrices are not consulted — Halcyon derives
     * bind matrices from the rebuilt REST pose, which matches files
     * exported by Halcyon exactly and approximates foreign files
     * whose joints have rest rotations. */
    if (ok && skins && skins->type == HAL_JSON_ARRAY) {
        /* parent-of map from children lists. */
        int *parent_of =
            malloc((node_count ? node_count : 1) * sizeof(int));
        hal_mat4f *world =
            malloc((node_count ? node_count : 1) * sizeof(hal_mat4f));
        if (parent_of && world) {
            for (size_t i = 0; i < node_count; i++)
                parent_of[i] = -1;
            for (size_t i = 0; i < node_count; i++) {
                const hal_json *n = nodes->items[i];
                const hal_json *ch = json_get(n, "children");
                if (ch && ch->type == HAL_JSON_ARRAY)
                    for (size_t c = 0; c < ch->count; c++)
                        if (ch->items[c] &&
                            ch->items[c]->type == HAL_JSON_NUMBER) {
                            size_t ci2 =
                                (size_t)ch->items[c]->number;
                            if (ci2 < node_count)
                                parent_of[ci2] = (int)i;
                        }
            }
            /* Worlds via memoized recursion (iterative with a stack
             * to keep depth bounded). */
            for (size_t i = 0; i < node_count; i++)
                world[i] = hal_mat4f_identity();
            bool *done = calloc(node_count ? node_count : 1, sizeof(bool));
            if (done) {
                size_t *stack =
                    malloc((node_count ? node_count : 1) * sizeof(size_t));
                if (stack) {
                    for (size_t root_n = 0; root_n < node_count;
                         root_n++) {
                        if (parent_of[root_n] != -1)
                            continue;
                        /* depth-first with an explicit stack; children
                         * are pushed after computing the parent world
                         * (post-order would be wrong; do pre-order). */
                        size_t sp = 0;
                        stack[sp++] = root_n;
                        while (sp > 0) {
                            size_t ni = stack[--sp];
                            if (done[ni])
                                continue;
                            const hal_json *n = nodes->items[ni];
                            hal_vec3f loc = hal_vec3f_zero();
                            hal_quatf rot = hal_quatf_identity();
                            hal_vec3f scl = hal_vec3f_one();
                            const hal_json *t = json_get(n, "translation");
                            if (t)
                                loc = hal_vec3f_make(
                                    (hal_f32)json_num(t, 0, 0),
                                    (hal_f32)json_num(t, 1, 0),
                                    (hal_f32)json_num(t, 2, 0));
                            const hal_json *r = json_get(n, "rotation");
                            if (r)
                                rot = hal_quatf_make(
                                    (hal_f32)json_num(r, 0, 0),
                                    (hal_f32)json_num(r, 1, 0),
                                    (hal_f32)json_num(r, 2, 0),
                                    (hal_f32)json_num(r, 3, 1));
                            const hal_json *sc = json_get(n, "scale");
                            if (sc)
                                scl = hal_vec3f_make(
                                    (hal_f32)json_num(sc, 0, 1),
                                    (hal_f32)json_num(sc, 1, 1),
                                    (hal_f32)json_num(sc, 2, 1));
                            hal_mat4f local =
                                hal_mat4f_from_trs(loc, rot, scl);
                            hal_mat4f parent_m =
                                parent_of[ni] >= 0 &&
                                        (size_t)parent_of[ni] < node_count
                                    ? world[parent_of[ni]]
                                    : hal_mat4f_identity();
                            world[ni] =
                                hal_mat4f_mul(parent_m, local);
                            done[ni] = true;
                            const hal_json *ch = json_get(n, "children");
                            if (ch && ch->type == HAL_JSON_ARRAY)
                                for (size_t c = 0; c < ch->count; c++)
                                    if (ch->items[c] &&
                                        ch->items[c]->type ==
                                            HAL_JSON_NUMBER) {
                                        size_t ci2 =
                                            (size_t)ch->items[c]->number;
                                        if (ci2 < node_count && !done[ci2])
                                            stack[sp++] = ci2;
                                    }
                        }
                    }
                    free(stack);
                }
                free(done);
            }

            for (size_t s = 0; ok && s < skins->count; s++) {
                const hal_json *sk = skins->items[s];
                const hal_json *joints = json_get(sk, "joints");
                if (!joints || joints->type != HAL_JSON_ARRAY ||
                    joints->count == 0 || joints->count > 255)
                    continue;
                size_t bone_count = joints->count;

                const hal_json *skel =
                    json_get(sk, "skeleton");
                const char *arm_name = "Armature";
                if (skel && skel->type == HAL_JSON_NUMBER &&
                    (size_t)skel->number < node_count) {
                    const hal_json *n =
                        nodes->items[(size_t)skel->number];
                    const hal_json *nm = json_get(n, "name");
                    if (nm && nm->type == HAL_JSON_STRING)
                        arm_name = nm->string;
                }
                /* The armature object's world transform: the ROOT
                 * joint's parent chain (the armature node). Halcyon
                 * exports put the armature node above the root joints
                 * with world = A, and joint locals = T(armature-space
                 * head) * R(pose) — so the armature world is the
                 * root joint's world minus its own head translation,
                 * and bones import with their LOCAL translations as
                 * armature-space heads (exact for Halcyon exports;
                 * foreign skins, whose joint translations are usually
                 * parent-relative, import approximately). */
                /* The root joint: the first joint whose parent is not
                 * itself a joint of this skin (usually the armature
                 * node — parent_of is NOT -1 for it). Needs bone_of,
                 * so it is computed after the map below. */
                size_t root_node = (size_t)-1;
                hal_object_id arm_id = hal_scene_object_add(
                    scene, arm_name, HAL_OBJECT_ARMATURE);
                hal_armature *arm = hal_armature_create();
                hal_object_set_armature(scene, arm_id, arm);
                if (!arm)
                    continue;

                /* node index -> bone index within this skin. */
                int *bone_of =
                    malloc((node_count ? node_count : 1) * sizeof(int));
                if (!bone_of)
                    continue;
                for (size_t i = 0; i < node_count; i++)
                    bone_of[i] = -1;
                for (size_t b = 0; b < bone_count; b++) {
                    const hal_json *jn = joints->items[b];
                    if (jn && jn->type == HAL_JSON_NUMBER &&
                        (size_t)jn->number < node_count)
                        bone_of[(size_t)jn->number] = (int)b;
                }

                for (size_t b = 0; b < bone_count; b++) {
                    const hal_json *jn = joints->items[b];
                    if (!jn || jn->type != HAL_JSON_NUMBER)
                        continue;
                    size_t ni = (size_t)jn->number;
                    if (ni >= node_count)
                        continue;
                    if (parent_of[ni] < 0 ||
                        bone_of[parent_of[ni]] < 0) {
                        root_node = ni;
                        break;
                    }
                }
                if (root_node != (size_t)-1 && root_node < node_count) {
                    const hal_json *rn = nodes->items[root_node];
                    const hal_json *rt = json_get(rn, "translation");
                    hal_vec3f head0 = hal_vec3f_zero();
                    if (rt)
                        head0 = hal_vec3f_make(
                            (hal_f32)json_num(rt, 0, 0),
                            (hal_f32)json_num(rt, 1, 0),
                            (hal_f32)json_num(rt, 2, 0));
                    /* arm_world = world[root] * T(-head0) */
                    hal_mat4f arm_world = world[root_node];
                    hal_mat4f unshift = hal_mat4f_identity();
                    unshift.m[12] = -head0.x;
                    unshift.m[13] = -head0.y;
                    unshift.m[14] = -head0.z;
                    arm_world = hal_mat4f_mul(arm_world, unshift);
                    hal_vec3f aloc;
                    hal_quatf arot;
                    hal_vec3f ascale;
                    hal_mat4f_decompose_trs(arm_world, &aloc, &arot,
                                            &ascale);
                    hal_object_set_location(scene, arm_id, aloc);
                    hal_object_set_rotation(scene, arm_id, arot);
                    hal_object_set_scale(scene, arm_id, ascale);
                }

                for (size_t b = 0; b < bone_count; b++) {
                    const hal_json *jn = joints->items[b];
                    if (!jn || jn->type != HAL_JSON_NUMBER ||
                        (size_t)jn->number >= node_count)
                        continue;
                    size_t ni = (size_t)jn->number;
                    const hal_json *n = nodes->items[ni];
                    /* Armature-space head = the joint's LOCAL
                     * translation (Halcyon's export convention). */
                    hal_vec3f head = hal_vec3f_zero();
                    const hal_json *lt = json_get(n, "translation");
                    if (lt)
                        head = hal_vec3f_make(
                            (hal_f32)json_num(lt, 0, 0),
                            (hal_f32)json_num(lt, 1, 0),
                            (hal_f32)json_num(lt, 2, 0));
                    int parent_bone =
                        parent_of[ni] >= 0 ? bone_of[parent_of[ni]] : -1;
                    hal_quatf pose = hal_quatf_identity();
                    const hal_json *r = json_get(n, "rotation");
                    if (r)
                        pose = hal_quatf_make(
                            (hal_f32)json_num(r, 0, 0),
                            (hal_f32)json_num(r, 1, 0),
                            (hal_f32)json_num(r, 2, 0),
                            (hal_f32)json_num(r, 3, 1));
                    char bname[64];
                    const hal_json *nm = json_get(n, "name");
                    snprintf(bname, sizeof(bname), "%s",
                             nm && nm->type == HAL_JSON_STRING
                                 ? nm->string
                                 : "Bone");
                    hal_armature_bone_add(
                        arm, bname, head,
                        hal_vec3f_add(head,
                                      hal_vec3f_make(0, 0.1f, 0)),
                        parent_bone);
                    arm->bones[b].pose = pose;
                }

                /* Attach every mesh node referencing this skin. */
                for (size_t ni = 0; ni < node_count; ni++) {
                    if (ni >= ictx.node_count ||
                        ictx.node_skin[ni] != (int)s ||
                        ictx.node_obj[ni] == HAL_OBJECT_ID_NONE)
                        continue;
                    hal_object *mo =
                        hal_scene_object(scene, ictx.node_obj[ni]);
                    if (mo && mo->mesh) {
                        mo->skin_armature = arm_id;
                        /* JOINTS_0 values are positions in the joints
                         * array == bone indices already. */
                    }
                }
                /* Bone animation channels for this armature (uses
                 * bone_of below; freed after). */
                if (anims && anims->type == HAL_JSON_ARRAY)
                    for (size_t a = 0; a < anims->count; a++) {
                        const hal_json *an = anims->items[a];
                        const hal_json *channels =
                            json_get(an, "channels");
                        const hal_json *samplers =
                            json_get(an, "samplers");
                        const hal_json *aname =
                            json_get(an, "name");
                        if (!channels || !samplers ||
                            channels->type != HAL_JSON_ARRAY ||
                            samplers->type != HAL_JSON_ARRAY)
                            continue;
                        int clip_idx = hal_scene_clip_add(
                            scene, aname && aname->type == HAL_JSON_STRING
                                        ? aname->string
                                        : "Imported");
                        hal_clip *clip =
                            hal_scene_clip(scene, clip_idx);
                        if (!clip)
                            continue;
                        clip->end = 0.0f;
                        for (size_t c = 0; c < channels->count;
                             c++) {
                            const hal_json *ch = channels->items[c];
                            const hal_json *sref = json_get(ch, "sampler");
                            const hal_json *tgt = json_get(ch, "target");
                            const hal_json *nref =
                                tgt ? json_get(tgt, "node") : NULL;
                            const hal_json *ch_path =
                                tgt ? json_get(tgt, "path") : NULL;
                            if (!sref || !nref || !ch_path ||
                                sref->type != HAL_JSON_NUMBER ||
                                nref->type != HAL_JSON_NUMBER ||
                                ch_path->type != HAL_JSON_STRING)
                                continue;
                            size_t target_node =
                                (size_t)nref->number;
                            if (target_node >= node_count ||
                                bone_of[target_node] < 0)
                                continue; /* not a joint here */
                            int bone = bone_of[target_node];
                            if (strcmp(ch_path->string, "rotation") != 0)
                                continue; /* only joint rotations */
                            size_t sidx = (size_t)sref->number;
                            if (sidx >= samplers->count)
                                continue;
                            const hal_json *sm =
                                samplers->items[sidx];
                            const hal_json *iref = json_get(sm, "input");
                            const hal_json *oref =
                                json_get(sm, "output");
                            if (!iref || !oref ||
                                iref->type != HAL_JSON_NUMBER ||
                                oref->type != HAL_JSON_NUMBER)
                                continue;
                            acc_info in, out;
                            if (!read_accessor(accessors,
                                               (size_t)iref->number,
                                               views, &g, &in, err,
                                               err_cap) ||
                                !read_accessor(accessors,
                                               (size_t)oref->number,
                                               views, &g, &out, err,
                                               err_cap) ||
                                in.vec_len != 1 || out.vec_len != 4)
                                continue;
                            for (size_t k = 0; k < in.count &&
                                             k < out.count;
                                 k++) {
                                hal_f32 t = (hal_f32)acc_read(&g, &in, k, 0);
                                hal_quatf q = hal_quatf_make(
                                    (hal_f32)acc_read(&g, &out, k, 0),
                                    (hal_f32)acc_read(&g, &out, k, 1),
                                    (hal_f32)acc_read(&g, &out, k, 2),
                                    (hal_f32)acc_read(&g, &out, k, 3));
                                hal_clip_key_rot(
                                    clip, arm_id, bone, t, q,
                                    HAL_INTERP_LINEAR);
                            }
                        }
                    }
                free(bone_of);
            }
            free(parent_of);
            free(world);
        } else {
            free(parent_of);
            free(world);
        }
    }

    hal_json_destroy(root);
    free(data);
    free(ictx.node_obj);
    free(ictx.node_skin);

    if (!ok) {
        hal_scene_destroy(scene);
        return HAL_ERR_INVALID_ARGUMENT;
    }
    *out_scene = scene;
    return HAL_OK;
}

hal_result hal_io_gltf_export(hal_scene *scene, const char *path,
                              char *err, size_t err_cap) {
    return hal_io_gltf_export_ex(scene, path, NULL, err, err_cap);
}
