/*
 * uv.c — unwrap implementations
 */

#include "uv.h"

#include <math.h>

#include <stdlib.h>
#include <string.h>

/* Projects a point onto the 2D plane that drops `axis`, returning
 * (u, v). */
static hal_vec2f project2(hal_vec3f p, int axis) {
    switch (axis) {
    case 0: return (hal_vec2f){ p.y, p.z }; /* drop X */
    case 1: return (hal_vec2f){ p.x, p.z }; /* drop Y */
    default: return (hal_vec2f){ p.x, p.y }; /* drop Z */
    }
}

/* Writes `count` UVs (already computed in `tmp`), normalized into
 * [0,1] over their own bounds and placed into the rect
 * (cell_x, cell_y, cell_w, cell_h) of UV space. */
static void place_uv(hal_vec2f *dst, const hal_vec2f *tmp, size_t count,
                     hal_f32 cell_x, hal_f32 cell_y, hal_f32 cell_w,
                     hal_f32 cell_h) {
    if (count == 0)
        return;
    hal_vec2f bmin = tmp[0], bmax = tmp[0];
    for (size_t i = 1; i < count; i++) {
        if (tmp[i].x < bmin.x) bmin.x = tmp[i].x;
        if (tmp[i].y < bmin.y) bmin.y = tmp[i].y;
        if (tmp[i].x > bmax.x) bmax.x = tmp[i].x;
        if (tmp[i].y > bmax.y) bmax.y = tmp[i].y;
    }
    hal_f32 w = bmax.x - bmin.x;
    hal_f32 h = bmax.y - bmin.y;
    if (w < 1e-9f)
        w = 1.0f;
    if (h < 1e-9f)
        h = 1.0f;
    /* Keep the aspect: scale by the larger side so the island is not
     * stretched inside its cell. */
    hal_f32 s = w > h ? cell_w / w : cell_h / h;
    for (size_t i = 0; i < count; i++)
        dst[i] = (hal_vec2f){
            cell_x + (tmp[i].x - bmin.x) * s,
            cell_y + (tmp[i].y - bmin.y) * s
        };
}

bool hal_mesh_unwrap_planar(hal_mesh *m, hal_uv_axis axis) {
    if (!m || m->loop_count == 0)
        return false;
    if (!m->uvs)
        return false; /* grown with the loops; present once faces exist */

    hal_vec2f *tmp = malloc(m->loop_count * sizeof(*tmp));
    if (!tmp)
        return false;
    for (size_t l = 0; l < m->loop_count; l++)
        tmp[l] = project2(m->verts[m->loops[l]], (int)axis);
    place_uv(m->uvs, tmp, m->loop_count, 0.0f, 0.0f, 1.0f, 1.0f);
    free(tmp);
    m->uvs_valid = true;
    return true;
}

/* The dominant axis of a normal (0/1/2) and its sign. */
static int dominant_axis(hal_vec3f n, hal_f32 *out_sign) {
    hal_f32 ax = n.x < 0 ? -n.x : n.x;
    hal_f32 ay = n.y < 0 ? -n.y : n.y;
    hal_f32 az = n.z < 0 ? -n.z : n.z;
    if (ax >= ay && ax >= az) {
        *out_sign = n.x >= 0 ? 1.0f : -1.0f;
        return 0;
    }
    if (ay >= az) {
        *out_sign = n.y >= 0 ? 1.0f : -1.0f;
        return 1;
    }
    *out_sign = n.z >= 0 ? 1.0f : -1.0f;
    return 2;
}

bool hal_mesh_unwrap_box(hal_mesh *m) {
    if (!m || m->face_count == 0 || !m->uvs)
        return false;
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    if (!fn)
        return false;

    /* Six groups (+X -X +Y -Y +Z -Z); each normalized into a 3x2
     * grid cell. */
    hal_vec2f *tmp = malloc(m->loop_count * sizeof(*tmp));
    size_t *group_loops = malloc(m->loop_count * sizeof(size_t));
    if (!tmp || !group_loops) {
        free(tmp);
        free(group_loops);
        return false;
    }

    for (int axis = 0; axis < 3; axis++) {
        for (int sign = 0; sign < 2; sign++) {
            size_t n = 0;
            for (size_t f = 0; f < m->face_count; f++) {
                hal_f32 s;
                int ax = dominant_axis(fn[f], &s);
                if (ax != axis || (s > 0) != (sign == 0))
                    continue;
                for (size_t l = m->face_off[f];
                     l < m->face_off[f + 1]; l++) {
                    tmp[n] = project2(m->verts[m->loops[l]], axis);
                    group_loops[n] = l;
                    n++;
                }
            }
            if (n == 0)
                continue;
            /* Cell: column = axis, row = sign; 3 columns x 2 rows. */
            hal_f32 cw = 1.0f / 3.0f, ch = 1.0f / 2.0f;
            hal_f32 cx = (hal_f32)axis * cw;
            hal_f32 cy = sign == 0 ? 0.0f : ch;
            hal_vec2f *dst = malloc(n * sizeof(*dst));
            if (!dst)
                continue;
            place_uv(dst, tmp, n, cx + 0.01f * cw, cy + 0.01f * ch,
                     cw * 0.98f, ch * 0.98f);
            for (size_t i = 0; i < n; i++)
                m->uvs[group_loops[i]] = dst[i];
            free(dst);
        }
    }
    free(tmp);
    free(group_loops);
    m->uvs_valid = true;
    return true;
}

bool hal_mesh_unwrap_smart(hal_mesh *m) {
    if (!m || m->face_count == 0 || !m->uvs)
        return false;
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    if (!fn)
        return false;

    /* Face -> island id; BFS across shared edges with normal
     * similarity. Edge sharing: a vert->face adjacency is enough for
     * a lite pass (faces sharing any vert with agreeing normals join
     * — coarser than edge-adjacency but far cheaper). */
    int *island = malloc(m->face_count * sizeof(int));
    int *queue = malloc(m->face_count * sizeof(int));
    hal_vec2f *tmp = malloc(m->loop_count * sizeof(*tmp));
    size_t *tmp_loops = malloc(m->loop_count * sizeof(size_t));
    size_t *vf_off = NULL;
    hal_u32 *vf = NULL;
    if (!island || !queue || !tmp || !tmp_loops)
        goto fail;
    for (size_t f = 0; f < m->face_count; f++)
        island[f] = -1;

    /* Vert -> faces adjacency (rebuilt per mesh size). */
    vf_off = calloc(m->vert_count + 1, sizeof(size_t));
    vf = malloc(m->loop_count * sizeof(hal_u32));
    if (!vf_off || !vf)
        goto fail;
    for (size_t l = 0; l < m->loop_count; l++)
        vf_off[m->loops[l] + 1]++;
    for (size_t v = 0; v < m->vert_count; v++)
        vf_off[v + 1] += vf_off[v];
    {
        size_t *fill = calloc(m->vert_count, sizeof(size_t));
        if (!fill)
            goto fail;
        for (size_t f = 0; f < m->face_count; f++)
            for (size_t l = m->face_off[f]; l < m->face_off[f + 1]; l++) {
                hal_u32 v = m->loops[l];
                vf[vf_off[v] + fill[v]++] = (hal_u32)f;
            }
        free(fill);
    }

    int island_count = 0;
    for (size_t seed = 0; seed < m->face_count; seed++) {
        if (island[seed] >= 0)
            continue;
        /* BFS. */
        int qh = 0, qt = 0;
        queue[qt++] = (int)seed;
        island[seed] = island_count;
        while (qh < qt) {
            int f = queue[qh++];
            for (size_t l = m->face_off[f]; l < m->face_off[f + 1]; l++) {
                hal_u32 v = m->loops[l];
                for (size_t k = vf_off[v]; k < vf_off[v + 1]; k++) {
                    int nf = (int)vf[k];
                    if (island[nf] >= 0)
                        continue;
                    if (hal_vec3f_dot(fn[f], fn[nf]) < 0.9f)
                        continue;
                    island[nf] = island_count;
                    if (qt < (int)m->face_count)
                        queue[qt++] = nf;
                }
            }
        }
        island_count++;
    }

    /* Per island: average normal -> dominant axis; project; pack
     * greedily into rows of height 1/rows_total. */
    {
        int rows = island_count < 1 ? 1 : island_count;
        hal_f32 row_h = 1.0f / (hal_f32)rows;
        for (int isl = 0; isl < island_count; isl++) {
            hal_vec3f avg = hal_vec3f_zero();
            size_t n = 0;
            for (size_t f = 0; f < m->face_count; f++) {
                if (island[f] != isl)
                    continue;
                avg = hal_vec3f_add(avg, fn[f]);
                for (size_t l = m->face_off[f]; l < m->face_off[f + 1];
                     l++) {
                    tmp[n] = project2(m->verts[m->loops[l]], 0);
                    tmp_loops[n] = l;
                    n++;
                }
            }
            if (n == 0)
                continue;
            hal_f32 sign;
            int axis = dominant_axis(hal_vec3f_normalize(avg), &sign);
            for (size_t i = 0; i < n; i++)
                tmp[i] = project2(m->verts[m->loops[tmp_loops[i]]], axis);

            hal_f32 cw = 1.0f / (hal_f32)(island_count);
            hal_f32 cx = (hal_f32)isl * cw;
            hal_f32 cy = (hal_f32)isl * row_h;
            hal_vec2f *dst = malloc(n * sizeof(*dst));
            if (!dst)
                continue;
            place_uv(dst, tmp, n, cx + 0.02f * cw, cy + 0.02f * row_h,
                     cw * 0.96f, row_h * 0.96f);
            for (size_t i = 0; i < n; i++)
                m->uvs[tmp_loops[i]] = dst[i];
            free(dst);
        }
    }

    free(island);
    free(queue);
    free(tmp);
    free(tmp_loops);
    free(vf_off);
    free(vf);
    m->uvs_valid = true;
    return true;

fail:
    free(island);
    free(queue);
    free(tmp);
    free(tmp_loops);
    free(vf_off);
    free(vf);
    return false;
}

bool hal_mesh_unwrap_angle(hal_mesh *m, hal_f32 threshold_deg) {
    if (!m || m->face_count == 0 || !m->uvs)
        return false;
    const hal_vec3f *fn = hal_mesh_face_normals(m);
    if (!fn)
        return false;

    /* Edge list with the (up to 2) using faces. */
    typedef struct {
        hal_u32 a, b;
        int f0, f1;
    } aedge;
    aedge *edges = NULL;
    size_t edge_count = 0, edge_cap = 0;

    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = 0;
        for (size_t i = 0; m->face_off && i < m->face_off[f + 1] - m->face_off[f]; i++) {
            hal_u32 a = m->loops[m->face_off[f] + i];
            hal_u32 b =
                m->loops[m->face_off[f] + (i + 1) %
                                            (m->face_off[f + 1] -
                                             m->face_off[f])];
            bool found = false;
            for (size_t e = 0; e < edge_count; e++)
                if ((edges[e].a == a && edges[e].b == b) ||
                    (edges[e].a == b && edges[e].b == a)) {
                    if (edges[e].f1 < 0)
                        edges[e].f1 = (int)f;
                    found = true;
                    break;
                }
            if (!found) {
                if (edge_count == edge_cap) {
                    edge_cap = edge_cap ? edge_cap * 2 : 64;
                    aedge *grown =
                        realloc(edges, edge_cap * sizeof(*grown));
                    if (!grown) {
                        free(edges);
                        return false;
                    }
                    edges = grown;
                }
                edges[edge_count].a = a;
                edges[edge_count].b = b;
                edges[edge_count].f0 = (int)f;
                edges[edge_count].f1 = -1;
                edge_count++;
            }
        }
        (void)n;
    }

    /* Face adjacency across NON-seam edges. */
    hal_f32 cos_threshold = cosf(threshold_deg * 3.14159265f / 180.0f);
    int *chart = malloc(m->face_count * sizeof(int));
    int *queue = malloc(m->face_count * sizeof(int));
    if (!chart || !queue) {
        free(edges);
        free(chart);
        free(queue);
        return false;
    }
    for (size_t f = 0; f < m->face_count; f++)
        chart[f] = -1;

    int chart_count = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        if (chart[f] >= 0)
            continue;
        chart[f] = chart_count;
        size_t qn = 0;
        queue[qn++] = (int)f;
        while (qn > 0) {
            int cf = queue[--qn];
            for (size_t e = 0; e < edge_count; e++) {
                if (edges[e].f0 != cf && edges[e].f1 != cf)
                    continue;
                if (edges[e].f1 < 0)
                    continue; /* mesh boundary: no crossing */
                int other = edges[e].f0 == cf ? edges[e].f1
                                              : edges[e].f0;
                if (chart[other] >= 0)
                    continue;
                /* Seam test: dihedral angle between the faces. */
                hal_f32 d =
                    hal_vec3f_dot(fn[edges[e].f0], fn[edges[e].f1]);
                if (d < cos_threshold)
                    continue; /* seam */
                chart[other] = chart_count;
                queue[qn++] = other;
            }
        }
        chart_count++;
    }

    /* Per chart: project onto the dominant axis of its area-weighted
     * average normal; normalize the chart to [0,1]; shelf-pack the
     * charts into the UV square. */
    hal_vec2f *tmp = malloc(m->loop_count * sizeof(*tmp));
    if (!tmp) {
        free(edges);
        free(chart);
        free(queue);
        return false;
    }

    typedef struct {
        hal_f32 w, h; /* chart size in UV units */
    } cbox;
    cbox *boxes = calloc((size_t)chart_count, sizeof(*boxes));
    hal_f32 shelf_y = 0.0f, shelf_h = 0.0f, cursor_x = 0.0f;
    const hal_f32 pad = 0.008f;

    for (int ci = 0; ci < chart_count; ci++) {
        hal_vec3f avg = hal_vec3f_zero();
        hal_f32 area = 0.0f;
        for (size_t f = 0; f < m->face_count; f++) {
            if (chart[f] != ci)
                continue;
            avg = hal_vec3f_add(avg, fn[f]);
            area += 1.0f;
        }
        (void)area;
        hal_f32 ax = fabsf(avg.x), ay = fabsf(avg.y),
                az = fabsf(avg.z);
        int drop = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);

        hal_vec2f bmin = { 1e30f, 1e30f };
        hal_vec2f bmax = { -1e30f, -1e30f };
        for (size_t f = 0; f < m->face_count; f++) {
            if (chart[f] != ci)
                continue;
            for (size_t i = 0;
                 i < m->face_off[f + 1] - m->face_off[f]; i++) {
                hal_vec3f p =
                    m->verts[m->loops[m->face_off[f] + i]];
                hal_vec2f uv;
                if (drop == 0)
                    uv = (hal_vec2f){ p.y, p.z };
                else if (drop == 1)
                    uv = (hal_vec2f){ p.x, p.z };
                else
                    uv = (hal_vec2f){ p.x, p.y };
                tmp[m->face_off[f] + i] = uv;
                bmin.x = fminf(bmin.x, uv.x);
                bmin.y = fminf(bmin.y, uv.y);
                bmax.x = fmaxf(bmax.x, uv.x);
                bmax.y = fmaxf(bmax.y, uv.y);
            }
        }
        hal_f32 sx = bmax.x - bmin.x, sy = bmax.y - bmin.y;
        if (sx < 1e-6f)
            sx = 1e-6f;
        if (sy < 1e-6f)
            sy = 1e-6f;
        boxes[ci].w = sx / (sx > sy ? sx : sy);
        boxes[ci].h = sy / (sx > sy ? sx : sy);
        /* Scale charts to a sane share of the square (charts are
         * normalized to max dimension 0.45 then shelf-packed). */
        /* Charts fit a ceil(sqrt(n)) grid so the shelf pack stays
         * inside the UV square (six cube charts -> 3x2 cells). */
        size_t grid = 1;
        while (grid * grid < (size_t)chart_count)
            grid++;
        hal_f32 scale =
            fminf(0.45f, 1.0f / (hal_f32)grid - 2.0f * pad);
        boxes[ci].w *= scale;
        boxes[ci].h *= scale;

        /* Shelf packing: new row when the cursor overflows. */
        if (cursor_x + boxes[ci].w + pad > 1.0f) {
            shelf_y += shelf_h + pad;
            shelf_h = 0.0f;
            cursor_x = 0.0f;
        }
        hal_f32 ox = cursor_x, oy = shelf_y;
        cursor_x += boxes[ci].w + pad;
        if (boxes[ci].h > shelf_h)
            shelf_h = boxes[ci].h;

        for (size_t f = 0; f < m->face_count; f++) {
            if (chart[f] != ci)
                continue;
            for (size_t i = 0;
                 i < m->face_off[f + 1] - m->face_off[f]; i++) {
                hal_vec2f uv = tmp[m->face_off[f] + i];
                m->uvs[m->face_off[f] + i] = (hal_vec2f){
                    ox + (uv.x - bmin.x) / sx * boxes[ci].w,
                    oy + (uv.y - bmin.y) / sy * boxes[ci].h
                };
            }
        }
    }
    m->uvs_valid = true;

    free(tmp);
    free(boxes);
    free(edges);
    free(chart);
    free(queue);
    return true;
}
