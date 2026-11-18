/*
 * mesh.c — mesh structure, normals, bounds, primitives
 */

#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

hal_mesh *hal_mesh_create(void) {
    /* calloc zeroes everything: face_off is NULL with face_cap 0 until
     * the first face_add grows it — allocation and capacity can never
     * disagree. face_off[0] (the leading 0) is written there. */
    return calloc(1, sizeof(hal_mesh));
}

void hal_mesh_destroy(hal_mesh *mesh) {
    if (!mesh)
        return;
    free(mesh->verts);
    free(mesh->edges);
    free(mesh->loops);
    free(mesh->face_off);
    free(mesh->face_normals);
    free(mesh->vert_normals);
    free(mesh);
}

static void invalidate_derived(hal_mesh *m) {
    free(m->face_normals);
    m->face_normals = NULL;
    free(m->vert_normals);
    m->vert_normals = NULL;
    m->bbox_valid = false;
}

/* Grows `count` (elements used) + `cap` + `buf` triplets. Written
 * longhand per array after a growth bug hid behind a void** helper:
 * each version is dumb, direct, and impossible to mis-apply. */
static bool grow_bytes(void **buf, size_t *cap, size_t need_elems,
                       size_t elem_size) {
    if (need_elems <= *cap)
        return true;
    size_t new_cap = *cap ? *cap : 16;
    while (new_cap < need_elems)
        new_cap *= 2;
    void *grown = realloc(*buf, new_cap * elem_size);
    if (!grown)
        return false;
    *buf = grown;
    *cap = new_cap;
    return true;
}

int hal_mesh_vert_add(hal_mesh *m, hal_vec3f p) {
    if (!m || !grow_bytes((void **)&m->verts, &m->vert_cap,
                          m->vert_count + 1, sizeof(hal_vec3f)))
        return -1;
    m->verts[m->vert_count] = p;
    invalidate_derived(m);
    return (int)m->vert_count++;
}

int hal_mesh_edge_add(hal_mesh *m, hal_u32 a, hal_u32 b) {
    if (!m || a == b || a >= (hal_u32)m->vert_count ||
        b >= (hal_u32)m->vert_count)
        return -1;
    if (!grow_bytes((void **)&m->edges, &m->edge_cap, m->edge_count + 1,
                    sizeof(hal_u32) * 2))
        return -1;
    m->edges[m->edge_count * 2] = a;
    m->edges[m->edge_count * 2 + 1] = b;
    m->edge_count++;
    return (int)m->edge_count - 1;
}

int hal_mesh_face_add(hal_mesh *m, const hal_u32 *corners, size_t count) {
    if (!m || !corners || count < 3)
        return -1;
    for (size_t i = 0; i < count; i++)
        if (corners[i] >= (hal_u32)m->vert_count)
            return -1;

    if (!grow_bytes((void **)&m->loops, &m->loop_cap, m->loop_count + count,
                    sizeof(hal_u32)))
        return -1;
    /* face_off needs face_count + 2 live entries when a face is being
     * added: indices 0..face_count+1 (the leading 0 plus the new
     * end offset). This +2 was the overflow: the old code grew to
     * face_count+1 and then wrote index face_count+1 after ++. */
    if (!grow_bytes((void **)&m->face_off, &m->face_cap, m->face_count + 2,
                    sizeof(size_t)))
        return -1;

    for (size_t i = 0; i < count; i++)
        m->loops[m->loop_count + i] = corners[i];
    m->loop_count += count;
    if (m->face_count == 0)
        m->face_off[0] = 0; /* the leading offset */
    m->face_count++;
    m->face_off[m->face_count] = m->loop_count;
    invalidate_derived(m);
    return (int)m->face_count - 1;
}

/* ---- Normals ---- */

static void compute_normals(hal_mesh *m) {
    if (m->face_normals && m->vert_normals)
        return;

    if (!m->face_normals)
        m->face_normals = calloc(m->face_count ? m->face_count : 1,
                                 sizeof(*m->face_normals));
    if (!m->vert_normals)
        m->vert_normals = calloc(m->vert_count ? m->vert_count : 1,
                                 sizeof(*m->vert_normals));
    if (!m->face_normals || !m->vert_normals)
        return;

    for (size_t f = 0; f < m->face_count; f++) {
        size_t start = m->face_off[f];
        size_t count = m->face_off[f + 1] - start;
        /* Newell's method: robust for non-perfectly-planar polygons. */
        hal_vec3f n = hal_vec3f_zero();
        for (size_t i = 0; i < count; i++) {
            hal_vec3f cur = m->verts[m->loops[start + i]];
            hal_vec3f nxt = m->verts[m->loops[start + (i + 1) % count]];
            n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
            n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
            n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
        }
        m->face_normals[f] = hal_vec3f_normalize(n);

        /* Area-weighted accumulation into the corner vertices. */
        for (size_t i = 0; i < count; i++)
            m->vert_normals[m->loops[start + i]] =
                hal_vec3f_add(m->vert_normals[m->loops[start + i]],
                              m->face_normals[f]);
    }
    for (size_t v = 0; v < m->vert_count; v++)
        m->vert_normals[v] = hal_vec3f_normalize(m->vert_normals[v]);
}

const hal_vec3f *hal_mesh_face_normals(hal_mesh *m) {
    if (!m)
        return NULL;
    compute_normals(m);
    return m->face_normals;
}

const hal_vec3f *hal_mesh_vert_normals(hal_mesh *m) {
    if (!m)
        return NULL;
    compute_normals(m);
    return m->vert_normals;
}

/* ---- Bounds ---- */

void hal_mesh_bounds(hal_mesh *m, hal_vec3f *out_min, hal_vec3f *out_max) {
    if (!m || !out_min || !out_max)
        return;
    if (!m->bbox_valid) {
        if (m->vert_count == 0) {
            m->bbox_min = hal_vec3f_zero();
            m->bbox_max = hal_vec3f_zero();
        } else {
            m->bbox_min = m->verts[0];
            m->bbox_max = m->verts[0];
            for (size_t i = 1; i < m->vert_count; i++) {
                m->bbox_min.x = fminf(m->bbox_min.x, m->verts[i].x);
                m->bbox_min.y = fminf(m->bbox_min.y, m->verts[i].y);
                m->bbox_min.z = fminf(m->bbox_min.z, m->verts[i].z);
                m->bbox_max.x = fmaxf(m->bbox_max.x, m->verts[i].x);
                m->bbox_max.y = fmaxf(m->bbox_max.y, m->verts[i].y);
                m->bbox_max.z = fmaxf(m->bbox_max.z, m->verts[i].z);
            }
        }
        m->bbox_valid = true;
    }
    *out_min = m->bbox_min;
    *out_max = m->bbox_max;
}

hal_f32 hal_mesh_bounds_radius(hal_mesh *m) {
    if (!m || m->vert_count == 0)
        return 0.0f;
    hal_f32 r2 = 0.0f;
    for (size_t i = 0; i < m->vert_count; i++) {
        hal_f32 d = hal_vec3f_length_sq(m->verts[i]);
        if (d > r2)
            r2 = d;
    }
    return (hal_f32)__builtin_sqrt((double)r2);
}

/* ---- Primitives ---- */

/* Adds verts + a quad face + the quad's edges. */
static void quad(hal_mesh *m, hal_u32 a, hal_u32 b, hal_u32 c, hal_u32 d) {
    hal_u32 corners[4] = { a, b, c, d };
    hal_mesh_face_add(m, corners, 4);
    hal_mesh_edge_add(m, a, b);
    hal_mesh_edge_add(m, b, c);
    hal_mesh_edge_add(m, c, d);
    hal_mesh_edge_add(m, d, a);
}

static void tri(hal_mesh *m, hal_u32 a, hal_u32 b, hal_u32 c) {
    hal_u32 corners[3] = { a, b, c };
    hal_mesh_face_add(m, corners, 3);
    hal_mesh_edge_add(m, a, b);
    hal_mesh_edge_add(m, b, c);
    hal_mesh_edge_add(m, c, a);
}

hal_mesh *hal_mesh_cube(hal_f32 size) {
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;
    hal_f32 s = size;
    /* Bottom four (CCW seen from below), then top four. */
    hal_mesh_vert_add(m, hal_vec3f_make(-s, -s, -s));
    hal_mesh_vert_add(m, hal_vec3f_make(s, -s, -s));
    hal_mesh_vert_add(m, hal_vec3f_make(s, -s, s));
    hal_mesh_vert_add(m, hal_vec3f_make(-s, -s, s));
    hal_mesh_vert_add(m, hal_vec3f_make(-s, s, -s));
    hal_mesh_vert_add(m, hal_vec3f_make(s, s, -s));
    hal_mesh_vert_add(m, hal_vec3f_make(s, s, s));
    hal_mesh_vert_add(m, hal_vec3f_make(-s, s, s));

    quad(m, 3, 2, 1, 0); /* bottom (-Y) */
    quad(m, 4, 5, 6, 7); /* top (+Y)    */
    quad(m, 0, 1, 5, 4); /* -Z          */
    quad(m, 1, 2, 6, 5); /* +X          */
    quad(m, 2, 3, 7, 6); /* +Z          */
    quad(m, 3, 0, 4, 7); /* -X          */
    return m;
}

hal_mesh *hal_mesh_sphere(hal_f32 radius, int rings, int segments) {
    if (rings < 2)
        rings = 2;
    if (segments < 3)
        segments = 3;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;

    /* rings-1 latitude bands of `segments` verts, plus two poles. */
    for (int r = 1; r < rings; r++) {
        hal_f32 phi = (hal_f32)r / (hal_f32)rings * HAL_PI_f32;
        for (int s = 0; s < segments; s++) {
            hal_f32 theta =
                (hal_f32)s / (hal_f32)segments * HAL_TAU_f32;
            hal_mesh_vert_add(
                m, hal_vec3f_make(
                       radius * (hal_f32)__builtin_sin((double)phi) *
                           (hal_f32)__builtin_cos((double)theta),
                       radius * (hal_f32)__builtin_cos((double)phi),
                       radius * (hal_f32)__builtin_sin((double)phi) *
                           (hal_f32)__builtin_sin((double)theta)));
        }
    }
    hal_u32 north = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, radius, 0.0f));
    hal_u32 south = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, -radius, 0.0f));

#define SPH(r, s) ((hal_u32)(((r)-1) * segments + ((s) % segments)))
    /* North cap triangles (CCW seen from outside, +Y up). */
    for (int s = 0; s < segments; s++)
        tri(m, north, SPH(1, s + 1), SPH(1, s));
    /* Middle quads: (theta-tangent) x (phi-tangent) points outward
     * (verified analytically at the equator; see test_mesh). */
    for (int r = 1; r < rings - 1; r++)
        for (int s = 0; s < segments; s++)
            quad(m, SPH(r, s), SPH(r, s + 1), SPH(r + 1, s + 1),
                 SPH(r + 1, s));
    /* South cap triangles. */
    for (int s = 0; s < segments; s++)
        tri(m, south, SPH(rings - 1, s), SPH(rings - 1, s + 1));
#undef SPH
    return m;
}

hal_mesh *hal_mesh_plane(hal_f32 size) {
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;
    hal_mesh_vert_add(m, hal_vec3f_make(-size, 0.0f, -size));
    hal_mesh_vert_add(m, hal_vec3f_make(size, 0.0f, -size));
    hal_mesh_vert_add(m, hal_vec3f_make(size, 0.0f, size));
    hal_mesh_vert_add(m, hal_vec3f_make(-size, 0.0f, size));
    quad(m, 0, 3, 2, 1); /* CCW seen from +Y */
    return m;
}

hal_mesh *hal_mesh_cylinder(hal_f32 radius, hal_f32 height, int segments) {
    if (segments < 3)
        segments = 3;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;
    hal_f32 hy = height * 0.5f;

    for (int ring = 0; ring < 2; ring++) {
        hal_f32 y = ring == 0 ? -hy : hy;
        for (int s = 0; s < segments; s++) {
            hal_f32 theta =
                (hal_f32)s / (hal_f32)segments * HAL_TAU_f32;
            hal_mesh_vert_add(
                m, hal_vec3f_make(
                       radius * (hal_f32)__builtin_cos((double)theta), y,
                       radius * (hal_f32)__builtin_sin((double)theta)));
        }
    }
    hal_u32 top_center = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, hy, 0.0f));
    hal_u32 bot_center = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, -hy, 0.0f));

#define CYL(ring, s) ((hal_u32)((ring) * segments + ((s) % segments)))
    for (int s = 0; s < segments; s++)
        quad(m, CYL(0, s), CYL(1, s), CYL(1, s + 1), CYL(0, s + 1));
    for (int s = 0; s < segments; s++)
        tri(m, top_center, CYL(1, s + 1), CYL(1, s));
    for (int s = 0; s < segments; s++)
        tri(m, bot_center, CYL(0, s), CYL(0, s + 1));
#undef CYL
    return m;
}

hal_mesh *hal_mesh_cone(hal_f32 radius, hal_f32 height, int segments) {
    if (segments < 3)
        segments = 3;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;
    hal_f32 hy = height * 0.5f;

    for (int s = 0; s < segments; s++) {
        hal_f32 theta = (hal_f32)s / (hal_f32)segments * HAL_TAU_f32;
        hal_mesh_vert_add(
            m, hal_vec3f_make(
                   radius * (hal_f32)__builtin_cos((double)theta), -hy,
                   radius * (hal_f32)__builtin_sin((double)theta)));
    }
    hal_u32 tip = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, hy, 0.0f));
    hal_u32 base_center = (hal_u32)hal_mesh_vert_add(
        m, hal_vec3f_make(0.0f, -hy, 0.0f));

    for (int s = 0; s < segments; s++) {
        hal_u32 a = (hal_u32)(s % segments);
        hal_u32 b = (hal_u32)((s + 1) % segments);
        tri(m, tip, b, a);          /* side, CCW from outside */
        tri(m, base_center, a, b);  /* base cap */
    }
    return m;
}

hal_mesh *hal_mesh_torus(hal_f32 main_radius, hal_f32 tube_radius,
                         int main_segments, int tube_segments) {
    if (main_segments < 3)
        main_segments = 3;
    if (tube_segments < 3)
        tube_segments = 3;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;

    for (int i = 0; i < main_segments; i++) {
        hal_f32 u = (hal_f32)i / (hal_f32)main_segments * HAL_TAU_f32;
        for (int j = 0; j < tube_segments; j++) {
            hal_f32 v = (hal_f32)j / (hal_f32)tube_segments * HAL_TAU_f32;
            hal_f32 cu = (hal_f32)__builtin_cos((double)u);
            hal_f32 su = (hal_f32)__builtin_sin((double)u);
            hal_f32 cv = (hal_f32)__builtin_cos((double)v);
            hal_f32 sv = (hal_f32)__builtin_sin((double)v);
            hal_mesh_vert_add(
                m, hal_vec3f_make((main_radius + tube_radius * cv) * cu,
                                  tube_radius * sv,
                                  (main_radius + tube_radius * cv) * su));
        }
    }

#define TOR(i, j) \
    ((hal_u32)(((i) % main_segments) * tube_segments + ((j) % tube_segments)))
    for (int i = 0; i < main_segments; i++)
        for (int j = 0; j < tube_segments; j++)
            quad(m, TOR(i, j), TOR(i, j + 1), TOR(i + 1, j + 1),
                 TOR(i + 1, j));
#undef TOR
    return m;
}
