/*
 * autosave.c — Halcyon autosave implementation
 */

#include "autosave.h"
#include "project.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct hal_autosave {
    char *path;             /* owned; NULL = disabled               */
    unsigned interval;      /* seconds between writes               */
    bool dirty;
    bool started;           /* the schedule anchors at the first tick */
    double last_save;       /* monotonic clock, from tick            */
};

hal_autosave *hal_autosave_create(const char *autosave_path,
                                  unsigned interval_seconds) {
    hal_autosave *as = calloc(1, sizeof(*as));
    if (!as)
        return NULL;
    if (autosave_path && autosave_path[0]) {
        as->path = malloc(strlen(autosave_path) + 1);
        if (!as->path) {
            free(as);
            return NULL;
        }
        strcpy(as->path, autosave_path);
    }
    as->interval = interval_seconds > 0 ? interval_seconds : 1u;
    as->last_save = 0.0;
    return as;
}

void hal_autosave_destroy(hal_autosave *autosave) {
    if (!autosave)
        return;
    free(autosave->path);
    free(autosave);
}

void hal_autosave_mark_dirty(hal_autosave *autosave) {
    if (autosave)
        autosave->dirty = true;
}

void hal_autosave_mark_clean(hal_autosave *autosave) {
    if (autosave)
        autosave->dirty = false;
}

bool hal_autosave_is_dirty(const hal_autosave *autosave) {
    return autosave && autosave->dirty;
}

bool hal_autosave_tick(hal_autosave *autosave, const hal_scene *scene,
                       double now_seconds) {
    if (!autosave || !autosave->path || !scene || !autosave->dirty)
        return false;

    /* The schedule anchors at the first tick: the clock's origin is
     * the caller's (process uptime, monotonic), so "fresh document"
     * cannot mean t=0. A fresh document waits one full interval like
     * every other. */
    if (!autosave->started) {
        autosave->started = true;
        autosave->last_save = now_seconds;
        return false;
    }

    if (now_seconds - autosave->last_save < (double)autosave->interval)
        return false;

    hal_result r = hal_project_save(scene, autosave->path, NULL, 0);
    autosave->last_save = now_seconds;
    if (r == HAL_OK)
        autosave->dirty = false;
    /* A failed autosave stays dirty and retries at the next interval —
     * a full disk should not turn into a busy loop. */
    return true;
}

bool hal_autosave_recovery_due(const char *project_path,
                               const char *autosave_path) {
    if (!autosave_path || !autosave_path[0])
        return false;

    struct stat st_auto;
    if (stat(autosave_path, &st_auto) != 0)
        return false; /* nothing to recover */

    if (!project_path || !project_path[0])
        return true; /* unsaved document with an autosave on disk */

    struct stat st_project;
    if (stat(project_path, &st_project) != 0)
        return true; /* the project file vanished; the autosave lives */

    return st_auto.st_mtime > st_project.st_mtime;
}
