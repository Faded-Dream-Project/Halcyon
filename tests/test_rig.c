/*
 * test_rig.c — armatures: pose math, skinning, auto weights, IK
 */

#include "hal_test.h"
#include "core/armature.h"
#include "core/mesh.h"
#include "core/project.h"
#include "core/scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-rig-test-%d",
                 (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

/* A 2-bone chain up the Y axis: (0,0,0)-(0,1,0)-(0,2,0). */
static hal_armature *chain2(void) {
    hal_armature *arm = hal_armature_create();
    hal_armature_bone_add(arm, "Upper", hal_vec3f_zero(),
                          hal_vec3f_make(0, 1, 0), -1);
    hal_armature_bone_add(arm, "Lower", hal_vec3f_make(0, 1, 0),
                          hal_vec3f_make(0, 2, 0), 0);
    return arm;
}

static void test_pose_chain(void) {
    hal_armature *arm = chain2();

    /* Rest: identity pose keeps heads and tails in place. */
    hal_bone_pose pose[2];
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.y, 1.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(pose[1].tail.y, 2.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(pose[1].head.y, 1.0f, 1e-5);

    /* Rotating the parent 90 deg about Z swings the child: the tip
     * moves from (0,2,0) to (-2,1,0)... rotate (0,1,0) by 90deg
     * about Z gives (-1,0,0); the upper tail goes to (-1,0,0), and
     * the child (whose rest offsets ride the parent's matrix)
     * follows: the child head lands at the parent's posed tail, and
     * the tip ends at (-2, 1... let me just verify the tip follows
     * the rotation: |tip| stays 2 and it lies off the Y axis. */
    arm->bones[0].pose = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.x, -1.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(pose[0].tail.y, 0.0f, 1e-4);
    /* The child head must sit at the parent's posed tail. */
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].head, pose[0].tail) <
                   1e-4);
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].tail,
                                      hal_vec3f_make(-2.0f, 0.0f, 0.0f)) <
                   1e-4);
    hal_armature_destroy(arm);
}

static void test_auto_weights(void) {
    hal_armature *arm = chain2();
    hal_mesh *m = hal_mesh_plane(1.0f);

    HAL_TEST_CHECK(hal_mesh_bind_armature(m, arm));
    HAL_TEST_CHECK(m->skin_weights != NULL && m->skin_bones != NULL);
    for (size_t v = 0; v < m->vert_count; v++) {
        hal_f32 sum = 0.0f;
        int used = 0;
        for (size_t i = 0; i < 4; i++) {
            if (m->skin_bones[v * 4 + i] >= 0) {
                sum += m->skin_weights[v * 4 + i];
                used++;
            }
        }
        HAL_TEST_CHECK(used >= 1 && used <= 2);
        HAL_TEST_CHECK_EQ_F(sum, 1.0f, 1e-4);
    }
    hal_mesh_destroy(m);
    hal_armature_destroy(arm);
}

static void test_skinning(void) {
    hal_armature *arm = chain2();
    hal_mesh *m = hal_mesh_plane(1.0f);
    hal_mesh_bind_armature(m, arm);

    /* Unposed skin leaves verts in place. */
    hal_vec3f out[4];
    HAL_TEST_CHECK(hal_mesh_apply_pose(m, arm, out));
    for (size_t v = 0; v < m->vert_count; v++)
        HAL_TEST_CHECK(hal_vec3f_distance(out[v], m->verts[v]) < 1e-5);

    /* Posing the parent moves verts (the plane rides the chain). */
    arm->bones[0].pose = hal_quatf_from_axis_angle(
        hal_vec3f_axis_z(), 90.0f * HAL_DEG2RAD_f32);
    HAL_TEST_CHECK(hal_mesh_apply_pose(m, arm, out));
    bool moved = false;
    for (size_t v = 0; v < m->vert_count; v++)
        if (hal_vec3f_distance(out[v], m->verts[v]) > 0.1f)
            moved = true;
    HAL_TEST_CHECK(moved);

    /* A far-away mesh with weights still sums to a valid position
     * (no NaNs). */
    for (size_t v = 0; v < m->vert_count; v++) {
        HAL_TEST_CHECK(isfinite(out[v].x) && isfinite(out[v].y) &&
                       isfinite(out[v].z));
    }
    hal_mesh_destroy(m);
    hal_armature_destroy(arm);
}

static void test_ik(void) {
    hal_armature *arm = chain2();

    /* Reach to (1, 1, 0): within the 2-unit reach. */
    HAL_TEST_CHECK(
        hal_armature_ik_2bone(arm, 1, hal_vec3f_make(1.0f, 1.0f, 0.0f)));
    hal_bone_pose pose[2];
    hal_armature_pose(arm, pose);
    /* The tip must land on (or very near) the target. */
    HAL_TEST_CHECK(hal_vec3f_distance(pose[1].tail,
                                      hal_vec3f_make(1.0f, 1.0f, 0.0f)) <
                   1e-3);

    /* Out-of-reach targets clamp to the maximum reach: the tip lands
     * on the sphere of radius l1 + l2 = 2 in the target direction. */
    hal_armature_destroy(arm);
    arm = chain2();
    HAL_TEST_CHECK(hal_armature_ik_2bone(arm, 1,
                                         hal_vec3f_make(10.0f, 0.0f,
                                                        0.0f)));
    hal_armature_pose(arm, pose);
    HAL_TEST_CHECK_EQ_F(hal_vec3f_length(pose[1].tail), 2.0f, 1e-3);
    HAL_TEST_CHECK(hal_vec3f_distance(
                       pose[1].tail,
                       hal_vec3f_make(2.0f, 0.0f, 0.0f)) < 1e-3);

    hal_armature_destroy(arm);
}

static void test_project_armature_round_trip(void) {
    hal_scene *s = hal_scene_create("Rig");
    hal_object_id arm_id =
        hal_scene_object_add(s, "Rig", HAL_OBJECT_ARMATURE);
    hal_object_set_armature(s, arm_id, chain2());

    hal_object_id mesh_id =
        hal_scene_object_add(s, "Limb", HAL_OBJECT_MESH);
    hal_mesh *m = hal_mesh_cube(0.5f);
    hal_object_set_mesh(s, mesh_id, m);
    hal_object_set_skin(s, mesh_id, arm_id);

    char path[512];
    snprintf(path, sizeof(path), "%s/rig.halcyon", tmp_dir());
    char err[256] = { 0 };
    HAL_TEST_CHECK_EQ_I(hal_project_save(s, path, err, sizeof(err)),
                        HAL_OK);
    hal_scene *loaded = NULL;
    HAL_TEST_CHECK_EQ_I(hal_project_load(path, &loaded, err,
                                         sizeof(err)),
                        HAL_OK);
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_I(hal_scene_object_count(loaded), 2);
        hal_object *lo = hal_scene_object_at(loaded, 0);
        HAL_TEST_CHECK(lo && lo->armature);
        if (lo && lo->armature) {
            HAL_TEST_CHECK_EQ_I(lo->armature->count, 2);
            HAL_TEST_CHECK_EQ_I(lo->armature->bones[1].parent, 0);
            HAL_TEST_CHECK_EQ_F(lo->armature->bones[1].head.y, 1.0f,
                                1e-5);
        }
        hal_object *lm = hal_scene_object_at(loaded, 1);
        HAL_TEST_CHECK(lm && lm->skin_armature == lo->id);
        hal_scene_destroy(loaded);
    }
    hal_scene_destroy(s);
}

int main(void) {
    test_pose_chain();
    test_auto_weights();
    test_skinning();
    test_ik();
    test_project_armature_round_trip();
    HAL_TEST_SUMMARY("test_rig");
}
