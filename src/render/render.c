/*
 * render.c — the viewport renderer: grid, objects, gizmo
 */

#include "render.h"
#include "raster.h"

#include <math.h>
#include <string.h>

hal_render_opts hal_render_opts_default(void) {
    hal_render_opts opts = {
        .shading = HAL_SHADING_SOLID,
        .bg = hal_color_hex(0x10142eu),
        .grid = hal_color_hex(0x3a4270u),
        .object = hal_color_rgb8(0xb9, 0xb2, 0xe0),
        .draw_grid = true,
        .draw_gizmo = true,
    };
    return opts;
}

/* ---- Homogeneous clipping ----
 * A polygon (in clip space) clipped against the near plane
 * (w >= eps). Returns the new vertex count (<= 8). */
#define CLIP_MAX 8

typedef struct clip_vertex {
    hal_vec4f pos;
    hal_vec3f normal; /* world normal for shading        */
    hal_f32 intensity;
} clip_vertex;

static size_t clip_near(clip_vertex *poly, size_t n, size_t cap) {
    if (n == 0)
        return 0;
    clip_vertex out[CLIP_MAX];
    size_t m = 0;
    if (cap > CLIP_MAX)
        cap = CLIP_MAX;
    for (size_t i = 0; i < n; i++) {
        clip_vertex cur = poly[i];
        clip_vertex nxt = poly[(i + 1) % n];
        hal_f32 d_cur = cur.pos.w - 1e-4f;
        hal_f32 d_nxt = nxt.pos.w - 1e-4f;
        if (d_cur >= 0.0f) {
            if (m < cap)
                out[m++] = cur;
            if (d_nxt < 0.0f && m < cap) {
                hal_f32 t = d_cur / (d_cur - d_nxt);
                clip_vertex v;
                v.pos = hal_vec4f_make(
                    cur.pos.x + (nxt.pos.x - cur.pos.x) * t,
                    cur.pos.y + (nxt.pos.y - cur.pos.y) * t,
                    cur.pos.z + (nxt.pos.z - cur.pos.z) * t,
                    cur.pos.w + (nxt.pos.w - cur.pos.w) * t);
                v.normal = hal_vec3f_lerp(cur.normal, nxt.normal, t);
                v.intensity =
                    cur.intensity + (nxt.intensity - cur.intensity) * t;
                out[m++] = v;
            }
        } else if (d_nxt >= 0.0f && m < cap) {
            hal_f32 t = d_cur / (d_cur - d_nxt);
            clip_vertex v;
            v.pos = hal_vec4f_make(
                cur.pos.x + (nxt.pos.x - cur.pos.x) * t,
                cur.pos.y + (nxt.pos.y - cur.pos.y) * t,
                cur.pos.z + (nxt.pos.z - cur.pos.z) * t,
                cur.pos.w + (nxt.pos.w - cur.pos.w) * t);
            v.normal = hal_vec3f_lerp(cur.normal, nxt.normal, t);
            v.intensity =
                cur.intensity + (nxt.intensity - cur.intensity) * t;
            out[m++] = v;
        }
    }
    memcpy(poly, out, m * sizeof(clip_vertex));
    return m;
}

/* Projects a clip vertex to canvas physical pixels + NDC z. */
static hal_vec3f to_screen(hal_vec4f clip, hal_f32 scale) {
    hal_f32 iw = 1.0f / clip.w;
    return hal_vec3f_make(
        (clip.x * iw * 0.5f + 0.5f) * scale, /* caller offsets below */
        (0.5f - clip.y * iw * 0.5f) * scale,
        clip.z * iw);
}

/* ---- The grid ---- */

static void render_grid(hal_raster *raster, hal_mat4f view_proj,
                        hal_canvas *canvas, hal_rectf viewport,
                        hal_color grid_color) {
    hal_f32 scale = canvas->scale;
    hal_f32 vx = viewport.x * scale, vy = viewport.y * scale;
    hal_f32 vw = viewport.width * scale, vh = viewport.height * scale;
    (void)vw;
    (void)vh;

    /* Lines on the XZ plane, 1 m apart, out to a fixed radius; the
     * frustum culls what is off screen. 40 m covers a large working
     * area without swamping the line count. */
    const int extent = 40;
    for (int i = -extent; i <= extent; i++) {
        clip_vertex line[CLIP_MAX];

        /* Parallel to X at z = i. */
        line[0].pos = hal_mat4f_mul_vec4(
            view_proj,
            hal_vec4f_make((hal_f32)-extent, 0.0f, (hal_f32)i, 1.0f));
        line[1].pos = hal_mat4f_mul_vec4(
            view_proj,
            hal_vec4f_make((hal_f32)extent, 0.0f, (hal_f32)i, 1.0f));
        size_t n = clip_near(line, 2, CLIP_MAX);
        if (n == 2) {
            hal_vec3f a = to_screen(line[0].pos, 1.0f);
            hal_vec3f b = to_screen(line[1].pos, 1.0f);
            hal_raster_line(raster,
                            hal_vec3f_make(a.x * vw + vx, a.y * vh + vy,
                                           a.z),
                            hal_vec3f_make(b.x * vw + vx, b.y * vh + vy,
                                           b.z),
                            grid_color);
        }

        /* Parallel to Z at x = i. */
        line[0].pos = hal_mat4f_mul_vec4(
            view_proj,
            hal_vec4f_make((hal_f32)i, 0.0f, (hal_f32)-extent, 1.0f));
        line[1].pos = hal_mat4f_mul_vec4(
            view_proj,
            hal_vec4f_make((hal_f32)i, 0.0f, (hal_f32)extent, 1.0f));
        n = clip_near(line, 2, CLIP_MAX);
        if (n == 2) {
            hal_vec3f a = to_screen(line[0].pos, 1.0f);
            hal_vec3f b = to_screen(line[1].pos, 1.0f);
            hal_raster_line(raster,
                            hal_vec3f_make(a.x * vw + vx, a.y * vh + vy,
                                           a.z),
                            hal_vec3f_make(b.x * vw + vx, b.y * vh + vy,
                                           b.z),
                            grid_color);
        }
    }
}

/* ---- Objects ---- */

static void render_mesh_solid(hal_raster *raster, hal_mesh *mesh,
                              hal_mat4f world, hal_mat4f view_proj,
                              hal_vec3f eye, bool smooth, hal_f32 ox,
                              hal_f32 oy, hal_f32 vw, hal_f32 vh,
                              hal_color base) {
    const hal_vec3f *fn = hal_mesh_face_normals(mesh);
    const hal_vec3f *vn = smooth ? hal_mesh_vert_normals(mesh) : NULL;
    if (!fn || (smooth && !vn))
        return;

    for (size_t f = 0; f < mesh->face_count; f++) {
        size_t start = mesh->face_off[f];
        size_t count = mesh->face_off[f + 1] - start;
        if (count < 3)
            continue;

        /* World-space face normal (rotation part; scenes keep scale
         * uniform per axis and normalize afterwards). */
        hal_vec3f wn = hal_mat4f_transform_dir(world, fn[f]);
        wn = hal_vec3f_normalize(wn);

        /* Headlight: the light sits at the camera eye, so whatever
         * the user looks at is lit — calm, even, never a black face
         * pointing at you. */
        hal_vec3f centroid = hal_vec3f_zero();
        for (size_t i = 0; i < count; i++)
            centroid = hal_vec3f_add(
                centroid,
                hal_mat4f_transform_point(
                    world, mesh->verts[mesh->loops[start + i]]));
        centroid = hal_vec3f_mul(centroid, 1.0f / (hal_f32)count);
        hal_vec3f light = hal_vec3f_normalize(
            hal_vec3f_sub(eye, centroid));
        hal_f32 d = hal_vec3f_dot(wn, light);
        if (d < 0.0f)
            d = 0.0f;
        hal_f32 shade = 0.18f + 0.82f * d;
        hal_color shaded = hal_color_rgb(base.r * shade, base.g * shade,
                                         base.b * shade);
        /* Smooth shading: per-corner lambert from the vertex normals
         * (against each corner's own light direction); flat: the
         * face's value everywhere. */
        hal_f32 flat_i = shade;

        /* Fan-triangulate (Halcyon's faces are convex). */
        for (size_t i = 1; i + 1 < count; i++) {
            clip_vertex poly[3];
            size_t idx[3] = { mesh->loops[start],
                              mesh->loops[start + i],
                              mesh->loops[start + i + 1] };
            for (int k = 0; k < 3; k++) {
                hal_vec3f wp = hal_mat4f_transform_point(
                    world, mesh->verts[idx[k]]);
                poly[k].pos = hal_mat4f_mul_vec4(
                    view_proj, hal_vec4f_make(wp.x, wp.y, wp.z, 1.0f));
                poly[k].normal = fn[f];
                poly[k].intensity = flat_i;
                if (vn) {
                    hal_vec3f n =
                        hal_vec3f_normalize(hal_mat4f_transform_dir(
                            world, vn[idx[k]]));
                    hal_vec3f ldir = hal_vec3f_normalize(
                        hal_vec3f_sub(eye, wp));
                    hal_f32 dd = hal_vec3f_dot(n, ldir);
                    if (dd < 0.0f)
                        dd = 0.0f;
                    poly[k].intensity = 0.18f + 0.82f * dd;
                }
            }

            size_t n = 3;
            bool clipped = false;
            for (int k = 0; k < 3; k++)
                if (poly[k].pos.w < 1e-4f)
                    clipped = true;
            if (clipped) {
                n = clip_near(poly, 3, 3);
                if (n < 3)
                    continue;
            }

            for (size_t t = clipped ? 1 : 1; t + 1 < n; t++) {
                hal_vec3f s0 = to_screen(poly[0].pos, 1.0f);
                hal_vec3f s1 = to_screen(poly[t].pos, 1.0f);
                hal_vec3f s2 = to_screen(poly[t + 1].pos, 1.0f);
                hal_raster_triangle(
                    raster,
                    hal_vec3f_make(s0.x * vw + ox, s0.y * vh + oy, s0.z),
                    poly[0].intensity,
                    hal_vec3f_make(s1.x * vw + ox, s1.y * vh + oy, s1.z),
                    poly[t].intensity,
                    hal_vec3f_make(s2.x * vw + ox, s2.y * vh + oy, s2.z),
                    poly[t + 1].intensity, shaded);
            }
        }
    }
}

static void render_mesh_wire(hal_raster *raster, hal_mesh *mesh,
                             hal_mat4f world, hal_mat4f view_proj,
                             hal_f32 ox, hal_f32 oy, hal_f32 vw,
                             hal_f32 vh, hal_color base) {
    for (size_t e = 0; e < mesh->edge_count; e++) {
        hal_u32 a = mesh->edges[e * 2];
        hal_u32 b = mesh->edges[e * 2 + 1];
        hal_vec3f wa =
            hal_mat4f_transform_point(world, mesh->verts[a]);
        hal_vec3f wb =
            hal_mat4f_transform_point(world, mesh->verts[b]);
        hal_vec4f ca = hal_mat4f_mul_vec4(
            view_proj, hal_vec4f_make(wa.x, wa.y, wa.z, 1.0f));
        hal_vec4f cb = hal_mat4f_mul_vec4(
            view_proj, hal_vec4f_make(wb.x, wb.y, wb.z, 1.0f));

        clip_vertex line[CLIP_MAX] = { { ca, hal_vec3f_zero(), 1.0f },
                                     { cb, hal_vec3f_zero(), 1.0f } };
        size_t n = clip_near(line, 2, CLIP_MAX);
        if (n != 2)
            continue;
        hal_vec3f sa = to_screen(line[0].pos, 1.0f);
        hal_vec3f sb = to_screen(line[1].pos, 1.0f);
        hal_raster_line(
            raster,
            hal_vec3f_make(sa.x * vw + ox, sa.y * vh + oy, sa.z),
            hal_vec3f_make(sb.x * vw + ox, sb.y * vh + oy, sb.z), base);
    }
}

/* ---- The axis gizmo (screen-space, bottom-left of the viewport) ---- */

static void render_gizmo(hal_canvas *canvas, hal_rectf viewport,
                         hal_mat4f view) {
    const hal_f32 r = 42.0f; /* gizmo radius, logical px */
    hal_pointf center = { viewport.x + r + 18.0f,
                          viewport.y + viewport.height - r - 18.0f };

    /* The view matrix's upper 3x3 rows are the world axes in view
     * space; project them onto the gizmo circle (orthographic feel). */
    hal_vec3f axes[3] = { hal_vec3f_axis_x(), hal_vec3f_axis_y(),
                          hal_vec3f_axis_z() };
    hal_color colors[3] = { hal_color_hex(0xe06c75u),
                            hal_color_hex(0x98c379u),
                            hal_color_hex(0x61afefu) };
    const char *names[3] = { "X", "Y", "Z" };
    (void)names; /* labels land with M4's gizmo interactions */

    for (int i = 0; i < 3; i++) {
        hal_vec3f a = hal_mat4f_transform_dir(view, axes[i]);
        hal_pointf tip = { center.x + a.x * r, center.y - a.y * r };
        hal_canvas_draw_line(canvas, center.x, center.y, tip.x, tip.y,
                             colors[i]);
        hal_canvas_fill_circle(canvas, tip.x, tip.y, 4.5f, colors[i]);
    }
}

/* ---- The view ---- */

void hal_render_view(hal_raster *raster, hal_scene *scene,
                     hal_camera *cam, hal_canvas *canvas,
                     hal_rectf viewport, const hal_render_opts *opts) {
    if (!raster || !scene || !cam || !canvas || !opts)
        return;
    if (hal_rect_empty((hal_rect){ (hal_i32)viewport.x, (hal_i32)viewport.y,
                                   (hal_i32)viewport.width,
                                   (hal_i32)viewport.height }))
        return;

    hal_f32 scale = canvas->scale;
    hal_f32 vw = viewport.width * scale;
    hal_f32 vh = viewport.height * scale;
    hal_f32 ox = viewport.x * scale;
    hal_f32 oy = viewport.y * scale;

    hal_mat4f view = hal_camera_view(cam);
    hal_mat4f proj = hal_camera_projection(cam, vw / vh);
    hal_mat4f view_proj = hal_mat4f_mul(proj, view);

    if (!hal_raster_begin(raster, canvas, viewport))
        return;
    hal_raster_clear(raster, opts->bg);

    if (opts->draw_grid)
        render_grid(raster, view_proj, canvas, viewport, opts->grid);

    hal_frustum frustum = hal_frustum_from_matrix(view_proj);

    for (size_t i = 0; i < scene->count; i++) {
        hal_object *obj = &scene->objects[i];
        if (!obj->visible)
            continue;

        hal_mat4f world = hal_object_world_matrix(scene, obj);
        hal_vec3f center =
            hal_mat4f_transform_point(world, hal_vec3f_zero());
        hal_f32 radius = hal_mesh_bounds_radius(obj->mesh);
        /* Scale the bounding radius by the largest world scale axis. */
        hal_f32 sx = hal_vec3f_length(hal_mat4f_transform_dir(
            world, hal_vec3f_axis_x()));
        hal_f32 sy = hal_vec3f_length(hal_mat4f_transform_dir(
            world, hal_vec3f_axis_y()));
        hal_f32 sz = hal_vec3f_length(hal_mat4f_transform_dir(
            world, hal_vec3f_axis_z()));
        hal_f32 s = sx > sy ? sx : sy;
        if (sz > s)
            s = sz;
        if (!hal_frustum_sphere_visible(&frustum, center, radius * s))
            continue;

        if (!obj->mesh)
            continue;

        if (opts->shading == HAL_SHADING_WIREFRAME) {
            render_mesh_wire(raster, obj->mesh, world, view_proj, ox, oy,
                             vw, vh, opts->object);
        } else {
            render_mesh_solid(raster, obj->mesh, world, view_proj,
                              hal_camera_eye(cam), obj->smooth_shading,
                              ox, oy, vw, vh, opts->object);
        }
    }

    /* The gizmo draws on the canvas (2D overlay, unclipped by the
     * raster). */
    if (opts->draw_gizmo)
        render_gizmo(canvas, viewport, view);

    hal_raster_end(raster);
}
