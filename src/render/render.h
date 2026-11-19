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
/* A loaded texture image cache: paths -> decoded 0x00RRGGBB rows.
 * OWNED by the application (loads happen on first use; entries stay
 * for the cache's lifetime). */
typedef struct hal_texture_cache hal_texture_cache;

hal_texture_cache *hal_texture_cache_create(void);
void hal_texture_cache_destroy(hal_texture_cache *cache);
/* Returns NULL when the path is NULL/empty or the image cannot be
 * loaded (the renderer falls back to the plain untextured path). */
const hal_texture *hal_texture_cache_get(hal_texture_cache *cache,
                                         const char *path);

typedef struct hal_render_opts {
    hal_shading shading;
    hal_color bg;         /* theme viewport_bg        */
    hal_color grid;       /* theme viewport_grid      */
    hal_color object;     /* default object color     */
    bool draw_grid;
    bool draw_gizmo;
    hal_texture_cache *textures; /* materials sample through this */
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
