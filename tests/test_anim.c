/*
 * test_anim.c — clips, keyframes, sampling, easing, project round trip
 */

#include "hal_test.h"
#include "core/anim.h"
#include "core/project.h"
#include "io/io.h"
#include "core/scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-anim-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

static void test_keys_and_sampling(void) {
    hal_scene *s = hal_scene_create("A");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Move");
    HAL_TEST_CHECK(ci >= 0);
    hal_clip *clip = hal_scene_clip(s, ci);
    HAL_TEST_CHECK(clip != NULL);

    /* Two location-X keys: 0 at t=0, 2 at t=2. */
    HAL_TEST_CHECK(hal_clip_key_vec(clip, HAL_CH_LOC_X, cube, -1, 0.0f,
                                    (hal_vec3f){ 0, 0, 0 },
                                    HAL_INTERP_LINEAR));
    HAL_TEST_CHECK(hal_clip_key_vec(clip, HAL_CH_LOC_X, cube, -1, 2.0f,
                                    (hal_vec3f){ 2, 0, 0 },
                                    HAL_INTERP_LINEAR));
    HAL_TEST_CHECK_EQ_I(hal_scene_clip_count(s), 1);

    /* Exact-time replaces; in-between interpolates. */
    hal_object *obj = hal_scene_object(s, cube);
    hal_clip_sample(clip, s, 0.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.x, 0.0f, 1e-5);
    hal_clip_sample(clip, s, 1.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.x, 1.0f, 1e-4);
    hal_clip_sample(clip, s, 2.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.x, 2.0f, 1e-4);
    /* Outside the range clamps. */
    hal_clip_sample(clip, s, 99.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.x, 2.0f, 1e-4);

    /* Replace on the same time. */
    HAL_TEST_CHECK(hal_clip_key_vec(clip, HAL_CH_LOC_X, cube, -1, 2.0f,
                                    (hal_vec3f){ 4, 0, 0 },
                                    HAL_INTERP_LINEAR));
    hal_anim_channel *ch = hal_clip_channel(clip, HAL_CH_LOC_X, cube,
                                            -1);
    HAL_TEST_CHECK_EQ_I(ch->count, 2);
    hal_clip_sample(clip, s, 2.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.x, 4.0f, 1e-4);

    /* Easing: SMOOTH at the midpoint is still the midpoint, but at
     * t=0.25 the eased value differs from linear. */
    hal_clip_key_vec(clip, HAL_CH_LOC_Z, cube, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_clip_key_vec(clip, HAL_CH_LOC_Z, cube, -1, 1.0f,
                     (hal_vec3f){ 0, 0, 1 }, HAL_INTERP_SMOOTH);
    hal_clip_sample(clip, s, 0.5f);
    HAL_TEST_CHECK_EQ_F(obj->location.z, 0.5f, 1e-4);
    hal_clip_sample(clip, s, 0.25f);
    /* smoothstep(0.25) = 0.15625 */
    HAL_TEST_CHECK_EQ_F(obj->location.z, 0.15625f, 1e-3);

    /* STEP holds. */
    hal_clip_key_vec(clip, HAL_CH_LOC_Y, cube, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_STEP);
    hal_clip_key_vec(clip, HAL_CH_LOC_Y, cube, -1, 1.0f,
                     (hal_vec3f){ 0, 5, 0 }, HAL_INTERP_STEP);
    hal_clip_sample(clip, s, 0.9f);
    HAL_TEST_CHECK_EQ_F(obj->location.y, 0.0f, 1e-5);
    hal_clip_sample(clip, s, 1.0f);
    HAL_TEST_CHECK_EQ_F(obj->location.y, 5.0f, 1e-5);

    hal_scene_destroy(s);
}

static void test_rotation_and_loop(void) {
    hal_scene *s = hal_scene_create("R");
    hal_object_id id = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Spin");
    hal_clip *clip = hal_scene_clip(s, ci);

    /* 0 -> 90 degrees about Z over one second. (A 180-degree target
     * is genuinely path-ambiguous under the quaternion double cover —
     * both arcs are valid halves — so the test pins an unambiguous
     * target.) */
    hal_quatf q0 = hal_quatf_identity();
    hal_quatf q1 = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    HAL_TEST_CHECK(hal_clip_key_rot(clip, id, -1, 0.0f, q0,
                                    HAL_INTERP_LINEAR));
    HAL_TEST_CHECK(hal_clip_key_rot(clip, id, -1, 1.0f, q1,
                                    HAL_INTERP_LINEAR));

    hal_object *obj = hal_scene_object(s, id);
    hal_clip_sample(clip, s, 0.5f);
    /* Halfway = 45 degrees. */
    hal_vec3f v = hal_quatf_rotate_vec3(obj->rotation,
                                        hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(v.x, 0.7071f, 1e-3);
    HAL_TEST_CHECK_EQ_F(v.y, 0.7071f, 1e-3);

    /* Loop: sampling past the end wraps. */
    clip->loop = true;
    hal_clip_sample(clip, s, 1.25f);
    /* 1.25 wraps to 0.25: 22.5 degrees. */
    v = hal_quatf_rotate_vec3(obj->rotation, hal_vec3f_axis_x());
    HAL_TEST_CHECK_EQ_F(v.x, 0.9239f, 1e-3);
    HAL_TEST_CHECK_EQ_F(v.y, 0.3827f, 1e-3);

    hal_scene_destroy(s);
}

static void test_bone_channel_and_project_round_trip(void) {
    hal_scene *s = hal_scene_create("P");
    hal_object_id arm_id =
        hal_scene_object_add(s, "Arm", HAL_OBJECT_ARMATURE);
    hal_armature *arm = hal_armature_create();
    hal_armature_bone_add(arm, "Upper", hal_vec3f_zero(),
                          hal_vec3f_make(0, 1, 0), -1);
    hal_object_set_armature(s, arm_id, arm);

    int ci = hal_scene_clip_add(s, "Wave");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 1.0f;

    /* Bone pose keys: rest -> 90 degrees about Z. */
    hal_quatf q1 = hal_quatf_from_axis_angle(hal_vec3f_axis_z(),
                                             90.0f * HAL_DEG2RAD_f32);
    HAL_TEST_CHECK(hal_clip_key_rot(clip, arm_id, 0, 0.0f,
                                    hal_quatf_identity(),
                                    HAL_INTERP_SMOOTH));
    HAL_TEST_CHECK(hal_clip_key_rot(clip, arm_id, 0, 1.0f, q1,
                                    HAL_INTERP_SMOOTH));

    /* Direct sample at the end (loop OFF: the end time is itself). */
    hal_clip_sample(clip, s, 1.0f);
    hal_bone_pose pose[1];
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.x, -1.0f, 1e-3);
    clip->loop = true; /* for the persisted round trip below */

    /* Project round trip: the clip, its bone channel, keys, loop and
     * range all survive. */
    char path[512];
    snprintf(path, sizeof(path), "%s/anim.halcyon", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)),
                        HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_clip_count(loaded), 1);
        hal_clip *lc = hal_scene_clip(loaded, 0);
        HAL_TEST_CHECK(lc != NULL);
        if (lc) {
            HAL_TEST_CHECK(strcmp(lc->name, "Wave") == 0);
            HAL_TEST_CHECK(lc->loop);
            HAL_TEST_CHECK_EQ_F(lc->end, 1.0f, 1e-5);
            HAL_TEST_CHECK_EQ_I(lc->count, 1);
            if (lc->count == 1) {
                HAL_TEST_CHECK_EQ_I(lc->channels[0].type, HAL_CH_BONE);
                HAL_TEST_CHECK_EQ_I(lc->channels[0].bone, 0);
                HAL_TEST_CHECK_EQ_I(lc->channels[0].count, 2);
                /* Sampling still works after the round trip (the
                 * clip loops: 1.5 wraps to 0.5, the smooth-eased
                 * midpoint = 45 degrees). */
                hal_clip_sample(lc, loaded, 1.5f);
                hal_object *lo = hal_scene_object_at(loaded, 0);
                HAL_TEST_CHECK(lo && lo->armature);
                if (lo && lo->armature) {
                    hal_bone_pose lp[1];
                    hal_armature_pose(lo->armature, lp);
                    HAL_TEST_CHECK_EQ_F(lp[0].tail.x, -0.7071f, 1e-3);
                }
            }
        }
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

static void test_gltf_animation_export(void) {
    hal_scene *s = hal_scene_create("X");
    hal_object_id cube = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    hal_object_set_mesh(s, cube, hal_mesh_cube(0.5f));
    int ci = hal_scene_clip_add(s, "Move");
    hal_clip *clip = hal_scene_clip(s, ci);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, cube, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, cube, -1, 1.0f,
                     (hal_vec3f){ 2, 0, 0 }, HAL_INTERP_SMOOTH);

    char path[512];
    snprintf(path, sizeof(path), "%s/anim.glb", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_io_gltf_export(s, path, err, sizeof(err)),
                        HAL_OK);

    /* The JSON chunk carries the animation, its channels and LINEAR
     * samplers. */
    FILE *f = fopen(path, "rb");
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        hal_u32 jlen, jtype;
        fseek(f, 12, SEEK_SET);
        HAL_TEST_CHECK(fread(&jlen, 4, 1, f) == 1);
        HAL_TEST_CHECK(fread(&jtype, 4, 1, f) == 1);
        char *json = malloc(jlen + 1);
        HAL_TEST_CHECK(json != NULL);
        if (json) {
            HAL_TEST_CHECK(fread(json, 1, jlen, f) == jlen);
            json[jlen] = 0;
            HAL_TEST_CHECK(strstr(json, "\"animations\"") != NULL);
            HAL_TEST_CHECK(strstr(json, "Move") != NULL);
            HAL_TEST_CHECK(strstr(json, "\"path\":\"translation\"") !=
                           NULL);
            HAL_TEST_CHECK(
                strstr(json, "\"interpolation\":\"LINEAR\"") != NULL);
        }
        free(json);
        fclose(f);
    }

    /* The GLB still re-imports (geometry unaffected). */
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(
        hal_io_gltf_import(path, &loaded, err, sizeof(err)), HAL_OK);
    if (loaded)
        hal_scene_destroy(loaded);
    hal_scene_destroy(s);
}

int main(void) {
    test_keys_and_sampling();
    test_rotation_and_loop();
    test_bone_channel_and_project_round_trip();
    test_gltf_animation_export();
    HAL_TEST_SUMMARY("test_anim");
}
