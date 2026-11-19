/*
 * types.h — Halcyon base types
 *
 * The small value types every Halcyon module shares: results, geometry
 * and colors. Lives under src/platform/ because the platform layer is
 * the bottom of the stack (FDK's fdk_types.h plays the same role), but
 * nothing here is platform-specific — no FDK type is ever re-exported,
 * only mirrored where the semantics are trivial value types.
 *
 * Halcyon is free software under a permissive license — see LICENSE.
 */

#ifndef HAL_TYPES_H
#define HAL_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Fixed-width aliases used throughout Halcyon ---- */
typedef uint8_t   hal_u8;
typedef int8_t    hal_i8;
typedef uint16_t  hal_u16;
typedef int16_t   hal_i16;
typedef int32_t   hal_i32;
typedef uint32_t  hal_u32;
typedef int64_t   hal_i64;
typedef uint64_t  hal_u64;
typedef float     hal_f32;
typedef double    hal_f64;

/* ---- Results ----
 * Halcyon functions that can fail return a hal_result directly (no
 * errno, no exceptions). Output values travel through pointer params. */
typedef enum hal_result {
    HAL_OK = 0,

    /* Generic */
    HAL_ERR_UNKNOWN          = -1,
    HAL_ERR_INVALID_ARGUMENT = -2,
    HAL_ERR_OUT_OF_MEMORY    = -3,
    HAL_ERR_NOT_INITIALIZED  = -4,
    HAL_ERR_UNSUPPORTED      = -6, /* valid request, backend can't do it */
    HAL_ERR_NOT_FOUND        = -7,
    HAL_ERR_LIMIT            = -8, /* a bounded resource is full */
    HAL_ERR_INVALID_STATE    = -9, /* call valid, object mid-mutation */

    /* Platform / windowing */
    HAL_ERR_PLATFORM_INIT    = -100,
    HAL_ERR_NO_DISPLAY       = -101,
    HAL_ERR_WINDOW_CREATE    = -102,
    HAL_ERR_PLATFORM         = -103,

    /* Rendering */
    HAL_ERR_RENDER_INIT      = -200,
    HAL_ERR_SURFACE_CREATE   = -201,

    /* Text / fonts */
    HAL_ERR_FONT_LOAD        = -500,

    /* File I/O */
    HAL_ERR_IO               = -400,
} hal_result;

/* Convenience: nonzero (true) when `result` is a success code. */
static inline int hal_ok(hal_result result) {
    return result == HAL_OK;
}

/* ---- Geometry (integer, logical pixels unless noted) ---- */

typedef struct hal_point {
    hal_i32 x;
    hal_i32 y;
} hal_point;

typedef struct hal_size {
    hal_i32 width;
    hal_i32 height;
} hal_size;

typedef struct hal_rect {
    hal_i32 x;
    hal_i32 y;
    hal_i32 width;
    hal_i32 height;
} hal_rect;

/* Half-open: [x, x + width) x [y, y + height). An empty rect (width or
 * height <= 0) is the universal "nothing" — every consumer treats it
 * as such. */
static inline hal_rect hal_rect_make(hal_i32 x, hal_i32 y,
                                     hal_i32 w, hal_i32 h) {
    hal_rect r = { x, y, w, h };
    return r;
}

static inline bool hal_rect_empty(hal_rect r) {
    return r.width <= 0 || r.height <= 0;
}

/* Intersection of two rects; empty when they do not overlap. */
static inline hal_rect hal_rect_intersect(hal_rect a, hal_rect b) {
    hal_i32 x0 = a.x > b.x ? a.x : b.x;
    hal_i32 y0 = a.y > b.y ? a.y : b.y;
    hal_i32 x1 = (a.x + a.width)  < (b.x + b.width)  ? (a.x + a.width)  : (b.x + b.width);
    hal_i32 y1 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return hal_rect_make(0, 0, 0, 0);
    return hal_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static inline bool hal_rect_contains(hal_rect r, hal_i32 x, hal_i32 y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

/* ---- Floating-point geometry (sub-pixel UI, 3D screen space) ---- */

typedef struct hal_pointf {
    hal_f32 x;
    hal_f32 y;
} hal_pointf;

typedef struct hal_sizef {
    hal_f32 width;
    hal_f32 height;
} hal_sizef;

typedef struct hal_rectf {
    hal_f32 x;
    hal_f32 y;
    hal_f32 width;
    hal_f32 height;
} hal_rectf;

/* ---- Color ----
 * Straight (non-premultiplied) RGBA, components in [0.0, 1.0] — the
 * same convention as the platform surface, so painter code never
 * converts. Constructors below make theme files and code read alike. */
typedef struct hal_color {
    hal_f32 r;
    hal_f32 g;
    hal_f32 b;
    hal_f32 a;
} hal_color;

static inline hal_color hal_color_rgb(hal_f32 r, hal_f32 g, hal_f32 b) {
    hal_color c = { r, g, b, 1.0f };
    return c;
}

static inline hal_color hal_color_rgba(hal_f32 r, hal_f32 g, hal_f32 b,
                                       hal_f32 a) {
    hal_color c = { r, g, b, a };
    return c;
}

/* From 8-bit channels (theme files, hex constants). */
static inline hal_color hal_color_rgb8(hal_u8 r, hal_u8 g, hal_u8 b) {
    hal_color c = { (hal_f32)r / 255.0f, (hal_f32)g / 255.0f,
                    (hal_f32)b / 255.0f, 1.0f };
    return c;
}

static inline hal_color hal_color_rgba8(hal_u8 r, hal_u8 g, hal_u8 b,
                                        hal_u8 a) {
    hal_color c = { (hal_f32)r / 255.0f, (hal_f32)g / 255.0f,
                    (hal_f32)b / 255.0f, (hal_f32)a / 255.0f };
    return c;
}

/* 0xRRGGBB (opaque). The dreamy palette constants live in the theme,
 * not here; this is just packing. */
static inline hal_color hal_color_hex(hal_u32 rgb) {
    return hal_color_rgb8((hal_u8)((rgb >> 16) & 0xFFu),
                          (hal_u8)((rgb >> 8) & 0xFFu),
                          (hal_u8)(rgb & 0xFFu));
}

#ifdef __cplusplus
}
#endif

#endif /* HAL_TYPES_H */
