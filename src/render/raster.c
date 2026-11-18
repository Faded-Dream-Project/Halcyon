/*
 * raster.c — the CPU rasterizer: triangle fill with perspective-
 * correct shading, depth-tested lines and points
 */

#include "raster.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static hal_u32 color_pack(hal_color c) {
    return ((hal_u32)(c.r * 255.0f + 0.5f) << 16) |
           ((hal_u32)(c.g * 255.0f + 0.5f) << 8) |
           (hal_u32)(c.b * 255.0f + 0.5f);
}

/* Unpacks a 0x00RRGGBB pixel to a color (the cache's textures are
 * opaque; alpha stays 1). */
static hal_color color_unpack(hal_u32 p) {
    return hal_color_rgb((hal_f32)((p >> 16) & 0xFFu) / 255.0f,
                         (hal_f32)((p >> 8) & 0xFFu) / 255.0f,
                         (hal_f32)(p & 0xFFu) / 255.0f);
}

/* Multiplies a color by an intensity (lambert shading), clamped. */
static hal_color color_shade(hal_color c, hal_f32 intensity) {
    if (intensity < 0.0f)
        intensity = 0.0f;
    if (intensity > 1.0f)
        intensity = 1.0f;
    return hal_color_rgb(c.r * intensity, c.g * intensity, c.b * intensity);
}

bool hal_raster_begin(hal_raster *r, hal_canvas *canvas, hal_rectf rect) {
    if (!r || !canvas)
        return false;

    hal_f32 s = canvas->scale;
    hal_i32 x0 = (hal_i32)floorf(rect.x * s);
    hal_i32 y0 = (hal_i32)floorf(rect.y * s);
    hal_i32 x1 = (hal_i32)ceilf((rect.x + rect.width) * s);
    hal_i32 y1 = (hal_i32)ceilf((rect.y + rect.height) * s);

    /* Intersect with the canvas's physical bounds AND its clip stack
     * (the canvas API clips in logical space; mirror it here in
     * physical space so 3D pixels obey the same discipline). */
    hal_i32 cx0 = 0, cy0 = 0, cx1 = canvas->width, cy1 = canvas->height;
    if (canvas->clip_count > 0) {
        hal_rectf c = canvas->clip[canvas->clip_count - 1];
        cx0 = (hal_i32)floorf(c.x * s);
        cy0 = (hal_i32)floorf(c.y * s);
        cx1 = (hal_i32)ceilf((c.x + c.width) * s);
        cy1 = (hal_i32)ceilf((c.y + c.height) * s);
    }
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;

    if (x1 <= x0 || y1 <= y0)
        return false;

    hal_i32 w = x1 - x0;
    hal_i32 h = y1 - y0;
    if (w != r->w || h != r->h || !r->depth) {
        hal_f32 *depth = realloc(r->depth, (size_t)w * (size_t)h *
                                               sizeof(hal_f32));
        if (!depth)
            return false;
        r->depth = depth;
        r->w = w;
        r->h = h;
    }

    r->color = canvas->pixels + (size_t)y0 * (size_t)canvas->stride +
               (size_t)x0;
    r->x = x0;
    r->y = y0;
    r->stride = canvas->stride;
    r->depth_test = true;
    return true;
}

void hal_raster_end(hal_raster *r) {
    if (!r)
        return;
    free(r->depth);
    r->depth = NULL;
    r->w = 0;
    r->h = 0;
}

void hal_raster_clear(hal_raster *r, hal_color color) {
    if (!r)
        return;
    hal_u32 packed = color_pack(color);
    for (hal_i32 y = 0; y < r->h; y++) {
        hal_u32 *row = r->color + (size_t)y * (size_t)r->stride;
        for (hal_i32 x = 0; x < r->w; x++)
            row[x] = packed;
    }
    size_t n = (size_t)r->w * (size_t)r->h;
    for (size_t i = 0; i < n; i++)
        r->depth[i] = FLT_MAX;
}

/* Region-local pixel write with depth test. */
static inline void px(hal_raster *r, hal_i32 lx, hal_i32 ly, hal_f32 z,
                      hal_u32 packed) {
    if (lx < 0 || ly < 0 || lx >= r->w || ly >= r->h)
        return;
    if (z < 0.0f || z > 1.0f)
        return; /* outside the depth range (far-clip safety) */
    size_t idx = (size_t)ly * (size_t)r->w + (size_t)lx;
    if (r->depth_test && z >= r->depth[idx])
        return;
    r->depth[idx] = z;
    r->color[(size_t)ly * (size_t)r->stride + (size_t)lx] = packed;
}

void hal_raster_triangle(hal_raster *r, hal_vec3f a, hal_f32 ia,
                         hal_vec3f b, hal_f32 ib, hal_vec3f c, hal_f32 ic,
                         hal_color color) {
    if (!r)
        return;

    /* Region-local coordinates. */
    hal_f32 ax = a.x - (hal_f32)r->x, ay = a.y - (hal_f32)r->y;
    hal_f32 bx = b.x - (hal_f32)r->x, by = b.y - (hal_f32)r->y;
    hal_f32 cx = c.x - (hal_f32)r->x, cy = c.y - (hal_f32)r->y;

    /* Back-face cull: in canvas space (y down), front faces (CCW in
     * world, right-handed) arrive clockwise; negative area = back. */
    hal_f32 area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (area > -1e-6f && area < 1e-6f)
        return; /* degenerate */
    if (area > 0.0f)
        return; /* back-facing */

    /* FLAT fast path (the common viewport case: one lambert value for
     * the whole face): the packed color is computed once and the
     * inner loop reduces to edge stepping + z + write. */
    const bool flat = ia == ib && ib == ic;

    hal_f32 iaw = ia, ibw = ib, icw = ic;

    hal_f32 minx = fminf(ax, fminf(bx, cx));
    hal_f32 maxx = fmaxf(ax, fmaxf(bx, cx));
    hal_f32 miny = fminf(ay, fminf(by, cy));
    hal_f32 maxy = fmaxf(ay, fmaxf(by, cy));
    hal_i32 y0 = (hal_i32)floorf(miny);
    hal_i32 y1 = (hal_i32)ceilf(maxy);
    hal_i32 x0 = (hal_i32)floorf(minx);
    hal_i32 x1 = (hal_i32)ceilf(maxx);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > r->h) y1 = r->h;
    if (x1 > r->w) x1 = r->w;

    hal_f32 inv_area = 1.0f / area;

    if (flat) {
        /* Incremental edge stepping: the edge functions are affine in
         * (x, y), so each pixel costs three adds per function. */
        const hal_u32 packed =
            color_pack(color_shade(color, ia));
        const hal_f32 px0 = (hal_f32)x0 + 0.5f;
        const hal_f32 py0 = (hal_f32)y0 + 0.5f;
        hal_f32 w0r = (bx - ax) * (py0 - ay) - (px0 - ax) * (by - ay);
        hal_f32 w1r = (cx - bx) * (py0 - by) - (px0 - bx) * (cy - by);
        hal_f32 w2r = (ax - cx) * (py0 - cy) - (px0 - cx) * (ay - cy);
        const hal_f32 w0dx = ay - by; /* dE_ab/dx */
        const hal_f32 w1dx = by - cy; /* dE_bc/dx */
        const hal_f32 w2dx = cy - ay; /* dE_ca/dx */
        const hal_f32 w0dy = bx - ax; /* dE_ab/dy */
        const hal_f32 w1dy = cx - bx;
        const hal_f32 w2dy = ax - cx;
        for (hal_i32 y = y0; y < y1; y++) {
            hal_f32 w0 = w0r, w1 = w1r, w2 = w2r;
            for (hal_i32 x = x0; x < x1; x++) {
                if (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f) {
                    hal_f32 b_a = w1 * inv_area,
                            b_b = w2 * inv_area,
                            b_c = w0 * inv_area;
                    hal_f32 z =
                        b_a * a.z + b_b * b.z + b_c * c.z;
                    px(r, x, y, z, packed);
                }
                w0 += w0dx;
                w1 += w1dx;
                w2 += w2dx;
            }
            w0r += w0dy;
            w1r += w1dy;
            w2r += w2dy;
        }
        return;
    }

    /* Smooth path (affine intensity — see the header note). */
    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 px_ = (hal_f32)x + 0.5f;
            hal_f32 py_ = (hal_f32)y + 0.5f;
            hal_f32 w0 = (bx - ax) * (py_ - ay) - (px_ - ax) * (by - ay);
            hal_f32 w1 = (cx - bx) * (py_ - by) - (px_ - bx) * (cy - by);
            hal_f32 w2 = (ax - cx) * (py_ - cy) - (px_ - cx) * (ay - cy);
            if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
                continue;
            /* Edge-function to barycentric binding: E_ab (w0) is the
             * weight of vertex C, E_bc (w1) of A, E_ca (w2) of B. */
            hal_f32 b_a = w1 * inv_area, b_b = w2 * inv_area,
                    b_c = w0 * inv_area;
            hal_f32 z = b_a * a.z + b_b * b.z + b_c * c.z;
            hal_f32 intensity =
                b_a * iaw + b_b * ibw + b_c * icw;
            px(r, x, y, z, color_pack(color_shade(color, intensity)));
        }
    }
}

void hal_raster_line(hal_raster *r, hal_vec3f a, hal_vec3f b,
                     hal_color color) {
    if (!r)
        return;

    hal_f32 ax = a.x - (hal_f32)r->x, ay = a.y - (hal_f32)r->y;
    hal_f32 bx = b.x - (hal_f32)r->x, by = b.y - (hal_f32)r->y;

    hal_u32 packed = color_pack(color);
    hal_i32 x0 = (hal_i32)floorf(ax), y0 = (hal_i32)floorf(ay);
    hal_i32 x1 = (hal_i32)floorf(bx), y1 = (hal_i32)floorf(by);
    hal_i32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
    hal_i32 dy = y1 > y0 ? y1 - y0 : y0 - y1;
    hal_i32 sx = x0 < x1 ? 1 : -1;
    hal_i32 sy = y0 < y1 ? 1 : -1;
    hal_i32 err = dx - dy;
    hal_i32 steps = dx > dy ? dx : dy;
    hal_f32 z = a.z;
    hal_f32 dz = steps > 0 ? (b.z - a.z) / (hal_f32)steps : 0.0f;

    for (hal_i32 i = 0; i <= steps; i++) {
        px(r, x0, y0, z, packed);
        hal_i32 e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
        z += dz;
    }
}

void hal_raster_point(hal_raster *r, hal_f32 x, hal_f32 y, hal_f32 z,
                      hal_f32 radius, hal_color color) {
    if (!r)
        return;
    hal_u32 packed = color_pack(color);
    hal_f32 lx = x - (hal_f32)r->x, ly = y - (hal_f32)r->y;
    hal_i32 x0 = (hal_i32)floorf(lx - radius);
    hal_i32 x1 = (hal_i32)ceilf(lx + radius);
    hal_i32 y0 = (hal_i32)floorf(ly - radius);
    hal_i32 y1 = (hal_i32)ceilf(ly + radius);
    for (hal_i32 yy = y0; yy <= y1; yy++)
        for (hal_i32 xx = x0; xx <= x1; xx++) {
            hal_f32 ddx = (hal_f32)xx - lx;
            hal_f32 ddy = (hal_f32)yy - ly;
            if (ddx * ddx + ddy * ddy <= radius * radius)
                px(r, xx, yy, z, packed);
        }
}

static hal_f32 wrap_coord(hal_f32 t) {
    t = (hal_f32)fmod((double)t, 1.0);
    if (t < 0.0f)
        t += 1.0f;
    return t;
}

hal_color hal_texture_sample(const hal_texture *tex, hal_f32 u,
                             hal_f32 v, hal_tex_filter filter) {
    if (!tex || !tex->pixels || tex->w <= 0 || tex->h <= 0)
        return hal_color_rgb(1.0f, 1.0f, 1.0f);
    u = wrap_coord(u);
    v = wrap_coord(v);

    if (filter == HAL_TEX_NEAREST) {
        hal_i32 x = (hal_i32)(u * (hal_f32)tex->w);
        hal_i32 y = (hal_i32)(v * (hal_f32)tex->h);
        if (x >= tex->w)
            x = tex->w - 1;
        if (y >= tex->h)
            y = tex->h - 1;
        return color_unpack(tex->pixels[(size_t)y * (size_t)tex->w +
                                        (size_t)x]);
    }

    /* Bilinear: sample centers are at (i + 0.5) / n. */
    hal_f32 fx = u * (hal_f32)tex->w - 0.5f;
    hal_f32 fy = v * (hal_f32)tex->h - 0.5f;
    hal_i32 x0 = (hal_i32)floorf(fx);
    hal_i32 y0 = (hal_i32)floorf(fy);
    hal_f32 tx = fx - (hal_f32)x0;
    hal_f32 ty = fy - (hal_f32)y0;
    hal_i32 x1 = x0 + 1, y1 = y0 + 1;
    /* Repeat-wrap the neighbor indices. */
    hal_i32 wrapx0 = x0 % tex->w; if (wrapx0 < 0) wrapx0 += tex->w;
    hal_i32 wrapx1 = x1 % tex->w; if (wrapx1 < 0) wrapx1 += tex->w;
    hal_i32 wrapy0 = y0 % tex->h; if (wrapy0 < 0) wrapy0 += tex->h;
    hal_i32 wrapy1 = y1 % tex->h; if (wrapy1 < 0) wrapy1 += tex->h;
    hal_color c00 = color_unpack(
        tex->pixels[(size_t)wrapy0 * (size_t)tex->w + (size_t)wrapx0]);
    hal_color c10 = color_unpack(
        tex->pixels[(size_t)wrapy0 * (size_t)tex->w + (size_t)wrapx1]);
    hal_color c01 = color_unpack(
        tex->pixels[(size_t)wrapy1 * (size_t)tex->w + (size_t)wrapx0]);
    hal_color c11 = color_unpack(
        tex->pixels[(size_t)wrapy1 * (size_t)tex->w + (size_t)wrapx1]);
    hal_color c0 = hal_color_rgb(
        c00.r + (c10.r - c00.r) * tx, c00.g + (c10.g - c00.g) * tx,
        c00.b + (c10.b - c00.b) * tx);
    hal_color c1 = hal_color_rgb(
        c01.r + (c11.r - c01.r) * tx, c01.g + (c11.g - c01.g) * tx,
        c01.b + (c11.b - c01.b) * tx);
    return hal_color_rgb(c0.r + (c1.r - c0.r) * ty,
                         c0.g + (c1.g - c0.g) * ty,
                         c0.b + (c1.b - c0.b) * ty);
}

void hal_raster_triangle_uv(hal_raster *r, hal_vec3f a, hal_f32 ia,
                            hal_f32 wa, hal_vec2f uva, hal_vec3f b,
                            hal_f32 ib, hal_f32 wb, hal_vec2f uvb,
                            hal_vec3f c, hal_f32 ic, hal_f32 wc,
                            hal_vec2f uvc, hal_color color,
                            const hal_texture *tex,
                            hal_tex_filter filter) {
    if (!r || !tex)
        return;
    if (wa <= 1e-6f || wb <= 1e-6f || wc <= 1e-6f)
        return; /* behind the near plane (clip should have caught) */

    hal_f32 ax = a.x - (hal_f32)r->x, ay = a.y - (hal_f32)r->y;
    hal_f32 bx = b.x - (hal_f32)r->x, by = b.y - (hal_f32)r->y;
    hal_f32 cx = c.x - (hal_f32)r->x, cy = c.y - (hal_f32)r->y;

    hal_f32 area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (area > -1e-6f && area < 1e-6f)
        return;
    if (area > 0.0f)
        return; /* back-facing */

    /* Perspective-correct attributes: interpolate x/w and 1/w. */
    hal_f32 ia_w = ia / wa, ib_w = ib / wb, ic_w = ic / wc;
    hal_f32 ua_w = uva.x / wa, ub_w = uvb.x / wb, uc_w = uvc.x / wc;
    hal_f32 va_w = uva.y / wa, vb_w = uvb.y / wb, vc_w = uvc.y / wc;
    hal_f32 one_a = 1.0f / wa, one_b = 1.0f / wb, one_c = 1.0f / wc;

    hal_f32 minx = fminf(ax, fminf(bx, cx));
    hal_f32 maxx = fmaxf(ax, fmaxf(bx, cx));
    hal_f32 miny = fminf(ay, fminf(by, cy));
    hal_f32 maxy = fmaxf(ay, fmaxf(by, cy));
    hal_i32 y0 = (hal_i32)floorf(miny);
    hal_i32 y1 = (hal_i32)ceilf(maxy);
    hal_i32 x0 = (hal_i32)floorf(minx);
    hal_i32 x1 = (hal_i32)ceilf(maxx);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > r->h) y1 = r->h;
    if (x1 > r->w) x1 = r->w;

    hal_f32 inv_area = 1.0f / area;

    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 px_ = (hal_f32)x + 0.5f;
            hal_f32 py_ = (hal_f32)y + 0.5f;
            hal_f32 w0 = (bx - ax) * (py_ - ay) - (px_ - ax) * (by - ay);
            hal_f32 w1 = (cx - bx) * (py_ - by) - (px_ - bx) * (cy - by);
            hal_f32 w2 = (ax - cx) * (py_ - cy) - (px_ - cx) * (ay - cy);
            if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
                continue;
            hal_f32 b_a = w1 * inv_area, b_b = w2 * inv_area,
                    b_c = w0 * inv_area;
            hal_f32 z = b_a * a.z + b_b * b.z + b_c * c.z;
            hal_f32 one_w =
                b_a * one_a + b_b * one_b + b_c * one_c;
            if (one_w <= 1e-9f)
                continue;
            hal_f32 inv_w = 1.0f / one_w;
            hal_f32 intensity =
                (b_a * ia_w + b_b * ib_w + b_c * ic_w) * inv_w;
            hal_f32 u =
                (b_a * ua_w + b_b * ub_w + b_c * uc_w) * inv_w;
            hal_f32 v =
                (b_a * va_w + b_b * vb_w + b_c * vc_w) * inv_w;
            hal_color texel =
                hal_texture_sample(tex, u, v, filter);
            hal_color shaded = hal_color_rgb(
                color.r * texel.r, color.g * texel.g,
                color.b * texel.b);
            px(r, x, y, z, color_pack(color_shade(shaded, intensity)));
        }
    }
}
