/*
 * test_scene.c — scene graph tests
 *
 * Object lifecycle, parenting (including cycle rejection), world
 * matrix composition through the hierarchy, dirty propagation.
 */

#include "hal_test.h"
#include "core/scene.h"

#include <string.h>

static void test_lifecycle(void) {
    hal_scene *s = hal_scene_create("Test");
    HAL_TEST_CHECK(s != NULL);

    /* NULL name falls back to "Scene". */
    hal_scene *anon = hal_scene_create(NULL);
    HAL_TEST_CHECK(anon != NULL);
    if (anon) {
        HAL_TEST_CHECK(strcmp(anon->name, "Scene") == 0);
        hal_scene_destroy(anon);
    }

    HAL_TEST_CHECK_EQ_I(hal_scene_object_count(s), 0);

    hal_object_id a = hal_scene_object_add(s, "Cube", HAL_OBJECT_MESH);
    HAL_TEST_CHECK(a != HAL_OBJECT_ID_NONE);
    HAL_TEST_CHECK_EQ_I(hal_scene_object_count(s), 1);

    hal_object *obj = hal_scene_object(s, a);
    HAL_TEST_CHECK(obj != NULL);
    HAL_TEST_CHECK(strcmp(obj->name, "Cube") == 0);
    HAL_TEST_CHECK(obj->type == HAL_OBJECT_MESH);
    HAL_TEST_CHECK(obj->visible);
    HAL_TEST_CHECK(!obj->selected);
    HAL_TEST_CHECK(obj->parent == HAL_OBJECT_ID_NONE);

    /* Identity transform. */
    HAL_TEST_CHECK_EQ_F(obj->location.x, 0.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(obj->rotation.w, 1.0f, 1e-6);
    HAL_TEST_CHECK_EQ_F(obj->scale.y, 1.0f, 1e-6);

    /* Ids are unique and never 0. */
    hal_object_id b = hal_scene_object_add(s, "Sphere", HAL_OBJECT_MESH);
    HAL_TEST_CHECK(b != a);
    HAL_TEST_CHECK(b != HAL_OBJECT_ID_NONE);

    /* NULL name falls back. */
    hal_object_id c = hal_scene_object_add(s, NULL, HAL_OBJECT_CAMERA);
    HAL_TEST_CHECK(strcmp(hal_scene_object(s, c)->name, "Object") == 0);

    /* Remove: children re-parent to the removed object's parent. */
    hal_object_id child = hal_scene_object_add(s, "Child", HAL_OBJECT_MESH);
    HAL_TEST_CHECK(hal_object_set_parent(s, child, b));
    hal_scene_object_remove(s, b);
    HAL_TEST_CHECK(hal_scene_object(s, b) == NULL);
    HAL_TEST_CHECK(hal_scene_object(s, child)->parent == HAL_OBJECT_ID_NONE);

    /* Unknown ids are safe no-ops. */
    hal_scene_object_remove(s, 9999u);
    HAL_TEST_CHECK(hal_scene_object(s, 9999u) == NULL);

    hal_scene_destroy(s);
    hal_scene_destroy(NULL);
}

static void test_parenting_cycles(void) {
    hal_scene *s = hal_scene_create("Cycles");
    hal_object_id a = hal_scene_object_add(s, "A", HAL_OBJECT_MESH);
    hal_object_id b = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);
    hal_object_id c = hal_scene_object_add(s, "C", HAL_OBJECT_MESH);

    /* A -> B -> C chain. */
    HAL_TEST_CHECK(hal_object_set_parent(s, b, a));
    HAL_TEST_CHECK(hal_object_set_parent(s, c, b));

    /* Self-parent refused. */
    HAL_TEST_CHECK(!hal_object_set_parent(s, a, a));
    /* Cycles refused: A cannot parent under B or C. */
    HAL_TEST_CHECK(!hal_object_set_parent(s, a, b));
    HAL_TEST_CHECK(!hal_object_set_parent(s, a, c));
    /* Unknown parent refused. */
    HAL_TEST_CHECK(!hal_object_set_parent(s, c, 4242u));

    /* The tree is unchanged after the refusals. */
    HAL_TEST_CHECK(hal_scene_object(s, a)->parent == HAL_OBJECT_ID_NONE);
    HAL_TEST_CHECK(hal_scene_object(s, b)->parent == a);
    HAL_TEST_CHECK(hal_scene_object(s, c)->parent == b);

    hal_scene_destroy(s);
}

static void test_world_matrices(void) {
    hal_scene *s = hal_scene_create("World");

    hal_object_id parent = hal_scene_object_add(s, "Parent", HAL_OBJECT_MESH);
    hal_object_id child = hal_scene_object_add(s, "Child", HAL_OBJECT_MESH);
    hal_object_set_parent(s, child, parent);

    /* Parent moves to (10, 0, 0) and doubles its scale; the child sits
     * at local (1, 0, 0): world = (12, 0, 0). */
    hal_object_set_location(s, parent, hal_vec3f_make(10.0f, 0.0f, 0.0f));
    hal_object_set_scale(s, parent, hal_vec3f_make(2.0f, 2.0f, 2.0f));
    hal_object_set_location(s, child, hal_vec3f_make(1.0f, 0.0f, 0.0f));

    hal_object *ch = hal_scene_object(s, child);
    hal_mat4f w = hal_object_world_matrix(s, ch);
    HAL_TEST_CHECK_EQ_F(w.m[12], 12.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(w.m[13], 0.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(w.m[14], 0.0f, 1e-5);

    /* Dirty propagation: editing the parent refreshes the child. */
    hal_object_set_location(s, parent, hal_vec3f_make(20.0f, 0.0f, 0.0f));
    w = hal_object_world_matrix(s, ch);
    HAL_TEST_CHECK_EQ_F(w.m[12], 22.0f, 1e-5);

    /* Rotations compose: parent rotates 90 deg about Z; the child's
     * local +X offset becomes +Y in world space. */
    hal_object_set_rotation_euler(s, parent,
                                  hal_vec3f_make(0.0f, 0.0f, 90.0f * HAL_DEG2RAD_f32));
    w = hal_object_world_matrix(s, ch);
    HAL_TEST_CHECK_EQ_F(w.m[13], 2.0f, 1e-4); /* 2 * local x, rotated to +Y */
    HAL_TEST_CHECK_EQ_F(w.m[12], 20.0f, 1e-4);

    hal_scene_destroy(s);
}

static void test_selection(void) {
    hal_scene *s = hal_scene_create("Sel");
    hal_object_id a = hal_scene_object_add(s, "A", HAL_OBJECT_MESH);
    hal_object_id b = hal_scene_object_add(s, "B", HAL_OBJECT_MESH);

    HAL_TEST_CHECK_EQ_I(hal_scene_selected(s), HAL_OBJECT_ID_NONE);
    hal_scene_select_only(s, a);
    HAL_TEST_CHECK_EQ_I(hal_scene_selected(s), a);
    HAL_TEST_CHECK(hal_scene_object(s, a)->selected);
    HAL_TEST_CHECK(!hal_scene_object(s, b)->selected);

    hal_scene_select_only(s, b);
    HAL_TEST_CHECK_EQ_I(hal_scene_selected(s), b);

    hal_object_set_visible(s, a, false);
    HAL_TEST_CHECK(!hal_scene_object(s, a)->visible);

    hal_scene_destroy(s);
}

int main(void) {
    test_lifecycle();
    test_parenting_cycles();
    test_world_matrices();
    test_selection();
    HAL_TEST_SUMMARY("test_scene");
}
