/*
 * canvas.c — Halcyon's 2D painter implementation
 *
 * All primitives composite straight-alpha source-over into XRGB8888
 * pixels with one rounding per channel, clipped to the stack. Logical
 * coordinates scale by canvas->scale with outward rounding on fills
 * (a scaled rect never leaves gaps between neighbors).
 */

#include "canvas.h"

#include <math.h>
#include <string.h>

bool hal_canvas_init(hal_canvas *canvas, hal_surface *surface,
                     hal_f32 scale) {
    if (!canvas || !surface)
        return false;
    hal_surface_info info;
    if (!hal_ok(hal_surface_info_get(surface, &info)))
        return false;
    memset(canvas, 0, sizeof(*canvas));
    canvas->pixels = info.pixels;
    canvas->width = info.width;
    canvas->height = info.height;
    canvas->stride = info.stride;
    canvas->scale = scale > 0.0f ? scale : 1.0f;
    canvas->surface = surface;
    return true;
}

hal_sizef hal_canvas_size(const hal_canvas *canvas) {
    hal_sizef s = { (hal_f32)canvas->width / canvas->scale,
                    (hal_f32)canvas->height / canvas->scale };
    return s;
}

/* ---- Clip stack ---- */

static const hal_rectf HAL_RECTF_INF = { -1e9f, -1e9f, 2e9f, 2e9f };

bool hal_canvas_push_clip_scroll(hal_canvas *canvas, hal_rectf rect) {
    bool ok = hal_canvas_push_clip(canvas, rect);
    if (ok && canvas->clip_count > 0)
        canvas->clip_scroll[canvas->clip_count - 1] = true;
    return ok;
}

bool hal_canvas_push_clip(hal_canvas *canvas, hal_rectf rect) {
    if (canvas->clip_count >= HAL_CANVAS_CLIP_DEPTH)
        return false;
    canvas->clip_scroll[canvas->clip_count] = false;
    if (rect.width <= 0.0f || rect.height <= 0.0f)
        rect = (hal_rectf){ 0, 0, 0, 0 }; /* empty intersection */
    if (canvas->clip_count > 0) {
        hal_rectf cur = canvas->clip[canvas->clip_count - 1];
        hal_f32 x0 = cur.x > rect.x ? cur.x : rect.x;
        hal_f32 y0 = cur.y > rect.y ? cur.y : rect.y;
        hal_f32 x1 = (cur.x + cur.width) < (rect.x + rect.width)
                         ? (cur.x + cur.width)
                         : (rect.x + rect.width);
        hal_f32 y1 = (cur.y + cur.height) < (rect.y + rect.height)
                         ? (cur.y + cur.height)
                         : (rect.y + rect.height);
        rect = (hal_rectf){ x0, y0, x1 > x0 ? x1 - x0 : 0.0f,
                            y1 > y0 ? y1 - y0 : 0.0f };
    }
    canvas->clip[canvas->clip_count++] = rect;
    return true;
}

void hal_canvas_pop_clip(hal_canvas *canvas) {
    if (canvas->clip_count > 0)
        canvas->clip_count--;
}

hal_rectf hal_canvas_clip(const hal_canvas *canvas) {
    if (canvas->clip_count == 0)
        return HAL_RECTF_INF;
    return canvas->clip[canvas->clip_count - 1];
}

/* The physical clip rect for the current logical clip. With nothing
 * pushed this is the whole framebuffer (NOT derived from the infinite
 * logical rect, whose float->int conversion would overflow). */
static hal_rect clip_px(const hal_canvas *canvas) {
    if (canvas->clip_count == 0)
        return hal_rect_make(0, 0, canvas->width, canvas->height);

    hal_rectf c = canvas->clip[canvas->clip_count - 1];
    hal_f32 s = canvas->scale;
    hal_f32 fx0 = c.x * s;
    hal_f32 fy0 = c.y * s;
    hal_f32 fx1 = (c.x + c.width) * s;
    hal_f32 fy1 = (c.y + c.height) * s;
    /* Clamp in float space before the int conversion — a clip pushed
     * far off-canvas must never overflow the integer math. */
    if (fx0 < 0.0f) fx0 = 0.0f;
    if (fy0 < 0.0f) fy0 = 0.0f;
    if (fx1 > (hal_f32)canvas->width) fx1 = (hal_f32)canvas->width;
    if (fy1 > (hal_f32)canvas->height) fy1 = (hal_f32)canvas->height;
    hal_i32 x0 = (hal_i32)floorf(fx0);
    hal_i32 y0 = (hal_i32)floorf(fy0);
    hal_i32 x1 = (hal_i32)ceilf(fx1);
    hal_i32 y1 = (hal_i32)ceilf(fy1);
    if (x1 <= x0 || y1 <= y0)
        return hal_rect_make(0, 0, 0, 0);
    return hal_rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* ---- Pixel blending ---- */

/* Blends one physical pixel by `alpha` (0..255 effective source
 * coverage). Fast path for opaque. */
static inline void blend_pixel(hal_canvas *canvas, hal_i32 x, hal_i32 y,
                               hal_color color, hal_u32 alpha) {
    if (alpha == 0)
        return;
    hal_rect c = clip_px(canvas);
    if (x < c.x || y < c.y || x >= c.x + c.width || y >= c.y + c.height)
        return;

    hal_u32 *px = &canvas->pixels[(size_t)y * (size_t)canvas->stride +
                                  (size_t)x];
    if (alpha >= 255u && color.a >= 1.0f) {
        *px = 0xFF000000u |
              ((hal_u32)(color.r * 255.0f + 0.5f) << 16) |
              ((hal_u32)(color.g * 255.0f + 0.5f) << 8) |
              (hal_u32)(color.b * 255.0f + 0.5f);
        return;
    }

    /* Straight source-over with the color's alpha folded into the
     * coverage: one effective alpha, one blend. On ARGB surfaces the
     * alpha channel composites with the same coverage (the GPU
     * window's UI layer draws over the transparent viewport hole). */
    hal_f32 eff = (hal_f32)alpha / 255.0f * color.a;
    if (eff <= 0.0f)
        return;
    if (eff >= 1.0f) {
        *px = 0xFF000000u |
              ((hal_u32)(color.r * 255.0f + 0.5f) << 16) |
              ((hal_u32)(color.g * 255.0f + 0.5f) << 8) |
              (hal_u32)(color.b * 255.0f + 0.5f);
        return;
    }

    hal_u32 dst = *px;
    hal_f32 dr = (hal_f32)((dst >> 16) & 0xFFu);
    hal_f32 dg = (hal_f32)((dst >> 8) & 0xFFu);
    hal_f32 db = (hal_f32)(dst & 0xFFu);
    hal_f32 da = (hal_f32)((dst >> 24) & 0xFFu);
    hal_u32 out_a =
        (hal_u32)(255.0f * eff + da * (1.0f - eff) + 0.5f);
    *px = (out_a << 24) |
          ((hal_u32)(color.r * 255.0f * eff + dr * (1.0f - eff) + 0.5f)
           << 16) |
          ((hal_u32)(color.g * 255.0f * eff + dg * (1.0f - eff) + 0.5f)
           << 8) |
          (hal_u32)(color.b * 255.0f * eff + db * (1.0f - eff) + 0.5f);
}



/* ---- Primitives ---- */

void hal_canvas_fill_rect(hal_canvas *canvas, hal_rectf rect,
                          hal_color color) {
    hal_f32 s = canvas->scale;
    hal_i32 x0 = (hal_i32)floorf(rect.x * s);
    hal_i32 y0 = (hal_i32)floorf(rect.y * s);
    hal_i32 x1 = (hal_i32)ceilf((rect.x + rect.width) * s);
    hal_i32 y1 = (hal_i32)ceilf((rect.y + rect.height) * s);

    hal_rect clip = clip_px(canvas);
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;
    if (x1 <= x0 || y1 <= y0)
        return;

    hal_u32 packed = 0xFF000000u |
                     ((hal_u32)(color.r * 255.0f + 0.5f) << 16) |
                     ((hal_u32)(color.g * 255.0f + 0.5f) << 8) |
                     (hal_u32)(color.b * 255.0f + 0.5f);

    if (color.a >= 1.0f) {
        /* Opaque span fill — the hot path. */
        for (hal_i32 y = y0; y < y1; y++) {
            hal_u32 *row = &canvas->pixels[(size_t)y *
                                           (size_t)canvas->stride];
            for (hal_i32 x = x0; x < x1; x++)
                row[x] = packed;
        }
        return;
    }

    hal_u32 alpha = (hal_u32)(color.a * 255.0f + 0.5f);
    for (hal_i32 y = y0; y < y1; y++)
        for (hal_i32 x = x0; x < x1; x++)
            blend_pixel(canvas, x, y, color, alpha);
}

void hal_canvas_fill_rounded_rect(hal_canvas *canvas, hal_rectf rect,
                                  hal_f32 radius, hal_color color) {
    hal_f32 s = canvas->scale;
    if (radius * s <= 0.5f) {
        hal_canvas_fill_rect(canvas, rect, color);
        return;
    }

    hal_f32 half =
        (rect.width < rect.height ? rect.width : rect.height) * 0.5f;
    if (radius > half)
        radius = half;

    /* Physical-space SDF of a rounded box:
     *   q  = |p - c| - (half - r)          (component-wise)
     *   d  = length(max(q, 0)) + min(max(qx, qy), 0) - r
     * Coverage = clamp(0.5 - d, 0, 1): full inside, a 1px fringe out. */
    hal_f32 px0 = rect.x * s, py0 = rect.y * s;
    hal_f32 px1 = (rect.x + rect.width) * s;
    hal_f32 py1 = (rect.y + rect.height) * s;
    hal_f32 pr = radius * s;

    hal_f32 cx = (px0 + px1) * 0.5f, cy = (py0 + py1) * 0.5f;
    hal_f32 hx = (px1 - px0) * 0.5f, hy = (py1 - py0) * 0.5f;

    hal_rect clip = clip_px(canvas);
    hal_i32 x0 = (hal_i32)floorf(px0) - 1, x1 = (hal_i32)ceilf(px1) + 1;
    hal_i32 y0 = (hal_i32)floorf(py0) - 1, y1 = (hal_i32)ceilf(py1) + 1;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;

    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 qx = fabsf((hal_f32)x + 0.5f - cx) - (hx - pr);
            hal_f32 qy = fabsf((hal_f32)y + 0.5f - cy) - (hy - pr);
            hal_f32 mx = qx > 0.0f ? qx : 0.0f;
            hal_f32 my = qy > 0.0f ? qy : 0.0f;
            hal_f32 outside = sqrtf(mx * mx + my * my);
            hal_f32 inside = qx > qy ? qx : qy;
            hal_f32 dist = outside + (inside < 0.0f ? inside : 0.0f) - pr;
            hal_f32 cov = 0.5f - dist;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_pixel(canvas, x, y, color,
                        (hal_u32)(cov * 255.0f + 0.5f));
        }
    }
}

void hal_canvas_draw_rounded_rect(hal_canvas *canvas, hal_rectf rect,
                                  hal_f32 radius, hal_color color) {
    hal_f32 s = canvas->scale;
    hal_f32 half = (rect.width < rect.height ? rect.width : rect.height) * 0.5f;
    if (radius > half)
        radius = half;
    if (radius * s <= 0.5f) {
        hal_canvas_draw_rect(canvas, rect, color);
        return;
    }

    /* Same SDF as the fill; a 1px band centered on the boundary. */
    hal_f32 px0 = rect.x * s, py0 = rect.y * s;
    hal_f32 px1 = (rect.x + rect.width) * s;
    hal_f32 py1 = (rect.y + rect.height) * s;
    hal_f32 pr = radius * s;

    hal_f32 cx = (px0 + px1) * 0.5f, cy = (py0 + py1) * 0.5f;
    hal_f32 hx = (px1 - px0) * 0.5f, hy = (py1 - py0) * 0.5f;

    hal_rect clip = clip_px(canvas);
    hal_i32 x0 = (hal_i32)floorf(px0) - 2, x1 = (hal_i32)ceilf(px1) + 2;
    hal_i32 y0 = (hal_i32)floorf(py0) - 2, y1 = (hal_i32)ceilf(py1) + 2;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;

    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 qx = fabsf((hal_f32)x + 0.5f - cx) - (hx - pr);
            hal_f32 qy = fabsf((hal_f32)y + 0.5f - cy) - (hy - pr);
            hal_f32 mx = qx > 0.0f ? qx : 0.0f;
            hal_f32 my = qy > 0.0f ? qy : 0.0f;
            hal_f32 outside = sqrtf(mx * mx + my * my);
            hal_f32 inside = qx > qy ? qx : qy;
            hal_f32 dist = outside + (inside < 0.0f ? inside : 0.0f) - pr;
            hal_f32 cov = 1.0f - fabsf(dist);
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_pixel(canvas, x, y, color,
                        (hal_u32)(cov * 255.0f + 0.5f));
        }
    }
}

void hal_canvas_draw_rect(hal_canvas *canvas, hal_rectf rect,
                          hal_color color) {
    hal_f32 s = canvas->scale;
    hal_i32 x0 = (hal_i32)floorf(rect.x * s);
    hal_i32 y0 = (hal_i32)floorf(rect.y * s);
    hal_i32 x1 = (hal_i32)ceilf((rect.x + rect.width) * s) - 1;
    hal_i32 y1 = (hal_i32)ceilf((rect.y + rect.height) * s) - 1;
    if (x1 < x0 || y1 < y0)
        return;

    hal_u32 alpha = (hal_u32)(color.a * 255.0f + 0.5f);
    for (hal_i32 x = x0; x <= x1; x++) {
        blend_pixel(canvas, x, y0, color, alpha);
        blend_pixel(canvas, x, y1, color, alpha);
    }
    for (hal_i32 y = y0 + 1; y < y1; y++) {
        blend_pixel(canvas, x0, y, color, alpha);
        blend_pixel(canvas, x1, y, color, alpha);
    }
}

void hal_canvas_draw_line(hal_canvas *canvas, hal_f32 x0, hal_f32 y0,
                          hal_f32 x1, hal_f32 y1, hal_color color) {
    hal_f32 s = canvas->scale;
    x0 *= s;
    y0 *= s;
    x1 *= s;
    y1 *= s;

    hal_f32 dx = x1 - x0, dy = y1 - y0;
    if (fabsf(dx) < 0.01f && fabsf(dy) < 0.01f) {
        blend_pixel(canvas, (hal_i32)floorf(x0), (hal_i32)floorf(y0),
                    color, (hal_u32)(color.a * 255.0f + 0.5f));
        return;
    }

    hal_u32 base = (hal_u32)(color.a * 255.0f + 0.5f);

    if (fabsf(dx) >= fabsf(dy)) {
        /* Horizontal-major Wu: endpoints swapped to left->right. */
        if (x0 > x1) {
            hal_f32 t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            dx = -dx;
            dy = -dy;
        }
        hal_f32 gradient = dx > 0.01f ? dy / dx : 0.0f;
        hal_f32 y = y0;
        for (hal_i32 x = (hal_i32)floorf(x0); x <= (hal_i32)floorf(x1) &&
                                                x < (hal_i32)ceilf(x1 + 1.0f);
             x++) {
            if (x < (hal_i32)x0 - 1 || x > (hal_i32)x1 + 1) {
                y += gradient;
                continue;
            }
            hal_f32 fy = floorf(y);
            hal_f32 frac = y - fy;
            hal_u32 a1 = (hal_u32)((1.0f - frac) * (hal_f32)base + 0.5f);
            hal_u32 a2 = (hal_u32)(frac * (hal_f32)base + 0.5f);
            blend_pixel(canvas, x, (hal_i32)fy, color, a1);
            blend_pixel(canvas, x, (hal_i32)fy + 1, color, a2);
            y += gradient;
        }
    } else {
        if (y0 > y1) {
            hal_f32 t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            dx = -dx;
            dy = -dy;
        }
        hal_f32 gradient = dy > 0.01f ? dx / dy : 0.0f;
        hal_f32 x = x0;
        for (hal_i32 y = (hal_i32)floorf(y0); y <= (hal_i32)floorf(y1) &&
                                                y < (hal_i32)ceilf(y1 + 1.0f);
             y++) {
            if (y < (hal_i32)y0 - 1 || y > (hal_i32)y1 + 1) {
                x += gradient;
                continue;
            }
            hal_f32 fx = floorf(x);
            hal_f32 frac = x - fx;
            hal_u32 a1 = (hal_u32)((1.0f - frac) * (hal_f32)base + 0.5f);
            hal_u32 a2 = (hal_u32)(frac * (hal_f32)base + 0.5f);
            blend_pixel(canvas, (hal_i32)fx, y, color, a1);
            blend_pixel(canvas, (hal_i32)fx + 1, y, color, a2);
            x += gradient;
        }
    }
}

void hal_canvas_fill_circle(hal_canvas *canvas, hal_f32 cx, hal_f32 cy,
                            hal_f32 radius, hal_color color) {
    hal_f32 s = canvas->scale;
    cx *= s;
    cy *= s;
    hal_f32 r = radius * s;
    if (r <= 0.0f)
        return;

    hal_rect clip = clip_px(canvas);
    hal_i32 x0 = (hal_i32)floorf(cx - r) - 1, x1 = (hal_i32)ceilf(cx + r) + 1;
    hal_i32 y0 = (hal_i32)floorf(cy - r) - 1, y1 = (hal_i32)ceilf(cy + r) + 1;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;

    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 dx = (hal_f32)x + 0.5f - cx;
            hal_f32 dy = (hal_f32)y + 0.5f - cy;
            hal_f32 dist = sqrtf(dx * dx + dy * dy) - r;
            hal_f32 cov = 0.5f - dist;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_pixel(canvas, x, y, color,
                        (hal_u32)(cov * 255.0f + 0.5f));
        }
    }
}

void hal_canvas_fill_triangle(hal_canvas *canvas, hal_pointf a,
                              hal_pointf b, hal_pointf c, hal_color color) {
    hal_f32 s = canvas->scale;
    hal_f32 ax = a.x * s, ay = a.y * s;
    hal_f32 bx = b.x * s, by = b.y * s;
    hal_f32 cx = c.x * s, cy = c.y * s;

    /* Degenerate: zero area. */
    hal_f32 area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (area > -0.01f && area < 0.01f)
        return;

    hal_rect clip = clip_px(canvas);
    hal_f32 minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    hal_f32 maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    hal_f32 miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    hal_f32 maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);

    hal_i32 x0 = (hal_i32)floorf(minx) - 1, x1 = (hal_i32)ceilf(maxx) + 1;
    hal_i32 y0 = (hal_i32)floorf(miny) - 1, y1 = (hal_i32)ceilf(maxy) + 1;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;

    /* Barycentric coverage with a 1px AA edge (edge function falloff
     * along the dominant axis is close enough at UI scales). */
    hal_f32 e0 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    for (hal_i32 y = y0; y < y1; y++) {
        for (hal_i32 x = x0; x < x1; x++) {
            hal_f32 px = (hal_f32)x + 0.5f, py = (hal_f32)y + 0.5f;
            hal_f32 w0 = (bx - ax) * (py - ay) - (px - ax) * (by - ay);
            hal_f32 w1 = (cx - bx) * (py - by) - (px - bx) * (cy - by);
            hal_f32 w2 = (ax - cx) * (py - cy) - (px - cx) * (ay - cy);
            hal_f32 d0 = e0 >= 0.0f ? w0 : -w0;
            hal_f32 d1 = e0 >= 0.0f ? w1 : -w1;
            hal_f32 d2 = e0 >= 0.0f ? w2 : -w2;
            /* Inside when all edge functions share the winding sign;
             * coverage softens near zero (about one pixel). */
            hal_f32 edge_len0 = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) + 1.0f;
            hal_f32 cov = d0 / edge_len0;
            hal_f32 t;
            t = d1 / (sqrtf((cx - bx) * (cx - bx) + (cy - by) * (cy - by)) + 1.0f);
            if (t < cov) cov = t;
            t = d2 / (sqrtf((ax - cx) * (ax - cx) + (ay - cy) * (ay - cy)) + 1.0f);
            if (t < cov) cov = t;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_pixel(canvas, x, y, color,
                        (hal_u32)(cov * 255.0f + 0.5f));
        }
    }
}

void hal_canvas_set_text_audit(hal_canvas *canvas,
                               hal_canvas_text_audit_fn fn,
                               void *user) {
    if (!canvas)
        return;
    canvas->audit_fn = fn;
    canvas->audit_user = user;
}

void hal_canvas_draw_text(hal_canvas *canvas, hal_font *font,
                          const char *utf8, size_t byte_len,
                          hal_f32 pen_x, hal_f32 baseline_y,
                          hal_color color) {
    if (!canvas->surface || !font || !utf8 || byte_len == 0)
        return;

    /* The audit: the text's full ink rectangle must fit the clip.
     * Runs before the platform call so it also fires for text the
     * platform would cull entirely. */
    if (canvas->audit_fn) {
        hal_font_metrics fm;
        hal_font_get_metrics(font, &fm);
        hal_text_metrics tm;
        hal_f32 adv = 0.0f;
        if (hal_ok(hal_font_measure(font, utf8, byte_len, &tm)))
            adv = (hal_f32)tm.advance_width / canvas->scale;
        hal_f32 asc = (hal_f32)fm.ascent / canvas->scale;
        hal_f32 desc = (hal_f32)fm.descent / canvas->scale;
        hal_rectf ink = { pen_x, baseline_y - asc, adv, asc + desc };
        hal_rectf clip = canvas->clip_count > 0
                             ? canvas->clip[canvas->clip_count - 1]
                             : (hal_rectf){ 0, 0, 1e9f, 1e9f };
        /* Tolerance: sub-pixel rounding only. Any real clip of ink
         * is a violation. */
        const hal_f32 tol = 0.75f;
        if (ink.x < clip.x - tol || ink.y < clip.y - tol ||
            ink.x + ink.width > clip.x + clip.width + tol ||
            ink.y + ink.height > clip.y + clip.height + tol) {
            /* A cut at a scrollable region's fold is correct
             * scrolling, not clipped layout. */
            bool scroll = false;
            for (int i = 0; i < canvas->clip_count; i++)
                if (canvas->clip_scroll[i])
                    scroll = true;
            canvas->audit_fn(canvas->audit_user, utf8, ink, clip,
                             scroll);
        }
    }

    hal_f32 s = canvas->scale;
    hal_i32 px = (hal_i32)(pen_x * s + 0.5f);
    hal_i32 py = (hal_i32)(baseline_y * s + 0.5f);

    /* Clamp the text to the canvas's logical clip: the platform draws
     * in physical pixels, so convert. */
    hal_rect clip = clip_px(canvas);
    if (clip.width <= 0 || clip.height <= 0)
        return;

    hal_surface_draw_text(canvas->surface, font, utf8, byte_len, px, py,
                          color, &clip);
}

hal_f32 hal_canvas_measure_text(hal_canvas *canvas, hal_font *font,
                                const char *utf8, size_t byte_len) {
    hal_text_metrics m;
    /* hal_font_measure returns hal_result (HAL_OK == 0): the old
     * !result test inverted the check and returned 0 for every
     * SUCCESSFUL measure — shortcut columns, button centering and
     * entry carets all laid out against zero-width text. */
    if (!hal_ok(hal_font_measure(font, utf8, byte_len, &m)))
        return 0.0f;
    return (hal_f32)m.advance_width / canvas->scale;
}
