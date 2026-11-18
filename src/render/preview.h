/*
 * preview.h — lit preview renders: single PNG, turntable frames,
 * thumbnails
 *
 * Renders through the same CPU rasterizer the viewport uses (gizmo
 * off, grid optional) into an offscreen surface and encodes PNG via
 * the platform's exposed encoder.
 */

#ifndef HAL_PREVIEW_H
#define HAL_PREVIEW_H

#include "../core/scene.h"
#include "camera.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames the whole scene (bounding box of all visible objects) in a
 * copy of `cam`; returns it. An empty scene keeps the camera. */
hal_camera hal_camera_frame_scene(hal_scene *scene, hal_camera cam,
                                  hal_f32 margin);

/* Renders to a PNG at `path` (w x h). */
hal_result hal_preview_render(hal_scene *scene, hal_camera cam, int w,
                              int h, bool grid, const char *path,
                              char *err, size_t err_cap);

/* Turntable: `frames` PNGs orbiting 360 degrees around the camera
 * target, written as `<prefix>_000.png` .. `<prefix>_%03d.png`. */
hal_result hal_preview_turntable(hal_scene *scene, hal_camera cam,
                                 int w, int h, int frames,
                                 const char *prefix, char *err,
                                 size_t err_cap);

/* Thumbnail: a small framed render (square). */
hal_result hal_preview_thumbnail(hal_scene *scene, int size,
                                 const char *path, char *err,
                                 size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PREVIEW_H */
