/*
 * autosave.h — Halcyon's autosave
 *
 * One job: keep a recent copy of the scene on disk without the user
 * thinking about it. The application marks the document dirty on every
 * edit and calls hal_autosave_tick from the frame loop; when the dirty
 * document has gone un-saved longer than the interval, the scene is
 * written (atomically, through the project save path) to the autosave
 * file.
 *
 * Crash recovery: hal_autosave_recovery_due compares the autosave
 * file's mtime against the project file's — when the autosave is
 * NEWER, the machine died between an autosave and a real save, and the
 * application should offer the autosave.
 *
 * The autosave file is a normal .halcyon file (format version 1) —
 * nothing special about it; rename it and it is a project.
 */

#ifndef HAL_AUTOSAVE_H
#define HAL_AUTOSAVE_H

#include "../platform/types.h"
#include "scene.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_autosave hal_autosave;

/* `autosave_path` is copied and owned by the handle; NULL = disabled
 * (tick becomes a no-op, useful for tests and --export runs).
 * `interval_seconds` is the minimum gap between automatic writes
 * (clamped to >= 1). */
hal_autosave *hal_autosave_create(const char *autosave_path,
                                  unsigned interval_seconds);
void hal_autosave_destroy(hal_autosave *autosave);

/* The document changed (call on every user edit). */
void hal_autosave_mark_dirty(hal_autosave *autosave);

/* The user saved the real project themselves (call after a successful
 * explicit save). */
void hal_autosave_mark_clean(hal_autosave *autosave);

bool hal_autosave_is_dirty(const hal_autosave *autosave);

/* Drives the schedule: saves `scene` to the autosave path when the
 * dirty document is due. `now_seconds` is a monotonic clock the caller
 * owns (the frame loop's). Returns true when a save attempt happened
 * (success or failure — failures stay dirty and retry next interval). */
bool hal_autosave_tick(hal_autosave *autosave, const hal_scene *scene,
                       double now_seconds);

/* True when the autosave file exists and is NEWER than `project_path`
 * (or the project does not exist at all) — the crash-recovery
 * condition. */
bool hal_autosave_recovery_due(const char *project_path,
                               const char *autosave_path);

#ifdef __cplusplus
}
#endif

#endif /* HAL_AUTOSAVE_H */
