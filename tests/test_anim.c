/*
 * test_anim.c — clips, keyframes, sampling, easing, project round trip
 */

#include "hal_test.h"
#include "core/anim.h"
#include "core/project.h"
#include "io/io.h"
#include "platform/platform.h"
#include "ui/ui.h"
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

/* The curve editor widget paints the bound clip's curves (grid,
 * polylines, key dots, playhead) — verified by pixel content, not
 * screenshots. */
static void test_curves_widget(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 300, 300 });
    hal_widget *cw = hal_curves_create(root);
    hal_widget_set_rect(cw, (hal_rectf){ 10, 10, 200, 150 });

    /* A clip with an eased location-X channel. */
    hal_scene *s = hal_scene_create("C");
    hal_object_id id = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Graph");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 1.0f;
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 1.0f,
                     (hal_vec3f){ 1, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_curves_bind(cw, clip);
    hal_curves_set_playhead(cw, 0.5f);
    hal_ui_layout(ui);

    hal_surface *surf = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(300, 300, &surf),
                        HAL_OK);
    hal_canvas canvas;
    HAL_TEST_CHECK(hal_canvas_init(&canvas, surf, 1.0f));
    hal_color bg = hal_color_hex(0x141a3cu);
    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 300, 300 }, bg);
    hal_ui_paint(ui, &canvas);

    int drawn = 0;
    for (int y = 12; y < 158; y += 2)
        for (int x = 12; x < 208; x += 2) {
            hal_u32 p = canvas.pixels[(size_t)y * (size_t)canvas.stride +
                                      (size_t)x];
            if ((p & 0xFFu) != (hal_u32)(bg.b * 255.0f))
                drawn++;
        }
    HAL_TEST_CHECK(drawn > 40);

    /* Unbound: still paints (the empty note). */
    hal_curves_bind(cw, NULL);
    hal_ui_paint(ui, &canvas);
    hal_surface_destroy(surf);
    hal_scene_destroy(s);
    hal_ui_destroy(ui);
}

/* ---- Curve editor interaction (synthesized pointer events) ---- */

static int g_edit_begin = 0, g_edit_end = 0;
static char g_edit_last_text[64] = { 0 };

static void edit_counter(int phase, const char *text, void *user) {
    (void)user;
    if (phase == HAL_CURVES_EDIT_BEGIN)
        g_edit_begin++;
    else if (phase == HAL_CURVES_EDIT_END) {
        g_edit_end++;
        snprintf(g_edit_last_text, sizeof(g_edit_last_text), "%s",
                 text ? text : "");
    }
}

static bool curve_motion(hal_ui *ui, float x, float y) {
    hal_event e = { .type = HAL_EVENT_POINTER_MOTION };
    e.pointer.position = (hal_pointf){ x, y };
    return hal_ui_handle_event(ui, &e);
}

static bool curve_press(hal_ui *ui, float x, float y, int button) {
    hal_event e = { .type = HAL_EVENT_POINTER_BUTTON_DOWN };
    e.pointer_button.position = (hal_pointf){ x, y };
    e.pointer_button.button = button == 3 ? HAL_BUTTON_RIGHT
                                          : HAL_BUTTON_LEFT;
    return hal_ui_handle_event(ui, &e);
}

static bool curve_release(hal_ui *ui, float x, float y) {
    hal_event e = { .type = HAL_EVENT_POINTER_BUTTON_UP };
    e.pointer_button.position = (hal_pointf){ x, y };
    e.pointer_button.button = HAL_BUTTON_LEFT;
    return hal_ui_handle_event(ui, &e);
}

/* A LOC_X channel 0 -> 1 over [0, 1]; drag the LAST key to the
 * middle of the graph in time and halfway in value. */
static void test_curve_key_drag(void) {
    hal_scene *s = hal_scene_create("C");
    hal_object_id id = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Drag");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 1.0f;
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 1.0f,
                     (hal_vec3f){ 1, 0, 0 }, HAL_INTERP_SMOOTH);

    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 300, 300 });
    hal_widget *cw = hal_curves_create(root);
    hal_widget_set_rect(cw, (hal_rectf){ 10, 10, 200, 150 });
    hal_curves_interactive(cw, clip, edit_counter, NULL);
    hal_ui_layout(ui);

    /* Key 2: time=1, value=1 -> screen: x = 10+200 = 210 (right
     * edge), y = 10 + 0 = 10 (top; bounds are 0..1). */
    g_edit_begin = g_edit_end = 0;
    HAL_TEST_CHECK(curve_press(ui, 209, 11, 1));
    /* Drag to the middle: time 0.5, value ~0.5. */
    HAL_TEST_CHECK(curve_motion(ui, 110, 85));
    HAL_TEST_CHECK(curve_release(ui, 110, 85));

    HAL_TEST_CHECK_EQ_I(g_edit_begin, 1);
    HAL_TEST_CHECK_EQ_I(g_edit_end, 1);
    /* The channel still has 2 keys; the moved one is at t=0.5 with
     * value ~0.5 (bounds frozen at 0..1 during the drag). */
    HAL_TEST_CHECK_EQ_I((int)clip->count, 1);
    hal_anim_channel *ch = &clip->channels[0];
    HAL_TEST_CHECK_EQ_I((int)ch->count, 2);
    HAL_TEST_CHECK_EQ_F(ch->keys[1].time, 0.5f, 0.02f);
    HAL_TEST_CHECK_EQ_F(ch->keys[1].vec.x, 0.5f, 0.05f);
    /* Keys stay time-sorted. */
    HAL_TEST_CHECK(ch->keys[0].time < ch->keys[1].time);

    hal_ui_destroy(ui);
    hal_scene_destroy(s);
}

/* Double-click on a key cycles easing; double-click on empty space
 * inserts; right-click deletes. */
static void test_curve_insert_delete_easing(void) {
    hal_scene *s = hal_scene_create("C2");
    hal_object_id id = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Edit");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 1.0f;
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_SMOOTH);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 1.0f,
                     (hal_vec3f){ 1, 0, 0 }, HAL_INTERP_SMOOTH);

    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 300, 300 });
    hal_widget *cw = hal_curves_create(root);
    hal_widget_set_rect(cw, (hal_rectf){ 10, 10, 200, 150 });
    hal_curves_interactive(cw, clip, edit_counter, NULL);
    hal_ui_layout(ui);

    /* Double-click the FIRST key (t=0, v=0 -> screen ~(11, 159)):
     * easing cycles SMOOTH -> STEP. */
    curve_press(ui, 12, 158, 1);
    g_edit_begin = g_edit_end = 0;
    HAL_TEST_CHECK(curve_press(ui, 12, 158, 1));
    HAL_TEST_CHECK_EQ_I((int)clip->channels[0].keys[0].interp,
                        (int)HAL_INTERP_STEP);
    HAL_TEST_CHECK(curve_release(ui, 12, 158));

    /* Double-click empty space mid-graph: a key is inserted into
     * LOC_X near t=0.5. */
    g_edit_begin = g_edit_end = 0;
    curve_press(ui, 60, 100, 1); /* first click (prime) */
    HAL_TEST_CHECK(curve_press(ui, 60, 100, 1)); /* double-click */
    HAL_TEST_CHECK_EQ_I((int)clip->channels[0].count, 3);
    HAL_TEST_CHECK_EQ_I(g_edit_begin, 1);
    HAL_TEST_CHECK_EQ_I(g_edit_end, 1);
    HAL_TEST_CHECK_EQ_F(clip->channels[0].keys[1].time, 0.25f,
                        0.06f);

    /* Right-click the inserted key deletes it. */
    g_edit_begin = g_edit_end = 0;
    HAL_TEST_CHECK(curve_press(ui, 60, 100, 3)); /* right button */
    HAL_TEST_CHECK_EQ_I((int)clip->channels[0].count, 2);
    HAL_TEST_CHECK_EQ_I(g_edit_begin, 1);
    HAL_TEST_CHECK_EQ_I(g_edit_end, 1);

    hal_ui_destroy(ui);
    hal_scene_destroy(s);
}

/* Core-level: move re-sorts, delete compacts (and kills empty
 * channels), clone round-trips. */
static void test_curve_core_ops(void) {
    hal_scene *s = hal_scene_create("C3");
    hal_object_id id = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    int ci = hal_scene_clip_add(s, "Ops");
    hal_clip *clip = hal_scene_clip(s, ci);
    clip->end = 1.0f;
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 0.0f,
                     (hal_vec3f){ 0, 0, 0 }, HAL_INTERP_LINEAR);
    hal_clip_key_vec(clip, HAL_CH_LOC_X, id, -1, 1.0f,
                     (hal_vec3f){ 1, 0, 0 }, HAL_INTERP_LINEAR);

    /* Move the second key to t=0.25 (between): re-sorted, the range
     * follows (no key sits at t=1 anymore). */
    HAL_TEST_CHECK(hal_clip_key_move(clip, 0, 1, 0.25f,
                                     (hal_vec3f){ 0.5f, 0, 0 },
                                     hal_quatf_identity()));
    HAL_TEST_CHECK_EQ_F(clip->channels[0].keys[0].time, 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(clip->channels[0].keys[1].time, 0.25f, 1e-5);
    HAL_TEST_CHECK_EQ_F(clip->end, 0.25f, 1e-5);

    /* Clone matches, and edits to the clone do not touch the
     * original. */
    hal_clip *cl = hal_clip_clone(clip);
    HAL_TEST_CHECK(cl != NULL);
    if (cl) {
        HAL_TEST_CHECK_EQ_I((int)cl->count, (int)clip->count);
        HAL_TEST_CHECK_EQ_F(cl->channels[0].keys[1].vec.x, 0.5f,
                            1e-5);
        hal_clip_key_delete(cl, 0, 0);
        hal_clip_key_delete(cl, 0, 0);
        HAL_TEST_CHECK_EQ_I((int)cl->count, 0); /* channel died */
        HAL_TEST_CHECK_EQ_I((int)clip->channels[0].count, 2);
        hal_clip_destroy(cl);
    }

    /* Delete updates the range (the t=0.25 key goes; only t=0
     * remains). */
    hal_clip_key_delete(clip, 0, 1);
    HAL_TEST_CHECK_EQ_F(clip->end, 0.0f, 1e-5);
    hal_scene_destroy(s);
}

int main(void) {
    test_keys_and_sampling();
    test_rotation_and_loop();
    test_bone_channel_and_project_round_trip();
    test_gltf_animation_export();
    test_curves_widget();
    test_curve_core_ops();
    test_curve_key_drag();
    test_curve_insert_delete_easing();
    HAL_TEST_SUMMARY("test_anim");
}
