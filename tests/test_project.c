/*
 * test_project.c — .halcyon project format + autosave tests
 *
 * Round trips (the format's definition of done), strict-parser
 * failure modes, atomic save behavior, and the autosave schedule and
 * crash-recovery condition.
 */

#include "hal_test.h"
#include "core/autosave.h"
#include "core/project.h"
#include "core/scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-test-%d", (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
}

static bool scenes_equal(const hal_scene *a, const hal_scene *b) {
    if (strcmp(a->name, b->name) != 0)
        return false;
    if (a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; i++) {
        const hal_object *oa = &a->objects[i];
        const hal_object *ob = &b->objects[i];
        if (oa->id != ob->id || strcmp(oa->name, ob->name) != 0 ||
            oa->type != ob->type || oa->parent != ob->parent ||
            oa->visible != ob->visible || oa->selected != ob->selected)
            return false;
        if (!hal_vec3f_eq(oa->location, ob->location) ||
            !hal_vec3f_eq(oa->scale, ob->scale))
            return false;
        if (!hal_f32_eq(oa->rotation.x, ob->rotation.x) ||
            !hal_f32_eq(oa->rotation.y, ob->rotation.y) ||
            !hal_f32_eq(oa->rotation.z, ob->rotation.z) ||
            !hal_f32_eq(oa->rotation.w, ob->rotation.w))
            return false;
    }
    return true;
}

static void test_round_trip(void) {
    hal_scene *s = hal_scene_create("My Project");

    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_location(s, cube, hal_vec3f_make(1.25f, -3.5f, 0.0f));
    hal_object_set_rotation_euler(
        s, cube, hal_vec3f_make(0.1f, 0.9f, -0.4f));
    hal_object_set_scale(s, cube, hal_vec3f_make(2.0f, 2.0f, 0.5f));

    hal_object_id sub = hal_scene_object_add(s, "SubCube", HAL_OBJECT_MESH);
    HAL_TEST_CHECK(hal_object_set_parent(s, sub, cube));
    hal_object_set_location(s, sub, hal_vec3f_make(0.0f, 1.0f, 0.0f));

    hal_object_id cam = hal_scene_object_add(s, "Camera", HAL_OBJECT_CAMERA);
    hal_scene_select_only(s, cam);
    hal_object_set_visible(s, cam, false);

    char path[512];
    snprintf(path, sizeof(path), "%s/rt.halcyon", tmp_dir());
    unlink(path);

    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)), HAL_OK);

    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err, sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK(scenes_equal(s, loaded));
        /* The saved rotation round-trips through normalization. */
        hal_object *lc = hal_scene_object(loaded, cube);
        HAL_TEST_CHECK(lc != NULL);
        HAL_TEST_CHECK_EQ_F(lc->location.x, 1.25f, 1e-6);
        HAL_TEST_CHECK_EQ_F(lc->location.y, -3.5f, 1e-6);
        hal_object *lsub = hal_scene_object(loaded, sub);
        HAL_TEST_CHECK(lsub != NULL && lsub->parent == cube);
        hal_object *lcam = hal_scene_object(loaded, cam);
        HAL_TEST_CHECK(lcam != NULL && !lcam->visible && lcam->selected);
        hal_scene_destroy(loaded);
    }

    /* No temp file survives the atomic save. */
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp~", path);
    struct stat st;
    HAL_TEST_CHECK(stat(tmp_path, &st) != 0);

    hal_scene_destroy(s);
}

static void test_parse_failures(void) {
    char path[512];
    char err[256];
    hal_scene *scene;

    /* Not a halcyon file. */
    snprintf(path, sizeof(path), "%s/bad1.halcyon", tmp_dir());
    write_file(path, "hello world\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);
    HAL_TEST_CHECK(scene == NULL);

    /* Newer format version. */
    snprintf(path, sizeof(path), "%s/bad2.halcyon", tmp_dir());
    write_file(path, "halcyon 99\nscene \"X\"\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_UNSUPPORTED);

    /* Unknown key. */
    snprintf(path, sizeof(path), "%s/bad3.halcyon", tmp_dir());
    write_file(path, "halcyon 1\nfrobnicate 1 2 3\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);
    HAL_TEST_CHECK(strstr(err, "line 2") != NULL);

    /* Property outside an object block. */
    snprintf(path, sizeof(path), "%s/bad4.halcyon", tmp_dir());
    write_file(path, "halcyon 1\nloc 1 2 3\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);

    /* Missing 'end'. */
    snprintf(path, sizeof(path), "%s/bad5.halcyon", tmp_dir());
    write_file(path, "halcyon 1\nobject 1 \"A\" mesh\n  parent 0\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);

    /* Forward parent reference (cycle or typo — both refused). */
    snprintf(path, sizeof(path), "%s/bad6.halcyon", tmp_dir());
    write_file(path,
               "halcyon 1\nscene \"S\"\nobject 1 \"A\" mesh\n"
               "  parent 2\nend\nobject 2 \"B\" mesh\n  parent 0\nend\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);

    /* Unknown object type. */
    snprintf(path, sizeof(path), "%s/bad7.halcyon", tmp_dir());
    write_file(path, "halcyon 1\nobject 1 \"A\" widget\n  parent 0\nend\n");
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &scene, err, sizeof(err)),
                        HAL_ERR_INVALID_ARGUMENT);

    /* Unreadable file. */
    scene = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_project_load("/tmp/halcyon-test-does-not-exist.halcyon", &scene,
                         err, sizeof(err)),
        HAL_ERR_IO);

    /* Save into an impossible directory fails loudly. */
    hal_scene *s = hal_scene_create("X");
    HAL_TEST_CHECK_EQ_I(
        hal_project_save(s, "/nonexistent-dir/x.halcyon", err, sizeof(err)),
        HAL_ERR_IO);
    hal_scene_destroy(s);
}

static void test_empty_scene(void) {
    char path[512];
    char err[256];
    snprintf(path, sizeof(path), "%s/empty.halcyon", tmp_dir());

    hal_scene *s = hal_scene_create("Empty");
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)), HAL_OK);
    hal_scene_destroy(s);

    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err, sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 0);
        HAL_TEST_CHECK(strcmp(loaded->name, "Empty") == 0);
        hal_scene_destroy(loaded);
    }

    /* A file with only a header loads as an empty scene named Scene. */
    write_file(path, "halcyon 1\n");
    loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err, sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    hal_scene_destroy(loaded);
}

static void test_autosave_schedule(void) {
    char auto_path[512];
    snprintf(auto_path, sizeof(auto_path), "%s/autosave.halcyon", tmp_dir());
    unlink(auto_path);

    hal_scene *s = hal_scene_create("Work");
    hal_object_id a = hal_scene_object_add(s, "Box", HAL_OBJECT_MESH);
    hal_object_set_location(s, a, hal_vec3f_make(1.0f, 2.0f, 3.0f));

    hal_autosave *as = hal_autosave_create(auto_path, 60);
    HAL_TEST_CHECK(as != NULL);
    HAL_TEST_CHECK(!hal_autosave_is_dirty(as));

    /* Clean documents never write. */
    HAL_TEST_CHECK(!hal_autosave_tick(as, s, 10.0));
    struct stat st;
    HAL_TEST_CHECK(stat(auto_path, &st) != 0);

    /* Dirty but not due yet. */
    hal_autosave_mark_dirty(as);
    HAL_TEST_CHECK(!hal_autosave_tick(as, s, 20.0));
    HAL_TEST_CHECK(stat(auto_path, &st) != 0);

    /* Due: writes and clears the dirty flag. */
    HAL_TEST_CHECK(hal_autosave_tick(as, s, 80.0));
    HAL_TEST_CHECK(stat(auto_path, &st) == 0);
    HAL_TEST_CHECK(!hal_autosave_is_dirty(as));

    /* The autosave is a valid project containing the edit. */
    hal_scene *recovered = NULL;
    char err[256];
    HAL_TEST_CHECK_EQ_I(
        hal_project_load(auto_path, &recovered, err, sizeof(err)), HAL_OK);
    HAL_TEST_CHECK(recovered != NULL);
    if (recovered) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(recovered), 1);
        hal_scene_destroy(recovered);
    }

    /* Mark clean (user saved): stays quiet even when due. */
    hal_autosave_mark_dirty(as);
    hal_autosave_mark_clean(as);
    HAL_TEST_CHECK(!hal_autosave_tick(as, s, 1000.0));

    hal_autosave_destroy(as);
    hal_autosave_destroy(NULL); /* safe no-op */
    hal_scene_destroy(s);
}

/* Pushes a file's mtime `delta_seconds` into the future (negative
 * pulls it back) so mtime-order tests never depend on the filesystem
 * clock's one-second granularity. */
static void bump_mtime(const char *path, long delta_seconds) {
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    struct timeval times[2];
    times[0].tv_sec = st.st_atime;
    times[0].tv_usec = 0;
    times[1].tv_sec = st.st_mtime + delta_seconds;
    times[1].tv_usec = 0;
    utimes(path, times);
}

static void test_autosave_recovery(void) {
    char proj[512], auto_path[512];
    snprintf(proj, sizeof(proj), "%s/recover.halcyon", tmp_dir());
    snprintf(auto_path, sizeof(auto_path), "%s/recover.halcyon.autosave",
             tmp_dir());
    unlink(proj);
    unlink(auto_path);

    /* No autosave: nothing to recover. */
    HAL_TEST_CHECK(!hal_autosave_recovery_due(proj, auto_path));

    hal_scene *s = hal_scene_create("R");
    char err[256];
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, proj, err, sizeof(err)), HAL_OK);

    /* Autosave appears AFTER the project: recovery due. */
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, auto_path, err, sizeof(err)),
                        HAL_OK);
    bump_mtime(auto_path, 10);
    HAL_TEST_CHECK(hal_autosave_recovery_due(proj, auto_path));

    /* The project is re-saved (newer): recovery is over. */
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, proj, err, sizeof(err)), HAL_OK);
    bump_mtime(proj, 20);
    HAL_TEST_CHECK(!hal_autosave_recovery_due(proj, auto_path));

    /* Project gone but autosave lives: recovery due. */
    unlink(proj);
    HAL_TEST_CHECK(hal_autosave_recovery_due(proj, auto_path));

    hal_scene_destroy(s);
}

int main(void) {
    test_round_trip();
    test_parse_failures();
    test_empty_scene();
    test_autosave_schedule();
    test_autosave_recovery();
    HAL_TEST_SUMMARY("test_project");
}
