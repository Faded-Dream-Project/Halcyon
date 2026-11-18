/*
 * render.h — the 3D viewport renderer
 *
 * Renders the scene through the CPU rasterizer into a canvas region:
 * the horizon grid, every visible object (solid shading or
 * wireframe), and the axis gizmo. Objects cull against the view
 * frustum (bounding sphere) before their triangles are clipped near/
 * far in homogeneous space and rasterized depth-tested.
 */

#ifndef HAL_RENDER_H
#define HAL_RENDER_H

#include "../core/scene.h"
#include "../ui/canvas.h"
#include "camera.h"
#include "raster.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hal_shading {
    HAL_SHADING_SOLID = 0,
    HAL_SHADING_WIREFRAME = 1,
} hal_shading;

/* Viewport render options. */
typedef struct hal_render_opts {
    hal_shading shading;
    hal_color bg;         /* theme viewport_bg        */
    hal_color grid;       /* theme viewport_grid      */
    hal_color object;     /* default object color     */
    bool draw_grid;
    bool draw_gizmo;
} hal_render_opts;

hal_render_opts hal_render_opts_default(void);

/* Renders `scene` from `cam` into the logical `viewport` rect of the
 * canvas. `raster` carries the depth buffer across frames (the
 * caller owns it; hal_raster_begin/end handle the details). */
void hal_render_view(hal_raster *raster, hal_scene *scene,
                     hal_camera *cam, hal_canvas *canvas,
                     hal_rectf viewport, const hal_render_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* HAL_RENDER_H */
