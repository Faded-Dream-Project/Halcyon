/*
 * project.h — the .halcyon project file
 *
 * A strict, line-oriented, human-readable text format. The grammar is
 * documented for users in docs/project-format.md; this header is the
 * API. Parsing is fail-closed (FDK's theme-parser posture): an unknown
 * key, a malformed number, a broken object block or a bad reference is
 * a loud error with a line number, never a half-loaded scene.
 *
 * Saving is atomic: the scene is written to a sibling temp file and
 * renamed over the destination, so a crash mid-save can never destroy
 * an existing project.
 *
 * Floats round-trip exactly: values are written with 9 significant
 * digits, which is lossless for binary32.
 */

#ifndef HAL_PROJECT_H
#define HAL_PROJECT_H

#include "../platform/types.h"
#include "scene.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_PROJECT_FORMAT_VERSION 1

/* Writes `scene` to `path` atomically. On failure, err (when non-NULL)
 * receives a human-readable reason (truncated to err_cap bytes).
 *
 * Fails with HAL_ERR_IO when the file cannot be written. */
hal_result hal_project_save(const hal_scene *scene, const char *path,
                            char *err, size_t err_cap);

/* Loads a .halcyon file into a freshly created scene, written to
 * *out_scene (caller destroys it with hal_scene_destroy). The file's
 * format version must be <= HAL_PROJECT_FORMAT_VERSION.
 *
 * Fails with HAL_ERR_IO (unreadable), HAL_ERR_INVALID_ARGUMENT (not a
 * .halcyon file / grammar error / bad reference), or
 * HAL_ERR_UNSUPPORTED (newer format version); err carries the reason
 * with a line number. *out_scene stays NULL on failure. */
hal_result hal_project_load(const char *path, hal_scene **out_scene,
                            char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PROJECT_H */
