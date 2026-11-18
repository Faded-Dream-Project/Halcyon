/*
 * canvas.h — Halcyon's own 2D painter
 *
 * The drawing half of Halcyon's from-scratch UI (decision D-003): the
 * widget toolkit paints through this, not through any platform
 * primitive set. The canvas wraps a surface's raw XRGB8888 pixels and
 * provides rect fills, antialiased rounded rects, lines and circles,
 * and text (routed through the platform's exposed rasterizer).
 *
 * Coordinate spaces: a canvas has a SCALE. The UI creates its canvas
 * with the window's HiDPI scale and lays out in LOGICAL units — every
 * draw call multiplies by the scale and rounds outward, so the UI code
 * never sees the factor. The 3D viewport uses a scale-1.0 canvas over
 * physical pixels. Both are the same type.
 *
 * Clipping: a stack of logical-space rects, intersected on push; every
 * primitive clips against the effective rect. Text draws through the
 * platform with the clip converted to physical pixels.
 */

#ifndef HAL_CANVAS_H
#define HAL_CANVAS_H

#include "../platform/platform.h"
#include "../platform/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_CANVAS_CLIP_DEPTH 32

/* ---- STEP-2 audit hook ----
 * When set, EVERY draw_text is checked: the text's full ink rect
 * (pen_x .. pen_x+advance, baseline-ascent .. baseline+descent,
 * logical px, from the REAL font metrics) must lie inside the
 * current clip stack. Any clip fires the callback with both rects.
 * The paint walk is unchanged; this is a verification overlay. */
typedef void (*hal_canvas_text_audit_fn)(void *user,
                                         const char *utf8,
                                         hal_rectf text_rect,
                                         hal_rectf clip_rect,
                                         bool scroll_boundary);

typedef struct hal_canvas {
    /* STEP-2 audit (see hal_canvas_set_text_audit). */
    hal_canvas_text_audit_fn audit_fn;
    void *audit_user;
    /* Per-stack-entry: this clip is a SCROLLABLE region's boundary
     * (an overflowing column). The audit exempts ink cut at scroll
     * folds — that is correct scrolling, not clipped layout. */
    bool clip_scroll[HAL_CANVAS_CLIP_DEPTH];
    hal_u32 *pixels;      /* physical framebuffer                     */
    hal_i32 width;        /* physical pixels                          */
    hal_i32 height;
    hal_i32 stride;       /* hal_u32 units                            */
    hal_f32 scale;        /* logical -> physical                      */
    hal_surface *surface; /* for text (may be NULL: no text)          */

    /* Clip stack (logical space). clip_count == 0 means "everything". */
    hal_rectf clip[HAL_CANVAS_CLIP_DEPTH];
    int clip_count;
} hal_canvas;

/* Wraps a surface at `scale` (acquires the framebuffer itself). */
bool hal_canvas_init(hal_canvas *canvas, hal_surface *surface,
                     hal_f32 scale);

/* The canvas's logical size (physical / scale). */
hal_sizef hal_canvas_size(const hal_canvas *canvas);

/* Pushes a logical clip rect (intersected with the current). Depth is
 * bounded; overflow refuses (returns false) without touching the
 * stack. */
bool hal_canvas_push_clip(hal_canvas *canvas, hal_rectf rect);
void hal_canvas_pop_clip(hal_canvas *canvas);

/* The effective clip in logical space (infinite when none pushed). */
hal_rectf hal_canvas_clip(const hal_canvas *canvas);

/* ---- Primitives (logical coordinates, straight-alpha colors) ---- */

void hal_canvas_fill_rect(hal_canvas *canvas, hal_rectf rect,
                          hal_color color);

/* Antialiased rounded rect fill: per-pixel signed distance to the
 * rounded boundary, blended by coverage — full coverage inside, a
 * smooth fringe at the edge. radius clamps to half the shorter side. */
void hal_canvas_fill_rounded_rect(hal_canvas *canvas, hal_rectf rect,
                                  hal_f32 radius, hal_color color);

/* 1px outline of a rounded rect (AA). */
void hal_canvas_draw_rounded_rect(hal_canvas *canvas, hal_rectf rect,
                                  hal_f32 radius, hal_color color);

/* Crisp 1px rect outline (device-aligned, hairline). */
void hal_canvas_draw_rect(hal_canvas *canvas, hal_rectf rect,
                          hal_color color);

/* Antialiased line between two logical points (Xiaolin Wu). */
void hal_canvas_draw_line(hal_canvas *canvas, hal_f32 x0, hal_f32 y0,
                          hal_f32 x1, hal_f32 y1, hal_color color);

/* Antialiased filled circle. */
void hal_canvas_fill_circle(hal_canvas *canvas, hal_f32 cx, hal_f32 cy,
                            hal_f32 radius, hal_color color);

/* Solid triangle (viewport gizmos, curve editor fills). Winding
 * agnostic; degenerate triangles are no-ops. */
void hal_canvas_fill_triangle(hal_canvas *canvas, hal_pointf a,
                              hal_pointf b, hal_pointf c, hal_color color);

/* Text at (pen_x, baseline_y) in logical units. `font` must be loaded
 * at pixel_size * canvas->scale by the caller (the UI's font cache
 * does this). Clipped to the canvas's current clip. */
void hal_canvas_draw_text(hal_canvas *canvas, hal_font *font,
                          const char *utf8, size_t byte_len,
                          hal_f32 pen_x, hal_f32 baseline_y,
                          hal_color color);

/* Measures text with a font loaded for this canvas's scale and returns
 * the LOGICAL advance. */
hal_f32 hal_canvas_measure_text(hal_canvas *canvas, hal_font *font,
                                const char *utf8, size_t byte_len);

#ifdef __cplusplus
}
#endif

void hal_canvas_set_text_audit(hal_canvas *canvas,
                               hal_canvas_text_audit_fn fn,
                               void *user);
/* Like push_clip, but marks the region as a scrollable boundary. */
bool hal_canvas_push_clip_scroll(hal_canvas *canvas, hal_rectf rect);

#endif /* HAL_CANVAS_H */
