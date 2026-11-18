/*
 * obj.c — Wavefront OBJ export and import
 *
 * Export: one 'o' group per mesh object, a single global vertex index
 * space (OBJ is 1-based), polygon faces written as-is (OBJ allows
 * n-gons). Transforms are LOCAL (OBJ has no node system; the standard
 * exchange is object-local geometry).
 *
 * Import: v / f / o lines; negative indices (relative) supported;
 * unknown lines skipped; faces may be polygons.
 */

#include "io.h"

#include "../core/mesh_ops.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void obj_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

hal_result hal_io_obj_export(const hal_scene *scene, const char *path,
                             char *err, size_t err_cap) {
    if (!scene || !path) {
        obj_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        obj_err(err, err_cap, "cannot write %s", path);
        return HAL_ERR_IO;
    }

    fprintf(f, "# Halcyon OBJ export\n");
    hal_u32 vert_base = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < scene->count; i++) {
        hal_object *obj = (hal_object *)&scene->objects[i];
        if (!obj->mesh)
            continue;
        hal_mesh *m = obj->mesh;
        ok = ok && fprintf(f, "o %s\n", obj->name) > 0;
        for (size_t v = 0; ok && v < m->vert_count; v++)
            ok = fprintf(f, "v %.9g %.9g %.9g\n",
                         (double)m->verts[v].x, (double)m->verts[v].y,
                         (double)m->verts[v].z) > 0;
        for (size_t fi = 0; ok && fi < m->face_count; fi++) {
            size_t n = m->face_off[fi + 1] - m->face_off[fi];
            ok = ok && fprintf(f, "f") > 0;
            for (size_t l = 0; ok && l < n; l++)
                ok = fprintf(f, " %u",
                             vert_base + 1u +
                                 m->loops[m->face_off[fi] + l]) > 0;
            ok = ok && fprintf(f, "\n") > 0;
        }
        vert_base += (hal_u32)m->vert_count;
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        obj_err(err, err_cap, "write failed");
        return HAL_ERR_IO;
    }
    return HAL_OK;
}

hal_result hal_io_obj_import(const char *path, hal_scene **out_scene,
                             char *err, size_t err_cap) {
    if (!path || !out_scene) {
        obj_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    *out_scene = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        obj_err(err, err_cap, "cannot read %s", path);
        return HAL_ERR_IO;
    }

    hal_scene *scene = hal_scene_create("Imported");
    hal_mesh *cur = NULL;
    hal_object_id cur_id = HAL_OBJECT_ID_NONE;
    /* OBJ numbers vertices GLOBALLY across 'o' groups; each object
     * remembers the global base at which its own verts start. */
    size_t total_verts = 0;
    size_t obj_vert_base = 0;

    char line[1024];
    bool ok = true;
    int line_no = 0;
    while (ok && fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
            p[--len] = '\0';
        if (len == 0 || p[0] == '#')
            continue;

        if (strncmp(p, "v ", 2) == 0) {
            if (!cur) {
                cur_id = hal_scene_object_add(scene, "Object",
                                              HAL_OBJECT_MESH);
                cur = hal_mesh_create();
                hal_object_set_mesh(scene, cur_id, cur);
                obj_vert_base = total_verts;
            }
            float x, y, z;
            if (sscanf(p + 2, "%f %f %f", &x, &y, &z) != 3) {
                obj_err(err, err_cap, "line %d: bad vertex", line_no);
                ok = false;
                break;
            }
            hal_mesh_vert_add(cur, hal_vec3f_make(x, y, z));
            total_verts++;
        } else if (strncmp(p, "f ", 2) == 0) {
            if (!cur) {
                obj_err(err, err_cap,
                        "line %d: face before any vertex", line_no);
                ok = false;
                break;
            }
            /* Corners: indices may be v or v/vt or v/vt/vn or v//vn;
             * negative = relative to the current vertex count. */
            hal_u32 corners[64];
            size_t n = 0;
            char *tok = p + 2;
            while (tok && n < 64) {
                while (*tok == ' ' || *tok == '\t')
                    tok++;
                if (*tok == '\0')
                    break;
                long idx = strtol(tok, &tok, 10);
                if (idx == 0)
                    break;
                hal_i32 v;
                if (idx < 0) /* relative to the running total */
                    v = (hal_i32)total_verts + (hal_i32)idx -
                        (hal_i32)obj_vert_base;
                else /* global 1-based -> this object's local 0-based */
                    v = (hal_i32)idx - 1 - (hal_i32)obj_vert_base;
                if (v < 0 ||
                    (size_t)v >= cur->vert_count) {
                    obj_err(err, err_cap,
                            "line %d: face index %ld out of range",
                            line_no, idx);
                    ok = false;
                    break;
                }
                corners[n++] = (hal_u32)v;
                /* Advance to the next corner; any /vt/vn suffix sits
                 * before the next whitespace, so one strpbrk skips it. */
                tok = strpbrk(tok, " \t");
            }
            if (!ok)
                break;
            if (n >= 3)
                hal_mesh_face_add(cur, corners, n);
        } else if (strncmp(p, "o ", 2) == 0) {
            char name[128];
            if (sscanf(p + 2, "%127s", name) == 1) {
                cur_id = hal_scene_object_add(scene, name,
                                              HAL_OBJECT_MESH);
                cur = hal_mesh_create();
                hal_object_set_mesh(scene, cur_id, cur);
                obj_vert_base = total_verts;
            }
        }
        /* vn/vt/mtllib/usemtl/s/l: skipped (positions and faces are
         * the geometry; materials arrive with M6). */
    }
    fclose(f);

    if (!ok) {
        hal_scene_destroy(scene);
        return HAL_ERR_INVALID_ARGUMENT;
    }

    /* Rebuild edges on every object. */
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].mesh)
            hal_mesh_rebuild_edges(scene->objects[i].mesh);

    *out_scene = scene;
    return HAL_OK;
}
