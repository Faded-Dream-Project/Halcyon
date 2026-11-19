/* gl_renderer.h — Halcyon's OpenGL 3.3 core viewport renderer.
 *
 * Runs INSIDE the GPU frame callback (the platform's
 * hal_gpu_frame_fn): the context is current, the code owns the GL,
 * FDK swaps afterwards. Mirrors the CPU rasterizer's feature set
 * (hal_render_opts) so the readback tests can diff the two.
 *
 * The CPU rasterizer stays as the reference oracle for tests; the
 * runtime path is this renderer only.
 */
#ifndef HAL_GL_RENDERER_H
#define HAL_GL_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include "../platform/types.h"
#include "../core/scene.h"
#include "camera.h"
#include "render.h"

typedef struct hal_gl_renderer hal_gl_renderer;

hal_gl_renderer *hal_gl_renderer_create(void);
void hal_gl_renderer_destroy(hal_gl_renderer *r);

/* The per-frame state the renderer reports back (the shell's
 * frame-time overlay + --gpu-info). */
typedef struct hal_gl_stats {
    int samples;         /* MSAA samples in use (0 = off)  */
    hal_u32 gpu_ns;      /* last completed GPU timer query */
    bool gpu_timer_ok;   /* timer queries available        */
    size_t meshes;       /* cached mesh uploads            */
    hal_u32 tris_drawn;  /* triangles in the last frame    */
} hal_gl_stats;

const hal_gl_stats *hal_gl_renderer_stats(const hal_gl_renderer *r);

/* True when the GL programs failed to build (report + exit). */
bool hal_gl_renderer_failed(const hal_gl_renderer *r);

/* Render `scene` from `cam` into the logical `viewport` rect of the
 * CURRENT framebuffer (physical = viewport * scale); the viewport
 * rect is cleared with opts->bg, the rest left untouched. GL must be
 * current (the frame callback). `scene_serial` changes on any edit;
 * changed meshes re-upload, untouched ones ride the GPU buffers.
 * `overlay_*` carry the shell's projected overlays (armature sticks,
 * edit selection, gizmo, axis widget). */
typedef struct hal_gl_seg2 {
    hal_f32 x0, y0, x1, y1;
    hal_color color;
} hal_gl_seg2;

typedef struct hal_gl_pt2 {
    hal_f32 x, y;
    hal_color color;
    hal_f32 size;
} hal_gl_pt2;

void hal_gl_render_view(hal_gl_renderer *r, hal_scene *scene,
                        hal_camera *cam, hal_rectf viewport,
                        hal_f32 scale, int fb_width, int fb_height,
                        const hal_render_opts *opts,
                        hal_u64 scene_serial,
                        const hal_gl_seg2 *segs, size_t seg_count,
                        const hal_gl_pt2 *pts, size_t pt_count);

/* Composite the UI layer — straight-alpha ARGB pixels (A<<24 | R<<16
 * | G<<8 | B), stride in fdk_u32 units — over the WHOLE current
 * framebuffer, as the LAST pass (menus and popups over the viewport
 * by construction). `content_dirty` gates the texture re-upload;
 * the draw itself happens every call (the 3D under it changed). */
void hal_gl_composite_ui(hal_gl_renderer *r, const hal_u32 *pixels,
                         int width, int height, int stride,
                         bool content_dirty);

/* Read back the current framebuffer's pixel at (x, y) — the
 * readback tests' ground truth probe (RGBA8 bytes out). */
bool hal_gl_read_pixel(int x, int y, hal_u8 out_rgba[4]);

/* Drop cached GPU buffers for meshes no longer in the scene (call
 * once per frame; cheap at object-count sizes). */
void hal_gl_renderer_gc(hal_gl_renderer *r, hal_scene *scene);

/* For tests: force-drop every cached mesh (fresh uploads). */
void hal_gl_renderer_drop_all(hal_gl_renderer *r);

#endif /* HAL_GL_RENDERER_H */
