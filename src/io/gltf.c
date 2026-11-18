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

    /* Per-corner UVs force a duplicated stream even for smooth
     * shading (seams must survive); otherwise smooth meshes share
     * the original verts. */
    bool has_uv = m->uvs != NULL && m->uvs_valid;
    bool smooth = vn != NULL && !has_uv;
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
    *out_tri_count = tri_count;
    *out_vert_count = out_verts;
    *out_min = bmin;
    *out_max = bmax;
    return true;
}

hal_result hal_io_gltf_export(hal_scene *scene, const char *path,
                              char *err, size_t err_cap) {
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
    } mesh_blocks;
    mesh_blocks *blocks =
        calloc(mesh_count ? mesh_count : 1, sizeof(mesh_blocks));
    if (!blocks) {
        io_err(err, err_cap, "out of memory");
        return HAL_ERR_OUT_OF_MEMORY;
    }

    size_t bi = 0;
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->mesh)
            continue;
        if (!gltf_write_mesh(obj, &bin, &blocks[bi].pos_off,
                             &blocks[bi].nrm_off, &blocks[bi].uv_off,
                             &blocks[bi].idx_off, &blocks[bi].has_uv,
                             &blocks[bi].tri_count,
                             &blocks[bi].vert_count, &blocks[bi].bmin,
                             &blocks[bi].bmax)) {
            free(bin.data);
            free(blocks);
            io_err(err, err_cap, "out of memory while writing buffers");
            return HAL_ERR_OUT_OF_MEMORY;
        }
        blocks[bi].material = obj->material;
        bi++;
    }

    /* Sequential accessor / bufferView indices (UV slots exist only
     * for meshes with UVs, so fixed strides would misnumber mixed
     * scenes). */
    {
        size_t acc = 0, view = 0;
        for (size_t mi = 0; mi < mesh_count; mi++) {
            blocks[mi].a_pos = acc++;
            blocks[mi].a_nrm = acc++;
            blocks[mi].a_uv = blocks[mi].has_uv ? acc++ : (size_t)-1;
            blocks[mi].a_idx = acc++;
            blocks[mi].v_pos = view++;
            blocks[mi].v_nrm = view++;
            blocks[mi].v_uv = blocks[mi].has_uv ? view++ : (size_t)-1;
            blocks[mi].v_idx = view++;
        }
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
        /* children array once, listing every object parented here. */
        first = true;
        for (size_t c = 0; c < scene->count; c++)
            if (scene->objects[c].parent == o->id) {
                if (first) {
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
                    if (!buf_puts(&js, "]"))
                        goto oom;
                    first = false;
                }
                break;
            }
        if (o->mesh) {
            size_t j = 0;
            for (size_t k = 0; k <= i; k++)
                if (scene->objects[k].mesh)
                    j++;
            if (!buf_putf(&js, ",\"mesh\":%zu", j - 1))
                goto oom;
        }
        if (!buf_putf(&js,
                      ",\"translation\":[%.9g,%.9g,%.9g],"
                      "\"rotation\":[%.9g,%.9g,%.9g,%.9g],"
                      "\"scale\":[%.9g,%.9g,%.9g]",
                      (double)o->location.x, (double)o->location.y,
                      (double)o->location.z, (double)o->rotation.x,
                      (double)o->rotation.y, (double)o->rotation.z,
                      (double)o->rotation.w, (double)o->scale.x,
                      (double)o->scale.y, (double)o->scale.z))
            goto oom;
        if (!buf_puts(&js, "}"))
            goto oom;
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
    }
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
                size_t view = mesh_count * 3 + k;
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
    out->vec_len = strcmp(type->string, "VEC3") == 0
                       ? 3
                       : strcmp(type->string, "SCALAR") == 0 ? 1 : -1;
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


static bool collect_nodes(const hal_json *nodes, size_t index,
                          hal_mat4f parent, hal_scene *scene,
                          const hal_json *meshes,
                          const hal_json *accessors,
                          const hal_json *views, const glb *g,
                          char *err, size_t err_cap) {
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
        hal_object_set_mesh(scene, id, m);

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
                               meshes, accessors, views, g, err,
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
    const hal_json *scenes = json_get(root, "scenes");
    const hal_json *scene0 = scenes ? json_get(scenes->type ==
                                                       HAL_JSON_ARRAY
                                                   ? scenes->items[0]
                                                   : NULL,
                                               "nodes")
                                    : NULL;

    bool ok = true;
    if (nodes && nodes->type == HAL_JSON_ARRAY) {
        if (scene0 && scene0->type == HAL_JSON_ARRAY) {
            for (size_t i = 0; ok && i < scene0->count; i++) {
                const hal_json *ni = scene0->items[i];
                if (ni && ni->type == HAL_JSON_NUMBER)
                    ok = collect_nodes(nodes, (size_t)ni->number,
                                       hal_mat4f_identity(), scene,
                                       meshes, accessors, views, &g,
                                       err, err_cap);
            }
        } else {
            /* No scene declaration: import every node. */
            for (size_t i = 0; ok && i < nodes->count; i++)
                ok = collect_nodes(nodes, i, hal_mat4f_identity(),
                                   scene, meshes, accessors, views,
                                   &g, err, err_cap);
        }
    }

    hal_json_destroy(root);
    free(data);

    if (!ok) {
        hal_scene_destroy(scene);
        return HAL_ERR_INVALID_ARGUMENT;
    }
    *out_scene = scene;
    return HAL_OK;
}
