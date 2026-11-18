/*
 * mesh_ops.c — mesh selection and the modeling operators
 *
 * Conventions: faces are loop runs (mesh.h); edge maps key undirected
 * (min,max). Operators mutate in place; the editor snapshots for undo
 * (hal_mesh_clone before, pointer-swap on undo/redo).
 */

#include "mesh_ops.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Selection ---- */

void hal_mesh_select_all(hal_mesh *m, bool on) {
    if (!m)
        return;
    hal_u8 v = on ? 1 : 0;
    if (m->vert_sel)
        memset(m->vert_sel, v, m->vert_count);
    if (m->edge_sel)
        memset(m->edge_sel, v, m->edge_count);
    if (m->face_sel)
        memset(m->face_sel, v, m->face_count);
}

size_t hal_mesh_count_selected_verts(const hal_mesh *m) {
    size_t n = 0;
    if (m && m->vert_sel)
        for (size_t i = 0; i < m->vert_count; i++)
            n += m->vert_sel[i] ? 1 : 0;
    return n;
}

size_t hal_mesh_count_selected_edges(const hal_mesh *m) {
    size_t n = 0;
    if (m && m->edge_sel)
        for (size_t i = 0; i < m->edge_count; i++)
            n += m->edge_sel[i] ? 1 : 0;
    return n;
}

size_t hal_mesh_count_selected_faces(const hal_mesh *m) {
    size_t n = 0;
    if (m && m->face_sel)
        for (size_t i = 0; i < m->face_count; i++)
            n += m->face_sel[i] ? 1 : 0;
    return n;
}

/* ---- Clone ---- */

hal_mesh *hal_mesh_clone(const hal_mesh *src) {
    if (!src)
        return NULL;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;

#define CLONE_ARRAY(dst, cap_field, count, src_ptr, type)                     \
    do {                                                                      \
        if ((count) != 0) {                                                   \
            (dst) = malloc((count) * sizeof(type));                           \
            if (!(dst))                                                       \
                goto fail;                                                    \
            memcpy((dst), (src_ptr), (count) * sizeof(type));                 \
            m->cap_field = (count);                                           \
        }                                                                     \
    } while (0)

    CLONE_ARRAY(m->verts, vert_cap, src->vert_count, src->verts, hal_vec3f);
    m->vert_count = src->vert_count;
    CLONE_ARRAY(m->edges, edge_cap, src->edge_count * 2, src->edges, hal_u32);
    m->edge_count = src->edge_count;
    CLONE_ARRAY(m->loops, loop_cap, src->loop_count, src->loops, hal_u32);
    m->loop_count = src->loop_count;
    CLONE_ARRAY(m->face_off, face_cap, src->face_count + 1, src->face_off,
                size_t);
    m->face_count = src->face_count;
#undef CLONE_ARRAY

    if (src->vert_count) {
        m->vert_sel = calloc(src->vert_count, 1);
        if (!m->vert_sel)
            goto fail;
        if (src->vert_sel)
            memcpy(m->vert_sel, src->vert_sel, src->vert_count);
        m->vert_sel_cap = src->vert_count;
    }
    if (src->edge_count) {
        m->edge_sel = calloc(src->edge_count, 1);
        if (!m->edge_sel)
            goto fail;
        if (src->edge_sel)
            memcpy(m->edge_sel, src->edge_sel, src->edge_count);
        m->edge_sel_cap = src->edge_count;
    }
    if (src->face_count) {
        m->face_sel = calloc(src->face_count, 1);
        if (!m->face_sel)
            goto fail;
        if (src->face_sel)
            memcpy(m->face_sel, src->face_sel, src->face_count);
        m->face_sel_cap = src->face_count;
    }
    if (src->uvs && src->loop_count) {
        m->uvs = malloc(src->loop_count * sizeof(hal_vec2f));
        if (!m->uvs)
            goto fail;
        memcpy(m->uvs, src->uvs, src->loop_count * sizeof(hal_vec2f));
        m->uv_cap = src->loop_count;
        m->uvs_valid = src->uvs_valid;
    }
    return m;

fail:
    hal_mesh_destroy(m);
    return NULL;
}

/* ---- Shared helpers ---- */

static size_t face_len(const hal_mesh *m, size_t f) {
    return m->face_off[f + 1] - m->face_off[f];
}

static hal_u32 face_vert(const hal_mesh *m, size_t f, size_t i) {
    return m->loops[m->face_off[f] + i];
}

typedef struct edge_key {
    hal_u32 a, b; /* a < b */
} edge_key;

static edge_key edge_key_make(hal_u32 a, hal_u32 b) {
    return a < b ? (edge_key){ a, b } : (edge_key){ b, a };
}

typedef struct edge_map {
    struct {
        edge_key key;
        hal_u32 value;
    } *pairs;
    size_t count, cap;
} edge_map;

static bool edge_map_put(edge_map *map, hal_u32 a, hal_u32 b, hal_u32 value) {
    if (map->count == map->cap) {
        size_t cap = map->cap ? map->cap * 2 : 32;
        void *grown = realloc(map->pairs, cap * sizeof(*map->pairs));
        if (!grown)
            return false;
        map->pairs = grown;
        map->cap = cap;
    }
    map->pairs[map->count].key = edge_key_make(a, b);
    map->pairs[map->count].value = value;
    map->count++;
    return true;
}

static bool edge_map_get(const edge_map *map, hal_u32 a, hal_u32 b,
                         hal_u32 *out) {
    edge_key k = edge_key_make(a, b);
    for (size_t i = 0; i < map->count; i++)
        if (map->pairs[i].key.a == k.a && map->pairs[i].key.b == k.b) {
            *out = map->pairs[i].value;
            return true;
        }
    return false;
}

/* Vert -> adjacent faces. */
typedef struct vert_faces {
    struct {
        hal_u32 *faces;
        size_t count, cap;
    } *per_vert;
    size_t vert_count;
} vert_faces;

static bool vert_faces_build(const hal_mesh *m, vert_faces *vf) {
    vf->vert_count = m->vert_count;
    vf->per_vert = calloc(m->vert_count ? m->vert_count : 1,
                          sizeof(*vf->per_vert));
    if (!vf->per_vert)
        return false;
    for (size_t f = 0; f < m->face_count; f++)
        for (size_t i = 0; i < face_len(m, f); i++) {
            hal_u32 v = face_vert(m, f, i);
            if (v >= m->vert_count)
                continue;
            if (vf->per_vert[v].count == vf->per_vert[v].cap) {
                size_t cap = vf->per_vert[v].cap ? vf->per_vert[v].cap * 2
                                                 : 4;
                hal_u32 *grown =
                    realloc(vf->per_vert[v].faces, cap * sizeof(hal_u32));
                if (!grown)
                    return false;
                vf->per_vert[v].faces = grown;
                vf->per_vert[v].cap = cap;
            }
            vf->per_vert[v].faces[vf->per_vert[v].count++] = (hal_u32)f;
        }
    return true;
}

static void vert_faces_free(vert_faces *vf) {
    if (!vf->per_vert)
        return;
    for (size_t v = 0; v < vf->vert_count; v++)
        free(vf->per_vert[v].faces);
    free(vf->per_vert);
    vf->per_vert = NULL;
}

/* Keeps faces whose keep[f]; rebuilds loops/face_off/face_sel. */
static bool compact_faces(hal_mesh *m, const bool *keep) {
    size_t kept = 0;
    for (size_t f = 0; f < m->face_count; f++)
        kept += keep[f] ? 1 : 0;
    if (kept == m->face_count)
        return true;

    hal_u32 *new_loops = malloc((m->loop_count ? m->loop_count : 1) *
                                sizeof(hal_u32));
    size_t *new_off = malloc((kept + 1) * sizeof(size_t));
    hal_u8 *new_sel = calloc(kept ? kept : 1, 1);
    /* UVs are loop-indexed: they must follow the kept loops or they
     * would silently desync after merge/delete/subdivide. */
    hal_vec2f *new_uvs =
        m->uvs ? malloc((m->loop_count ? m->loop_count : 1) *
                        sizeof(hal_vec2f))
               : NULL;
    if (!new_loops || !new_off || !new_sel || (m->uvs && !new_uvs)) {
        free(new_loops);
        free(new_off);
        free(new_sel);
        free(new_uvs);
        return false;
    }

    size_t nl = 0, nf = 0;
    new_off[0] = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        if (!keep[f])
            continue;
        for (size_t i = 0; i < face_len(m, f); i++) {
            new_loops[nl] = face_vert(m, f, i);
            if (new_uvs)
                new_uvs[nl] = m->uvs[m->face_off[f] + i];
            nl++;
        }
        new_sel[nf] = m->face_sel ? m->face_sel[f] : 0;
        nf++;
        new_off[nf] = nl;
    }

    free(m->loops);
    free(m->face_off);
    free(m->face_sel);
    m->loops = new_loops;
    m->loop_count = m->loop_cap = nl;
    m->face_off = new_off;
    m->face_count = m->face_cap = nf;
    m->face_sel = new_sel;
    m->face_sel_cap = kept;
    if (m->uvs) {
        free(m->uvs);
        m->uvs = new_uvs;
        m->uv_cap = m->loop_count;
    }
    hal_mesh_invalidate_derived(m);
    return true;
}

/* Drops verts where keep_vert is false; loops must ALREADY be
 * remapped to surviving ids (remap_survivors[v] for a dropped vert is
 * never read). Faces referencing dropped ids must already be gone. */
static bool compact_verts(hal_mesh *m, const bool *keep_vert,
                          const hal_u32 *new_id) {
    size_t kept = 0;
    for (size_t v = 0; v < m->vert_count; v++)
        kept += keep_vert[v] ? 1 : 0;

    hal_vec3f *new_verts = malloc((kept ? kept : 1) * sizeof(hal_vec3f));
    hal_u8 *new_sel = calloc(kept ? kept : 1, 1);
    if (!new_verts || !new_sel) {
        free(new_verts);
        free(new_sel);
        return false;
    }
    size_t nv = 0;
    for (size_t v = 0; v < m->vert_count; v++) {
        if (!keep_vert[v])
            continue;
        new_verts[nv] = m->verts[v];
        new_sel[nv] = m->vert_sel ? m->vert_sel[v] : 0;
        nv++;
    }
    (void)new_id;
    free(m->verts);
    free(m->vert_sel);
    m->verts = new_verts;
    m->vert_count = m->vert_cap = nv;
    m->vert_sel = new_sel;
    m->vert_sel_cap = nv;
    hal_mesh_invalidate_derived(m);
    return true;
}

/* ---- Extrude ---- */

bool hal_mesh_extrude_faces(hal_mesh *m, hal_f32 offset) {
    if (!m || m->face_count == 0)
        return false;

    const hal_vec3f *fn = hal_mesh_face_normals(m);
    if (!fn)
        return false;

    /* Capture the selection first (the op consumes it). */
    hal_u8 *was = malloc(m->face_count);
    if (!was)
        return false;
    for (size_t f = 0; f < m->face_count; f++)
        was[f] = m->face_sel ? m->face_sel[f] : 0;

    bool changed = false;
    size_t original_faces = m->face_count;
    size_t verts_before = m->vert_count;

    for (size_t f = original_faces; f-- > 0;) {
        if (!was[f])
            continue;
        size_t n = face_len(m, f);
        if (n < 3 || n > 64)
            continue;

        hal_vec3f normal = fn[f];
        hal_u32 old_c[64], new_c[64];
        for (size_t i = 0; i < n; i++) {
            old_c[i] = face_vert(m, f, i);
            int nv = hal_mesh_vert_add(
                m, hal_vec3f_add(m->verts[old_c[i]],
                                 hal_vec3f_mul(normal, offset)));
            if (nv < 0)
                goto done;
            new_c[i] = (hal_u32)nv;
        }

        /* The face becomes its extruded copy; sides connect. */
        for (size_t i = 0; i < n; i++)
            m->loops[m->face_off[f] + i] = new_c[i];
        for (size_t i = 0; i < n; i++) {
            /* (old_i, old_{i+1}, new_{i+1}, new_i): outward sides
             * (re-derived on the corrected cube's top face). */
            hal_u32 quad[4] = { old_c[i], old_c[(i + 1) % n],
                                new_c[(i + 1) % n], new_c[i] };
            hal_mesh_face_add(m, quad, 4);
        }
        changed = true;
    }

done:
    free(was);
    hal_mesh_select_all(m, false);
    /* Select the new verts so a following move grabs them. */
    for (size_t v = verts_before; v < m->vert_count; v++)
        m->vert_sel[v] = 1;
    hal_mesh_rebuild_edges(m);
    return changed;
}

/* ---- Inset ---- */

bool hal_mesh_inset_faces(hal_mesh *m, hal_f32 t) {
    if (!m || t <= 0.0f || t >= 1.0f || m->face_count == 0)
        return false;

    hal_u8 *was = malloc(m->face_count);
    if (!was)
        return false;
    for (size_t f = 0; f < m->face_count; f++)
        was[f] = m->face_sel ? m->face_sel[f] : 0;

    bool changed = false;
    size_t original_faces = m->face_count;

    for (size_t f = original_faces; f-- > 0;) {
        if (!was[f])
            continue;
        size_t n = face_len(m, f);
        if (n < 3 || n > 64)
            continue;

        hal_u32 old_c[64], new_c[64];
        hal_vec3f centroid = hal_vec3f_zero();
        for (size_t i = 0; i < n; i++) {
            old_c[i] = face_vert(m, f, i);
            centroid = hal_vec3f_add(centroid, m->verts[old_c[i]]);
        }
        centroid = hal_vec3f_mul(centroid, 1.0f / (hal_f32)n);

        for (size_t i = 0; i < n; i++) {
            int nv = hal_mesh_vert_add(
                m, hal_vec3f_lerp(m->verts[old_c[i]], centroid, t));
            if (nv < 0)
                goto done;
            new_c[i] = (hal_u32)nv;
        }

        for (size_t i = 0; i < n; i++)
            m->loops[m->face_off[f] + i] = new_c[i];
        for (size_t i = 0; i < n; i++) {
            hal_u32 quad[4] = { old_c[i], old_c[(i + 1) % n],
                                new_c[(i + 1) % n], new_c[i] };
            hal_mesh_face_add(m, quad, 4);
        }
        changed = true;
    }

done:
    free(was);
    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return changed;
}

/* ---- Bevel ---- */

bool hal_mesh_bevel_edges(hal_mesh *m, hal_f32 amount) {
    if (!m || amount <= 0.0f || m->edge_count == 0)
        return false;

    vert_faces vf = { 0 };
    if (!vert_faces_build(m, &vf))
        return false;

    bool *sel_edge = malloc(m->edge_count);
    int *endpoint_use = calloc(m->vert_count ? m->vert_count : 1,
                               sizeof(int));
    if (!sel_edge || !endpoint_use) {
        free(sel_edge);
        free(endpoint_use);
        vert_faces_free(&vf);
        return false;
    }
    for (size_t e = 0; e < m->edge_count; e++) {
        sel_edge[e] = m->edge_sel && m->edge_sel[e];
        if (sel_edge[e]) {
            endpoint_use[m->edges[e * 2]]++;
            endpoint_use[m->edges[e * 2 + 1]]++;
        }
    }

    bool changed = false;
    for (size_t e = m->edge_count; e-- > 0;) {
        if (!sel_edge[e])
            continue;
        hal_u32 a = m->edges[e * 2];
        hal_u32 b = m->edges[e * 2 + 1];
        if (endpoint_use[a] > 1 || endpoint_use[b] > 1)
            continue; /* vertex bevels: out of scope (documented) */

        /* The two faces sharing (a, b). */
        size_t f2[2] = { (size_t)-1, (size_t)-1 };
        for (size_t i = 0; i < vf.per_vert[a].count; i++) {
            size_t f = vf.per_vert[a].faces[i];
            for (size_t j = 0; j < face_len(m, f); j++)
                if (face_vert(m, f, j) == b) {
                    if (f2[0] == (size_t)-1)
                        f2[0] = f;
                    else if (f2[1] == (size_t)-1 && f != f2[0])
                        f2[1] = f;
                }
        }
        if (f2[0] == (size_t)-1 || f2[1] == (size_t)-1)
            continue; /* not manifold */

        /* Slide each endpoint toward the average of its edge and
         * face-neighbor directions. */
        hal_vec3f dir_a = hal_vec3f_sub(m->verts[b], m->verts[a]);
        hal_vec3f dir_b = hal_vec3f_neg(dir_a);
        for (int fi = 0; fi < 2; fi++) {
            size_t n = face_len(m, f2[fi]);
            for (size_t i = 0; i < n; i++) {
                hal_u32 v = face_vert(m, f2[fi], i);
                if (v == a || v == b)
                    continue;
                hal_u32 prev = face_vert(m, f2[fi], (i + n - 1) % n);
                hal_u32 next = face_vert(m, f2[fi], (i + 1) % n);
                if (prev == a || next == a)
                    dir_a = hal_vec3f_add(
                        dir_a, hal_vec3f_sub(m->verts[v], m->verts[a]));
                if (prev == b || next == b)
                    dir_b = hal_vec3f_add(
                        dir_b, hal_vec3f_sub(m->verts[v], m->verts[b]));
            }
        }

        int na = hal_mesh_vert_add(
            m, hal_vec3f_add(m->verts[a],
                             hal_vec3f_mul(hal_vec3f_normalize(dir_a),
                                           amount)));
        int nb = hal_mesh_vert_add(
            m, hal_vec3f_add(m->verts[b],
                             hal_vec3f_mul(hal_vec3f_normalize(dir_b),
                                           amount)));
        if (na < 0 || nb < 0)
            break;
        hal_u32 a2 = (hal_u32)na, b2 = (hal_u32)nb;

        for (int fi = 0; fi < 2; fi++)
            for (size_t i = 0; i < face_len(m, f2[fi]); i++) {
                hal_u32 v = face_vert(m, f2[fi], i);
                if (v == a)
                    m->loops[m->face_off[f2[fi]] + i] = a2;
                else if (v == b)
                    m->loops[m->face_off[f2[fi]] + i] = b2;
            }

        hal_u32 quad[4] = { a, a2, b2, b };
        hal_mesh_face_add(m, quad, 4);
        changed = true;
    }

    free(sel_edge);
    free(endpoint_use);
    vert_faces_free(&vf);
    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return changed;
}

/* ---- Loop cut ---- */

bool hal_mesh_loop_cut(hal_mesh *m, hal_u32 edge, hal_f32 t) {
    if (!m || edge >= (hal_u32)m->edge_count || t <= 0.0f || t >= 1.0f)
        return false;

    hal_u32 a = m->edges[edge * 2];
    hal_u32 b = m->edges[edge * 2 + 1];

    vert_faces vf = { 0 };
    if (!vert_faces_build(m, &vf))
        return false;

    /* Walk the quad ring from edge (a,b), remembering each face's
     * ENTRY edge so the split can orient consistently. */
    size_t ring_f[512];
    hal_u32 ring_in_a[512], ring_in_b[512];
    size_t ring_count = 0;
    hal_u32 cur_a = a, cur_b = b;
    bool closed = false;

    while (ring_count < 512) {
        size_t found = (size_t)-1;
        for (size_t i = 0; i < vf.per_vert[cur_a].count; i++) {
            size_t f = vf.per_vert[cur_a].faces[i];
            if (face_len(m, f) != 4)
                continue;
            bool visited = false;
            for (size_t r = 0; r < ring_count; r++)
                if (ring_f[r] == f)
                    visited = true;
            if (visited)
                continue;
            for (size_t j = 0; j < 4; j++)
                if (face_vert(m, f, j) == cur_b) {
                    found = f;
                    break;
                }
            if (found != (size_t)-1)
                break;
        }
        if (found == (size_t)-1)
            break;

        /* Orient the quad: v0,v1 is the entry edge (either order). */
        size_t rot = 0;
        bool found_rot = false;
        for (size_t j = 0; j < 4; j++) {
            hal_u32 v0 = face_vert(m, found, j);
            hal_u32 v1 = face_vert(m, found, (j + 1) % 4);
            if ((v0 == cur_a && v1 == cur_b) || (v0 == cur_b && v1 == cur_a)) {
                rot = j;
                found_rot = true;
                break;
            }
        }
        if (!found_rot)
            break;

        hal_u32 q[4];
        for (size_t j = 0; j < 4; j++)
            q[j] = face_vert(m, found, (rot + j) % 4);

        ring_f[ring_count] = found;
        ring_in_a[ring_count] = q[0];
        ring_in_b[ring_count] = q[1];
        ring_count++;

        /* Exit edge = the opposite one; the walk continues through it. */
        hal_u32 na = q[2], nb = q[3];
        if ((na == a && nb == b) || (na == b && nb == a)) {
            closed = true;
            break;
        }
        cur_a = na;
        cur_b = nb;
    }

    vert_faces_free(&vf);
    if (ring_count == 0 || !closed)
        return ring_count > 0; /* an open strip still cuts what it found */

    /* Split each ring quad across SHARED side-edge midpoints. */
    edge_map mids = { 0 };
    for (size_t r = 0; r < ring_count; r++) {
        size_t f = ring_f[r];
        hal_u32 q[4];
        for (size_t j = 0; j < 4; j++)
            q[j] = face_vert(m, f, j);
        /* Rotate so q[0],q[1] is the entry edge again (the face may
         * have been touched only by us — the walk stored it rotated,
         * but a face can appear once in a simple ring, so re-find). */
        size_t rot = 0;
        for (size_t j = 0; j < 4; j++) {
            hal_u32 v0 = face_vert(m, f, j);
            hal_u32 v1 = face_vert(m, f, (j + 1) % 4);
            if ((v0 == ring_in_a[r] && v1 == ring_in_b[r]) ||
                (v0 == ring_in_b[r] && v1 == ring_in_a[r])) {
                rot = j;
                break;
            }
        }
        for (size_t j = 0; j < 4; j++)
            q[j] = face_vert(m, f, (rot + j) % 4);

        /* The cut runs ACROSS each ring quad: midpoints on the entry
         * edge (q0,q1) and the exit edge (q2,q3), shared through the
         * map so the loop stays connected. */
        hal_u32 m01;
        if (!edge_map_get(&mids, q[0], q[1], &m01)) {
            int nv = hal_mesh_vert_add(
                m, hal_vec3f_lerp(m->verts[q[0]], m->verts[q[1]], t));
            if (nv < 0)
                break;
            m01 = (hal_u32)nv;
            edge_map_put(&mids, q[0], q[1], m01);
        }
        hal_u32 m23;
        if (!edge_map_get(&mids, q[2], q[3], &m23)) {
            int nv = hal_mesh_vert_add(
                m, hal_vec3f_lerp(m->verts[q[2]], m->verts[q[3]], t));
            if (nv < 0)
                break;
            m23 = (hal_u32)nv;
            edge_map_put(&mids, q[2], q[3], m23);
        }

        hal_u32 half1[4] = { q[0], m01, m23, q[3] };
        hal_u32 half2[4] = { m01, q[1], q[2], m23 };
        for (size_t j = 0; j < 4; j++)
            m->loops[m->face_off[f] + j] = half1[j];
        hal_mesh_face_add(m, half2, 4);
    }

    free(mids.pairs);
    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return true;
}

/* ---- Subdivide ---- */

bool hal_mesh_subdivide(hal_mesh *m) {
    if (!m || m->face_count == 0)
        return false;

    bool all = hal_mesh_count_selected_faces(m) == 0;
    size_t original_faces = m->face_count;

    bool *keep = malloc(original_faces);
    if (!keep)
        return false;
    for (size_t f = 0; f < original_faces; f++)
        keep[f] = !(all || (m->face_sel && m->face_sel[f]));

    edge_map mids = { 0 };
    bool changed = false;

    /* Only the ORIGINAL faces are visited (new ones append). */
    for (size_t f = 0; f < original_faces; f++) {
        if (keep[f])
            continue;
        size_t n = face_len(m, f);
        if (n < 3 || n > 64)
            continue;

        hal_u32 corners[64], edge_mids[64];
        hal_vec3f center = hal_vec3f_zero();
        for (size_t i = 0; i < n; i++) {
            corners[i] = face_vert(m, f, i);
            center = hal_vec3f_add(center, m->verts[corners[i]]);
        }
        center = hal_vec3f_mul(center, 1.0f / (hal_f32)n);
        int cv = hal_mesh_vert_add(m, center);
        if (cv < 0)
            break;

        for (size_t i = 0; i < n; i++) {
            hal_u32 v0 = corners[i];
            hal_u32 v1 = corners[(i + 1) % n];
            if (!edge_map_get(&mids, v0, v1, &edge_mids[i])) {
                int nv = hal_mesh_vert_add(
                    m, hal_vec3f_mul(hal_vec3f_add(m->verts[v0],
                                                    m->verts[v1]),
                                     0.5f));
                if (nv < 0)
                    goto done;
                edge_mids[i] = (hal_u32)nv;
                edge_map_put(&mids, v0, v1, edge_mids[i]);
            }
        }
        for (size_t i = 0; i < n; i++) {
            hal_u32 quad[4] = { corners[i], edge_mids[i], (hal_u32)cv,
                                edge_mids[(i + n - 1) % n] };
            hal_mesh_face_add(m, quad, 4);
        }
        changed = true;
    }

done:
    if (changed) {
        /* New faces appended past original_faces must be kept: grow
         * the flag array to the current count (compact_faces walks
         * the whole array). */
        bool *grown = realloc(keep, m->face_count ? m->face_count : 1);
        if (grown) {
            for (size_t f = original_faces; f < m->face_count; f++)
                grown[f] = true;
            keep = grown;
            compact_faces(m, keep);
        }
    }
    free(keep);
    free(mids.pairs);
    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return changed;
}

/* ---- Merge ---- */

bool hal_mesh_merge_selected_verts(hal_mesh *m) {
    if (!m)
        return false;
    size_t count = hal_mesh_count_selected_verts(m);
    if (count < 2)
        return false;

    hal_vec3f centroid = hal_vec3f_zero();
    for (size_t v = 0; v < m->vert_count; v++)
        if (m->vert_sel[v])
            centroid = hal_vec3f_add(centroid, m->verts[v]);
    centroid = hal_vec3f_mul(centroid, 1.0f / (hal_f32)count);

    /* Target: the first selected vert moves to the centroid; every
     * other selected vert remaps onto it. */
    hal_u32 target = 0;
    for (size_t v = 0; v < m->vert_count; v++)
        if (m->vert_sel[v]) {
            target = (hal_u32)v;
            break;
        }
    m->verts[target] = centroid;

    hal_u32 *remap = malloc(m->vert_count * sizeof(hal_u32));
    bool *keep_vert = malloc(m->vert_count);
    if (!remap || !keep_vert) {
        free(remap);
        free(keep_vert);
        return false;
    }
    for (size_t v = 0; v < m->vert_count; v++) {
        bool sel = m->vert_sel[v];
        remap[v] = sel ? target : (hal_u32)v;
        keep_vert[v] = !sel || (hal_u32)v == target;
    }

    /* Rebuild the faces: remap corners, drop consecutive duplicates,
     * drop faces that fall below 3 corners. */
    size_t total_loops = 0;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = face_len(m, f);
        hal_u32 dedup[64];
        size_t dn = 0;
        for (size_t i = 0; i < n && i < 64; i++) {
            hal_u32 v = remap[face_vert(m, f, i)];
            if (dn == 0 || dedup[dn - 1] != v)
                dedup[dn++] = v;
        }
        if (dn >= 3 && dedup[0] == dedup[dn - 1])
            dn--;
        if (dn < 3)
            continue; /* face dies */
        for (size_t i = 0; i < dn; i++)
            m->loops[m->face_off[f] + i] = dedup[i];
        m->face_off[f + 1] = m->face_off[f] + dn;
        total_loops += dn;
        (void)total_loops;
    }

    /* Compact faces: a face dies when any corner remapped to a vert
     * that is being dropped (i.e. a corner is selected-but-not-target
     * — those corners became `target`, which survives) or when its
     * run shrank to zero above. Simplest death test: recompute its
     * run length and check every corner survives. */
    bool *keep_face = malloc(m->face_count ? m->face_count : 1);
    if (!keep_face) {
        free(keep_face);
        free(remap);
        free(keep_vert);
        return false;
    }
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = m->face_off[f + 1] - m->face_off[f];
        keep_face[f] = n >= 3;
        for (size_t i = 0; i < n && keep_face[f]; i++)
            keep_face[f] = keep_vert[m->loops[m->face_off[f] + i]];
    }
    compact_faces(m, keep_face);
    free(keep_face);

    /* Compact verts: loops already point at OLD ids (survivors). */
    compact_verts(m, keep_vert, remap);
    free(remap);
    free(keep_vert);

    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return true;
}

/* ---- Delete ---- */

bool hal_mesh_delete_selected(hal_mesh *m, hal_delete_mode mode) {
    if (!m)
        return false;

    if (mode == HAL_DELETE_FACES) {
        if (hal_mesh_count_selected_faces(m) == 0)
            return false;
        bool *keep = malloc(m->face_count);
        if (!keep)
            return false;
        bool changed = false;
        for (size_t f = 0; f < m->face_count; f++) {
            keep[f] = !(m->face_sel && m->face_sel[f]);
            changed = changed || !keep[f];
        }
        if (changed)
            compact_faces(m, keep);
        free(keep);
        hal_mesh_select_all(m, false);
        hal_mesh_rebuild_edges(m);
        return changed;
    }

    if (hal_mesh_count_selected_verts(m) == 0)
        return false;
    bool *keep_vert = malloc(m->vert_count);
    hal_u32 *new_id = malloc(m->vert_count * sizeof(hal_u32));
    if (!keep_vert || !new_id) {
        free(keep_vert);
        free(new_id);
        return false;
    }
    size_t next = 0;
    for (size_t v = 0; v < m->vert_count; v++) {
        bool sel = m->vert_sel && m->vert_sel[v];
        keep_vert[v] = !sel;
        new_id[v] = (hal_u32)next;
        if (keep_vert[v])
            next++;
    }

    /* Faces touching a deleted vert die; survivors remap. */
    bool *keep_face = malloc(m->face_count ? m->face_count : 1);
    if (!keep_face) {
        free(keep_face);
        free(keep_vert);
        free(new_id);
        return false;
    }
    for (size_t f = 0; f < m->face_count; f++) {
        keep_face[f] = true;
        for (size_t i = 0; i < face_len(m, f); i++)
            if (!keep_vert[face_vert(m, f, i)]) {
                keep_face[f] = false;
                break;
            }
        if (keep_face[f])
            for (size_t i = 0; i < face_len(m, f); i++) {
                hal_u32 v = face_vert(m, f, i);
                m->loops[m->face_off[f] + i] = new_id[v];
            }
    }
    compact_faces(m, keep_face);
    free(keep_face);
    compact_verts(m, keep_vert, new_id);
    free(keep_vert);
    free(new_id);

    hal_mesh_select_all(m, false);
    hal_mesh_rebuild_edges(m);
    return true;
}

/* ---- Mirror / winding / normals ---- */

bool hal_mesh_mirror(hal_mesh *m, hal_mirror_axis axis) {
    if (!m || m->vert_count == 0)
        return false;
    for (size_t v = 0; v < m->vert_count; v++) {
        switch (axis) {
        case HAL_MIRROR_X: m->verts[v].x = -m->verts[v].x; break;
        case HAL_MIRROR_Y: m->verts[v].y = -m->verts[v].y; break;
        case HAL_MIRROR_Z: m->verts[v].z = -m->verts[v].z; break;
        }
    }
    hal_mesh_flip_winding(m);
    hal_mesh_rebuild_edges(m);
    return true;
}

void hal_mesh_flip_winding(hal_mesh *m) {
    if (!m)
        return;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = face_len(m, f);
        for (size_t i = 0; i < n / 2; i++) {
            hal_u32 tmp = m->loops[m->face_off[f] + i];
            m->loops[m->face_off[f] + i] =
                m->loops[m->face_off[f] + n - 1 - i];
            m->loops[m->face_off[f] + n - 1 - i] = tmp;
        }
    }
    hal_mesh_invalidate_derived(m);
}

void hal_mesh_rebuild_edges(hal_mesh *m) {
    if (!m)
        return;
    m->edge_count = 0;
    edge_map seen = { 0 };
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = face_len(m, f);
        for (size_t i = 0; i < n; i++) {
            hal_u32 a = face_vert(m, f, i);
            hal_u32 b = face_vert(m, f, (i + 1) % n);
            if (a == b)
                continue;
            hal_u32 dummy;
            if (edge_map_get(&seen, a, b, &dummy))
                continue;
            edge_map_put(&seen, a, b, 1);
            hal_mesh_edge_add(m, a, b);
        }
    }
    free(seen.pairs);
    hal_mesh_invalidate_derived(m);
}

void hal_mesh_recalc_normals(hal_mesh *m) {
    if (!m)
        return;
    hal_mesh_invalidate_derived(m);
    hal_mesh_face_normals(m);
    hal_mesh_vert_normals(m);
}

hal_vec3f hal_mesh_origin_to_center(hal_mesh *m) {
    if (!m || m->vert_count == 0)
        return hal_vec3f_zero();
    hal_vec3f bmin, bmax;
    hal_mesh_bounds(m, &bmin, &bmax);
    hal_vec3f center = hal_vec3f_mul(hal_vec3f_add(bmin, bmax), 0.5f);
    for (size_t v = 0; v < m->vert_count; v++)
        m->verts[v] = hal_vec3f_sub(m->verts[v], center);
    hal_mesh_invalidate_derived(m);
    return center;
}

hal_vec3f hal_mesh_origin_to_zero(hal_mesh *m) {
    if (!m)
        return hal_vec3f_zero();
    hal_vec3f offset = hal_vec3f_zero();
    /* Shifting verts by their own positions makes no sense; the
     * object's LOCATION is what moves — handled by the caller. */
    return offset;
}

/* Sort key for the clustering weld. */
typedef struct cluster_key {
    long kx, ky, kz;
    hal_u32 old_index;
    hal_u32 new_index;
} cluster_key;

static int cluster_cmp(const void *a, const void *b) {
    const cluster_key *ca = a, *cb = b;
    if (ca->kx != cb->kx)
        return ca->kx < cb->kx ? -1 : 1;
    if (ca->ky != cb->ky)
        return ca->ky < cb->ky ? -1 : 1;
    if (ca->kz != cb->kz)
        return ca->kz < cb->kz ? -1 : 1;
    if (ca->old_index != cb->old_index)
        return ca->old_index < cb->old_index ? -1 : 1;
    return 0;
}

bool hal_mesh_decimate_cluster(hal_mesh *m, hal_f32 cell) {
    if (!m || m->vert_count == 0 || cell <= 1e-6f)
        return false;

    cluster_key *keys = malloc(m->vert_count * sizeof(*keys));
    if (!keys)
        return false;
    for (size_t v = 0; v < m->vert_count; v++) {
        keys[v].kx = (long)floorf(m->verts[v].x / cell);
        keys[v].ky = (long)floorf(m->verts[v].y / cell);
        keys[v].kz = (long)floorf(m->verts[v].z / cell);
        keys[v].old_index = (hal_u32)v;
        keys[v].new_index = 0;
    }
    qsort(keys, m->vert_count, sizeof(*keys), cluster_cmp);

    /* Assign cluster ids in sorted order; the representative
     * position is the cluster centroid. */
    hal_u32 *remap = malloc(m->vert_count * sizeof(hal_u32));
    hal_vec3f *centroids = NULL;
    size_t cluster_count = 0;
    if (!remap)
        goto fail;
    centroids = malloc(m->vert_count * sizeof(*centroids));
    if (!centroids)
        goto fail;
    {
        size_t run_start = 0;
        for (size_t i = 0; i <= m->vert_count; i++) {
            bool new_run =
                i == m->vert_count ||
                (i > run_start &&
                 (keys[i].kx != keys[run_start].kx ||
                  keys[i].ky != keys[run_start].ky ||
                  keys[i].kz != keys[run_start].kz));
            if (new_run) {
                hal_vec3f c = hal_vec3f_zero();
                for (size_t k = run_start; k < i; k++)
                    c = hal_vec3f_add(
                        c, m->verts[keys[k].old_index]);
                c = hal_vec3f_mul(
                    c, 1.0f / (hal_f32)(i - run_start));
                centroids[cluster_count] = c;
                for (size_t k = run_start; k < i; k++)
                    keys[k].new_index = (hal_u32)cluster_count;
                cluster_count++;
                run_start = i;
            }
        }
    }
    for (size_t i = 0; i < m->vert_count; i++)
        remap[keys[i].old_index] = keys[i].new_index;

    /* Remap loops; a face survives ONLY when its deduped corner
     * count equals its run length (runs never shrink in place — a
     * shorter survivor would carry stale tail loops) and has no
     * wrap-around collapse. */
    bool *keep_face = malloc(m->face_count ? m->face_count : 1);
    if (!keep_face)
        goto fail;
    for (size_t f = 0; f < m->face_count; f++) {
        size_t n = face_len(m, f);
        if (n > 64) {
            keep_face[f] = false;
            continue;
        }
        hal_u32 dedup[64];
        size_t dn = 0;
        for (size_t i = 0; i < n; i++) {
            hal_u32 v = remap[face_vert(m, f, i)];
            if (dn == 0 || dedup[dn - 1] != v)
                dedup[dn++] = v;
        }
        if (dn >= 3 && dedup[0] == dedup[dn - 1])
            dn--; /* wrap collapse */
        bool ok = dn == n && dn >= 3;
        if (ok)
            for (size_t i = 0; i < n; i++)
                m->loops[m->face_off[f] + i] = dedup[i];
        keep_face[f] = ok;
    }
    compact_faces(m, keep_face);
    free(keep_face);

    /* Weld: the loops already reference cluster ids; replace the
     * vert array with the cluster centroids directly (compact_verts
     * is a DELETE helper — it would drop faces that reference
     * merged-away verts, which is wrong for welding). */
    {
        free(m->verts);
        m->verts = centroids;
        centroids = NULL; /* ownership moved */
        m->vert_count = cluster_count;
        m->vert_cap = cluster_count;
        free(m->vert_sel);
        m->vert_sel = calloc(cluster_count ? cluster_count : 1, 1);
        if (!m->vert_sel)
            goto fail; /* non-fatal: selection just clears */
        m->vert_sel_cap = cluster_count;
        hal_mesh_invalidate_derived(m);
    }

    free(keys);
    free(remap);
    free(centroids);
    hal_mesh_rebuild_edges(m);
    return true;

fail:
    free(keys);
    free(remap);
    free(centroids);
    return false;
}

bool hal_mesh_paint_weight(hal_mesh *m, hal_mat4f world,
                           hal_vec3f point, hal_f32 radius,
                           int bone, hal_f32 strength) {
    if (!m || !m->skin_weights || !m->skin_bones || radius <= 1e-5f ||
        bone < 0)
        return false;

    /* The brush point in mesh-local space. */
    hal_mat4f inv = hal_mat4f_invert(world);
    hal_vec3f local = hal_mat4f_transform_point(inv, point);
    /* Radius scaled by the world matrix's largest axis scale. */
    hal_vec3f axis_s = {
        hal_vec3f_length(hal_mat4f_transform_dir(world,
                                                 hal_vec3f_axis_x())),
        hal_vec3f_length(hal_mat4f_transform_dir(world,
                                                 hal_vec3f_axis_y())),
        hal_vec3f_length(hal_mat4f_transform_dir(world,
                                                 hal_vec3f_axis_z())),
    };
    hal_f32 scale = fmaxf(axis_s.x, fmaxf(axis_s.y, axis_s.z));
    if (scale < 1e-5f)
        scale = 1.0f;
    hal_f32 local_r = radius / scale;

    bool changed = false;
    for (size_t v = 0; v < m->vert_count; v++) {
        hal_f32 d = hal_vec3f_distance(m->verts[v], local);
        if (d >= local_r)
            continue;
        hal_f32 t = 1.0f - d / local_r;
        t = t * t * (3.0f - 2.0f * t); /* smoothstep falloff */

        /* Find the bone's slot (or a free one). */
        int slot = -1;
        int free_slot = -1;
        for (int i = 0; i < 4; i++) {
            size_t wi = v * 4 + (size_t)i;
            if (m->skin_bones[wi] == bone)
                slot = i;
            if (m->skin_bones[wi] < 0 && free_slot < 0)
                free_slot = i;
        }
        if (slot < 0 && free_slot < 0)
            continue; /* 4 influences used, none ours */
        if (slot < 0) {
            slot = free_slot;
            m->skin_bones[v * 4 + (size_t)slot] = bone;
            m->skin_weights[v * 4 + (size_t)slot] = 0.0f;
        }

        hal_f32 w =
            m->skin_weights[v * 4 + (size_t)slot] + strength * t;
        if (w < 0.0f)
            w = 0.0f;
        if (w > 1.0f)
            w = 1.0f;
        if (w != m->skin_weights[v * 4 + (size_t)slot]) {
            m->skin_weights[v * 4 + (size_t)slot] = w;
            changed = true;
        }

        /* Renormalize the surviving influences. */
        hal_f32 sum = 0.0f;
        for (int i = 0; i < 4; i++)
            if (m->skin_bones[v * 4 + (size_t)i] >= 0)
                sum += m->skin_weights[v * 4 + (size_t)i];
        if (sum > 1e-6f)
            for (int i = 0; i < 4; i++)
                m->skin_weights[v * 4 + (size_t)i] /= sum;
    }
    hal_mesh_invalidate_derived(m);
    return changed;
}

/* ---- Convex hull (quickhull, collider-grade) ----
 *
 * Faces keep outward normals; "outside" means dot(n, p) > d + eps.
 * Each iteration wraps the farthest outside point around its
 * visible region (horizon edges in the non-visible faces' winding).
 * Degenerate inputs (coplanar point sets) return NULL rather than a
 * broken mesh — callers fall back to a box collider. */

typedef struct hull_face {
    hal_u32 v[3];
    hal_vec3f n;
    hal_f32 d; /* plane: dot(n, x) = d, n outward */
    bool alive;
} hull_face;

static bool hull_make_face(hull_face *f, hal_u32 a, hal_u32 b,
                           hal_u32 c, const hal_vec3f *pts) {
    f->v[0] = a;
    f->v[1] = b;
    f->v[2] = c;
    f->alive = true;
    hal_vec3f e1 = hal_vec3f_sub(pts[b], pts[a]);
    hal_vec3f e2 = hal_vec3f_sub(pts[c], pts[a]);
    hal_vec3f n = hal_vec3f_cross(e1, e2);
    hal_f32 len = hal_vec3f_length(n);
    if (len < 1e-12f)
        return false; /* degenerate triangle */
    f->n = hal_vec3f_mul(n, 1.0f / len);
    f->d = hal_vec3f_dot(f->n, pts[a]);
    return true;
}

static bool hull_face_visible(const hull_face *f, hal_vec3f p,
                              hal_f32 eps) {
    return hal_vec3f_dot(f->n, p) > f->d + eps;
}

hal_mesh *hal_mesh_convex_hull(const hal_mesh *m) {
    if (!m || m->vert_count < 4)
        return NULL;

    /* Unique points (exact duplicates collapse — they would make
     * degenerate faces). */
    hal_vec3f *pts = malloc(m->vert_count * sizeof(*pts));
    if (!pts)
        return NULL;
    size_t pt_count = 0;
    for (size_t i = 0; i < m->vert_count; i++) {
        bool dup = false;
        for (size_t j = 0; j < pt_count; j++)
            if (hal_vec3f_distance(pts[j], m->verts[i]) < 1e-9f) {
                dup = true;
                break;
            }
        if (!dup)
            pts[pt_count++] = m->verts[i];
    }
    if (pt_count < 4) {
        free(pts);
        return NULL;
    }

    hal_vec3f bmin, bmax;
    hal_mesh_bounds((hal_mesh *)(size_t)m, &bmin, &bmax);
    hal_f32 eps = hal_vec3f_distance(bmin, bmax) * 1e-6f;

    /* The initial tetrahedron: X extremes, then farthest from the X
     * line, then farthest from that plane. */
    size_t a = 0, b = 0;
    for (size_t i = 1; i < pt_count; i++) {
        if (pts[i].x < pts[a].x)
            a = i;
        if (pts[i].x > pts[b].x)
            b = i;
    }
    if (a == b || hal_vec3f_distance(pts[a], pts[b]) < eps) {
        free(pts);
        return NULL;
    }
    size_t c = 0;
    hal_f32 best_c = -1.0f;
    for (size_t i = 0; i < pt_count; i++) {
        if (i == a || i == b)
            continue;
        hal_vec3f ab = hal_vec3f_sub(pts[b], pts[a]);
        hal_vec3f ap = hal_vec3f_sub(pts[i], pts[a]);
        hal_f32 d = hal_vec3f_length(hal_vec3f_cross(ab, ap));
        if (d > best_c) {
            best_c = d;
            c = i;
        }
    }
    if (best_c < eps) {
        free(pts); /* collinear */
        return NULL;
    }
    size_t d_i = 0;
    hal_f32 best_d = -1.0f;
    {
        hal_vec3f n = hal_vec3f_normalize(hal_vec3f_cross(
            hal_vec3f_sub(pts[b], pts[a]),
            hal_vec3f_sub(pts[c], pts[a])));
        hal_f32 dd = hal_vec3f_dot(n, pts[a]);
        for (size_t i = 0; i < pt_count; i++) {
            hal_f32 dist =
                fabsf(hal_vec3f_dot(n, pts[i]) - dd);
            if (dist > best_d) {
                best_d = dist;
                d_i = i;
            }
        }
    }
    if (best_d < eps) {
        free(pts); /* coplanar */
        return NULL;
    }
    if (d_i == a || d_i == b || d_i == c) {
        free(pts);
        return NULL;
    }

    /* Faces array (grows; dead faces stay as tombstones). */
    size_t face_cap = 64, face_count = 0;
    hull_face *faces = malloc(face_cap * sizeof(*faces));
    if (!faces) {
        free(pts);
        return NULL;
    }

    /* Orient the tetra outward: the 4th point must be BEHIND each
     * face (inside). */
    hal_u32 tv[4] = { (hal_u32)a, (hal_u32)b, (hal_u32)c,
                      (hal_u32)d_i };
    static const int tri[4][3] = { { 0, 1, 2 }, { 0, 2, 3 },
                                   { 0, 3, 1 }, { 1, 3, 2 } };
    for (int t = 0; t < 4; t++) {
        hull_face f;
        if (!hull_make_face(&f, tv[tri[t][0]], tv[tri[t][1]],
                            tv[tri[t][2]], pts)) {
            free(pts);
            free(faces);
            return NULL;
        }
        /* Flip so the opposite tetra vertex is behind. */
        hal_u32 opp = tv[0] == f.v[0] || tv[0] == f.v[1] ||
                              tv[0] == f.v[2]
                          ? (tv[1] == f.v[0] || tv[1] == f.v[1] ||
                                     tv[1] == f.v[2]
                                 ? (tv[2] == f.v[0] ||
                                            tv[2] == f.v[1] ||
                                            tv[2] == f.v[2]
                                        ? tv[3]
                                        : tv[2])
                                 : tv[1])
                          : tv[0];
        if (hal_vec3f_dot(f.n, pts[opp]) > f.d) {
            hal_u32 tmp = f.v[1];
            f.v[1] = f.v[2];
            f.v[2] = tmp;
            f.n = hal_vec3f_mul(f.n, -1.0f);
            f.d = -f.d;
        }
        if (face_count == face_cap) {
            face_cap *= 2;
            hull_face *grown =
                realloc(faces, face_cap * sizeof(*faces));
            if (!grown) {
                free(pts);
                free(faces);
                return NULL;
            }
            faces = grown;
        }
        faces[face_count++] = f;
    }

    /* The remaining points wait in the pool. */
    bool *in_pool = calloc(pt_count, sizeof(bool));
    hal_u32 *pool = malloc(pt_count * sizeof(hal_u32));
    if (!in_pool || !pool) {
        free(in_pool);
        free(pool);
        free(pts);
        free(faces);
        return NULL;
    }
    size_t pool_count = 0;
    for (size_t i = 0; i < pt_count; i++) {
        if (i == a || i == b || i == c || i == d_i)
            continue;
        in_pool[i] = true;
        pool[pool_count++] = (hal_u32)i;
    }

    size_t guard = pt_count * 4 + 1024;
    while (guard-- > 0) {
        /* Any face with an outside point? */
        bool *visible = calloc(face_count, sizeof(bool));
        if (!visible)
            break;
        int eye = -1;
        hal_f32 eye_dist = -1.0f;
        size_t eye_face = (size_t)-1;
        for (size_t fi = 0; fi < face_count; fi++) {
            if (!faces[fi].alive)
                continue;
            for (size_t pi = 0; pi < pool_count; pi++) {
                hal_u32 p = pool[pi];
                if (!in_pool[p])
                    continue;
                if (hull_face_visible(&faces[fi], pts[p], eps)) {
                    hal_f32 dist = hal_vec3f_dot(faces[fi].n,
                                                 pts[p]) -
                                   faces[fi].d;
                    if (dist > eye_dist) {
                        eye_dist = dist;
                        eye = (int)p;
                        eye_face = fi;
                    }
                }
            }
        }
        if (eye < 0) {
            free(visible);
            break; /* hull complete */
        }
        (void)eye_face;

        /* The visible region from the eye point. */
        for (size_t fi = 0; fi < face_count; fi++) {
            if (!faces[fi].alive)
                continue;
            visible[fi] = hull_face_visible(&faces[fi],
                                            pts[eye], eps);
        }

        /* Horizon: edges of alive faces used by exactly one visible
         * face... simpler: edges shared between a visible and a
         * non-visible alive face. Collect from the NON-visible side
         * in ITS winding. */
        typedef struct {
            hal_u32 a, b; /* as wound on the non-visible face */
        } h_edge;
        h_edge *horizon = malloc(face_count * 3 * sizeof(*horizon));
        if (!horizon) {
            free(visible);
            break;
        }
        size_t horizon_count = 0;
        for (size_t fi = 0; fi < face_count; fi++) {
            if (!faces[fi].alive || visible[fi])
                continue;
            for (int e = 0; e < 3; e++) {
                hal_u32 ea = faces[fi].v[e];
                hal_u32 eb = faces[fi].v[(e + 1) % 3];
                /* Is the reversed edge on a visible face? */
                for (size_t fj = 0; fj < face_count; fj++) {
                    if (!faces[fj].alive || !visible[fj] || fj == fi)
                        continue;
                    for (int e2 = 0; e2 < 3; e2++) {
                        if (faces[fj].v[e2] == eb &&
                            faces[fj].v[(e2 + 1) % 3] == ea) {
                            horizon[horizon_count].a = ea;
                            horizon[horizon_count].b = eb;
                            horizon_count++;
                            break;
                        }
                    }
                }
            }
        }
        if (horizon_count == 0) {
            free(horizon);
            free(visible);
            break; /* numerical dead end: stop, keep the hull so far */
        }

        /* Kill visible faces; add a face per horizon edge. */
        for (size_t fi = 0; fi < face_count; fi++)
            if (visible[fi])
                faces[fi].alive = false;
        bool add_ok = true;
        for (size_t hi = 0; hi < horizon_count; hi++) {
            hull_face f;
            /* The new face traverses the horizon edge OPPOSITE to
             * the non-visible face's winding — the consistent
             * orientation for outward normals (same-direction would
             * flip every wrap face inward). */
            if (!hull_make_face(&f, horizon[hi].b, horizon[hi].a,
                                (hal_u32)eye, pts)) {
                add_ok = false;
                break; /* sliver: skip this edge (rare, near-degenerate) */
            }
            /* Orient outward: the eye is ON the new faces; use any
             * remaining pool point? The horizon winding already
             * guarantees outward orientation when the non-visible
             * face was outward — trust it, but verify against the
             * face's own centroid test: flip if the HULL CENTROID is
             * in front. */
            if (face_count == face_cap) {
                face_cap *= 2;
                hull_face *grown =
                    realloc(faces, face_cap * sizeof(*faces));
                if (!grown) {
                    add_ok = false;
                    break;
                }
                faces = grown;
            }
            faces[face_count++] = f;
        }
        free(horizon);
        free(visible);
        if (!add_ok)
            break;

        /* Re-pool: points still outside ANY alive face stay. */
        for (size_t pi = 0; pi < pool_count; pi++) {
            hal_u32 p = pool[pi];
            if (!in_pool[p])
                continue;
            bool outside = false;
            for (size_t fi = 0; fi < face_count && !outside; fi++)
                if (faces[fi].alive &&
                    hull_face_visible(&faces[fi], pts[p], eps))
                    outside = true;
            if (!outside)
                in_pool[p] = false; /* inside (or on) the hull */
        }
    }

    /* Compact the pool array (membership via in_pool). */
    (void)pool;

    /* Build the output mesh from the alive faces. */
    bool *used = calloc(pt_count, sizeof(bool));
    if (!used) {
        free(in_pool);
        free(pool);
        free(pts);
        free(faces);
        return NULL;
    }
    for (size_t fi = 0; fi < face_count; fi++)
        if (faces[fi].alive)
            for (int e = 0; e < 3; e++)
                used[faces[fi].v[e]] = true;
    int *new_id = malloc(pt_count * sizeof(int));
    if (!new_id) {
        free(used);
        free(in_pool);
        free(pool);
        free(pts);
        free(faces);
        return NULL;
    }
    hal_mesh *out = hal_mesh_create();
    if (!out) {
        free(new_id);
        free(used);
        free(in_pool);
        free(pool);
        free(pts);
        free(faces);
        return NULL;
    }
    for (size_t i = 0; i < pt_count; i++) {
        new_id[i] = -1;
        if (used[i]) {
            hal_mesh_vert_add(out, pts[i]);
            new_id[i] = (int)out->vert_count - 1;
        }
    }
    for (size_t fi = 0; fi < face_count; fi++) {
        if (!faces[fi].alive)
            continue;
        hal_u32 tri2[3];
        bool ok_ids = true;
        for (int e = 0; e < 3; e++) {
            int id = new_id[faces[fi].v[e]];
            if (id < 0)
                ok_ids = false;
            tri2[e] = (hal_u32)(id < 0 ? 0 : id);
        }
        if (ok_ids)
            hal_mesh_face_add(out, tri2, 3);
    }

    free(new_id);
    free(used);
    free(in_pool);
    free(pool);
    free(pts);
    free(faces);
    if (out->face_count < 4 || out->vert_count < 4) {
        hal_mesh_destroy(out);
        return NULL; /* degenerate result */
    }
    hal_mesh_rebuild_edges(out);
    return out;
}
