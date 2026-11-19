/*
 * shell.h — the Halcyon editor shell
 *
 * The application object: one window, the M2 UI laid out (menu bar,
 * tool rail, outliner + properties sidebar, viewport placeholder,
 * status bar), the M1 core wired in (scene, undo, project, autosave),
 * and the frame driver. The viewport area is a reserved rect today;
 * M3 renders into it.
 */

#ifndef HAL_SHELL_H
#define HAL_SHELL_H

#include "core/autosave.h"
#include "core/scene.h"
#include "core/undo.h"
#include "platform/platform.h"
#include "render/render.h"
#include "ui/ui.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_app hal_app;

hal_app *hal_app_create(hal_platform *platform, hal_window *window);

/* Bump the GPU scene serial: live scene mutations (vertex drags,
 * transforms, playback) re-upload mesh buffers on the next frame. */
void hal_app_bump_gpu_serial(hal_app *app);

/* The CPU raster viewport — the reference oracle for the GL
 * renderer's readback tests. Never called at runtime. */
void hal_app_paint_viewport_cpu(hal_app *app, hal_canvas *canvas);
void hal_app_destroy(hal_app *app);

/* True when the user asked to quit (menu, shortcut, window close). */
bool hal_app_running(const hal_app *app);
void hal_app_quit(hal_app *app);

/* Feeds one platform event: the UI first, then app-level handling
 * (shortcuts, the viewport area). Returns true when consumed. */
bool hal_app_handle_event(hal_app *app, const hal_event *event);

/* One frame: resize handling, autosave, and the full repaint onto
 * `surface`. Call from the frame loop when the UI is dirty or the
 * viewport animates. */
void hal_app_paint(hal_app *app, hal_surface *surface);

/* STEP-2 audit: paints with the text/popup audit overlay (every
 * draw_text ink rect vs the clip stack, every popup vs its widest
 * item); returns the violation count, also printed to stderr.
 * forced_scale > 0 overrides the window scale (HiDPI testing). */
int hal_app_run_ui_audit(hal_app *app, hal_surface *surface,
                         hal_f32 forced_scale);

/* STEP-1a regression: opens the File menu over the viewport and
 * checks pixel ownership + input priority. 0 = pass. */
int hal_app_run_zorder_test(hal_app *app, hal_surface *surface);

/* STEP-1b regression: property entries show their text. 0 = pass. */
int hal_app_run_entries_test(hal_app *app, hal_surface *surface);

/* Advances clip playback (call every loop iteration with a monotonic
 * clock); marks the UI dirty while playing. */
void hal_app_tick(hal_app *app, double now_seconds);

/* The viewport's rect in window-logical pixels (where the 3D view
 * renders). */
hal_rectf hal_app_viewport_rect(const hal_app *app);

/* The viewport camera (borrow; mutate through the app's controls). */
hal_camera *hal_app_camera(hal_app *app);
const hal_render_opts *hal_app_render_opts(const hal_app *app);

/* The scene, UI, undo stack and autosave (owned by the app; borrow,
 * do not destroy). */
hal_scene *hal_app_scene(hal_app *app);
hal_ui *hal_app_ui(hal_app *app);
hal_undo_stack *hal_app_undo(hal_app *app);
hal_autosave *hal_app_autosave(hal_app *app);

/* Reload the outliner from the scene (call after scene mutations
 * from outside the UI). */
void hal_app_refresh_outliner(hal_app *app);

/* The current project path ("" until saved). */
const char *hal_app_project_path(const hal_app *app);
bool hal_app_dirty(const hal_app *app);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SHELL_H */
