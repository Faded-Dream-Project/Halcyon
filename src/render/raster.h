/*
 * raster.h — Halcyon's CPU rasterizer (the render module's engine)
 *
 * A depth-buffered triangle and line rasterizer writing straight into
 * a canvas's pixel region. Everything here is PHYSICAL pixels over an
 * integer scissor rect; the scene-level renderer (render.h) does the
 * transform work and hands over screen-space geometry.
 *
 * Depth convention: NDC z in [0,1] (near=0, far=1), smaller wins.
 */

#ifndef HAL_RASTER_H
#define HAL_RASTER_H

#include "../core/math.h"
#include "../platform/types.h"
#include "../ui/canvas.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_raster {
    hal_u32 *color;   /* points into the canvas (top-left of region) */
    hal_f32 *depth;   /* owned, w*h                                */
    hal_i32 x, y;     /* region origin in canvas pixels            */
    hal_i32 w, h;
    hal_i32 stride;   /* the canvas's stride                       */
    bool depth_test;
} hal_raster;

/* Binds the raster to `rect` (LOGICAL viewport units) of the canvas,
 * allocating/ resizing the depth buffer when the region size changed.
 * The canvas's clip stack applies through the origin + region bounds.
 * Returns false when the rect is empty or allocation failed. */
bool hal_raster_begin(hal_raster *r, hal_canvas *canvas, hal_rectf rect);

/* Releases the depth buffer (the color memory is the canvas's). */
void hal_raster_end(hal_raster *r);

/* Clears color + depth. */
void hal_raster_clear(hal_raster *r, hal_color color);

/* A screen-space triangle. p* are physical-pixel coordinates in the
 * CANVAS space (not region-local); z* are NDC depths; i* are shading
 * intensities (0..1) interpolated perspective-correctly. Back faces
 * (clockwise in canvas space, y-down) are culled. */
void hal_raster_triangle(hal_raster *r, hal_vec3f a, hal_f32 ia,
                         hal_vec3f b, hal_f32 ib, hal_vec3f c, hal_f32 ic,
                         hal_color color);

/* A depth-tested line (Bresenham with interpolated z). */
void hal_raster_line(hal_raster *r, hal_vec3f a, hal_vec3f b,
                     hal_color color);

/* A soft 2D point (the gizmo's axis dots). */
void hal_raster_point(hal_raster *r, hal_f32 x, hal_f32 y, hal_f32 z,
                      hal_f32 radius, hal_color color);

#ifdef __cplusplus
}
#endif

#endif /* HAL_RASTER_H */
