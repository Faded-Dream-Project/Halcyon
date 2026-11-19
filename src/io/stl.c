/*
 * stl.c — binary STL export
 *
 * STL carries one solid: every visible mesh object's triangles baked
 * to world space, with recomputed facet normals (the format's normal
 * field is advisory; we write the true geometric normal).
 */

#include "io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stl_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

hal_result hal_io_stl_export(hal_scene *scene, const char *path,
                             char *err, size_t err_cap) {
    if (!scene || !path) {
        stl_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }

    /* Count triangles first (fan triangulation). */
    hal_u32 tri_count = 0;
    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->visible || !obj->mesh)
            continue;
        hal_mesh *m = obj->mesh;
        for (size_t f = 0; f < m->face_count; f++) {
            size_t n = m->face_off[f + 1] - m->face_off[f];
            if (n >= 3)
                tri_count += (hal_u32)(n - 2);
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        stl_err(err, err_cap, "cannot write %s", path);
        return HAL_ERR_IO;
    }

    char header[80];
    memset(header, 0, sizeof(header));
    strcpy(header, "Halcyon STL export");
    bool ok = fwrite(header, sizeof(header), 1, f) == 1;
    ok = ok && fwrite(&tri_count, sizeof(tri_count), 1, f) == 1;

    for (size_t i = 0; ok && i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->visible || !obj->mesh)
            continue;
        hal_mesh *m = obj->mesh;
        const hal_vec3f *fn = hal_mesh_face_normals(m);
        if (!fn) {
            ok = false;
            break;
        }
        hal_mat4f world = hal_object_world_matrix((hal_scene *)scene,
                                                  obj);
        for (size_t fi = 0; ok && fi < m->face_count; fi++) {
            size_t n = m->face_off[fi + 1] - m->face_off[fi];
            for (size_t k = 0; ok && k + 2 < n; k++) {
                hal_vec3f v[3] = {
                    hal_mat4f_transform_point(
                        world, m->verts[m->loops[m->face_off[fi]]]),
                    hal_mat4f_transform_point(
                        world,
                        m->verts[m->loops[m->face_off[fi] + k + 1]]),
                    hal_mat4f_transform_point(
                        world,
                        m->verts[m->loops[m->face_off[fi] + k + 2]]),
                };
                /* World-space facet normal from the baked winding. */
                hal_vec3f nrm = hal_vec3f_normalize(hal_vec3f_cross(
                    hal_vec3f_sub(v[1], v[0]),
                    hal_vec3f_sub(v[2], v[0])));
                float out[12] = {
                    nrm.x, nrm.y, nrm.z,
                    v[0].x, v[0].y, v[0].z,
                    v[1].x, v[1].y, v[1].z,
                    v[2].x, v[2].y, v[2].z,
                };
                ok = fwrite(out, sizeof(float), 12, f) == 12;
                hal_u16 attr = 0;
                ok = ok && fwrite(&attr, sizeof(attr), 1, f) == 1;
            }
        }
    }

    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        stl_err(err, err_cap, "write failed");
        return HAL_ERR_IO;
    }
    return HAL_OK;
}
