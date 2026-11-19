/*
 * test_undo.c — undo/redo stack tests
 *
 * The set/restore contract: push after mutation, undo restores,
 * redo replays, a new push clears redo, limits evict oldest, and no
 * state leaks across any path (ASan holds us to that).
 */

#include "hal_test.h"
#include "core/undo.h"

#include <stdlib.h>
#include <string.h>

/* Test document: a single int the commands set. */
typedef struct int_doc {
    int value;
    int apply_calls;
    int revert_calls;
} int_doc;

typedef struct int_change {
    int old_value;
    int new_value;
} int_change;

static void change_apply(void *state, void *ctx) {
    int_change *c = state;
    int_doc *doc = ctx;
    doc->value = c->new_value;
    doc->apply_calls++;
}

static void change_revert(void *state, void *ctx) {
    int_change *c = state;
    int_doc *doc = ctx;
    doc->value = c->old_value;
    doc->revert_calls++;
}

static void change_free(void *state) {
    free(state);
}

static void test_basic(void) {
    int_doc doc = { 0, 0, 0 };
    hal_undo_stack *stack = hal_undo_stack_create(&doc, 0);
    HAL_TEST_CHECK(stack != NULL);
    HAL_TEST_CHECK(!hal_undo_stack_can_undo(stack));

    /* Edit 1: set value to 10 (caller mutates, then pushes). */
    int_change *c1 = malloc(sizeof(*c1));
    c1->old_value = 0;
    c1->new_value = 10;
    doc.value = 10;
    HAL_TEST_CHECK(hal_undo_stack_push(stack, "Set 10", change_apply,
                                       change_revert, change_free, c1));
    HAL_TEST_CHECK(hal_undo_stack_can_undo(stack));
    HAL_TEST_CHECK(!hal_undo_stack_can_redo(stack));
    HAL_TEST_CHECK_EQ_I(hal_undo_stack_depth(stack), 1);
    HAL_TEST_CHECK(strcmp(hal_undo_stack_undo_label(stack), "Set 10") == 0);

    /* Edit 2: set value to 20. */
    int_change *c2 = malloc(sizeof(*c2));
    c2->old_value = 10;
    c2->new_value = 20;
    doc.value = 20;
    HAL_TEST_CHECK(hal_undo_stack_push(stack, "Set 20", change_apply,
                                       change_revert, change_free, c2));
    HAL_TEST_CHECK_EQ_I(hal_undo_stack_depth(stack), 2);

    /* Undo both, redo both — the value walks back and forth. */
    HAL_TEST_CHECK(hal_undo_stack_undo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 10);
    HAL_TEST_CHECK(hal_undo_stack_undo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 0);
    HAL_TEST_CHECK(!hal_undo_stack_undo(stack)); /* nothing left */

    HAL_TEST_CHECK(hal_undo_stack_can_redo(stack));
    HAL_TEST_CHECK(strcmp(hal_undo_stack_redo_label(stack), "Set 10") == 0);
    HAL_TEST_CHECK(hal_undo_stack_redo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 10);
    HAL_TEST_CHECK(hal_undo_stack_redo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 20);
    HAL_TEST_CHECK(!hal_undo_stack_redo(stack));

    hal_undo_stack_destroy(stack);
}

static void test_redo_cleared_on_push(void) {
    int_doc doc = { 0, 0, 0 };
    hal_undo_stack *stack = hal_undo_stack_create(&doc, 0);

    for (int round = 1; round <= 3; round++) {
        int_change *c = malloc(sizeof(*c));
        c->old_value = doc.value;
        c->new_value = round * 100;
        doc.value = c->new_value;
        HAL_TEST_CHECK(hal_undo_stack_push(stack, "edit", change_apply,
                                           change_revert, change_free, c));
    }

    HAL_TEST_CHECK(hal_undo_stack_undo(stack)); /* 300 -> 200 */
    HAL_TEST_CHECK_EQ_I(doc.value, 200);
    HAL_TEST_CHECK(hal_undo_stack_can_redo(stack));

    /* A new edit invalidates the redo branch. */
    int_change *c = malloc(sizeof(*c));
    c->old_value = doc.value;
    c->new_value = 7;
    doc.value = 7;
    HAL_TEST_CHECK(hal_undo_stack_push(stack, "branch", change_apply,
                                       change_revert, change_free, c));

    HAL_TEST_CHECK(!hal_undo_stack_can_redo(stack));
    HAL_TEST_CHECK(!hal_undo_stack_redo(stack));
    HAL_TEST_CHECK_EQ_I(hal_undo_stack_depth(stack), 3);

    hal_undo_stack_destroy(stack);
}

static void test_limit(void) {
    int_doc doc = { 0, 0, 0 };
    hal_undo_stack *stack = hal_undo_stack_create(&doc, 3);

    for (int i = 1; i <= 5; i++) {
        int_change *c = malloc(sizeof(*c));
        c->old_value = doc.value;
        c->new_value = i;
        doc.value = i;
        HAL_TEST_CHECK(hal_undo_stack_push(stack, "e", change_apply,
                                           change_revert, change_free, c));
    }

    HAL_TEST_CHECK_EQ_I(hal_undo_stack_depth(stack), 3);

    /* The oldest two commands (0->1, 1->2) fell off: the survivors are
     * (2,3), (3,4), (4,5). Three undos walk 5 -> 4 -> 3 -> 2, then the
     * stack is empty (the floor is 2 — the earliest state still known). */
    HAL_TEST_CHECK(hal_undo_stack_undo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 4);
    HAL_TEST_CHECK(hal_undo_stack_undo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 3);
    HAL_TEST_CHECK(hal_undo_stack_undo(stack));
    HAL_TEST_CHECK_EQ_I(doc.value, 2);
    HAL_TEST_CHECK(!hal_undo_stack_can_undo(stack));

    hal_undo_stack_destroy(stack);
}

static void test_clear_and_labels(void) {
    int_doc doc = { 0, 0, 0 };
    hal_undo_stack *stack = hal_undo_stack_create(&doc, 0);

    HAL_TEST_CHECK(hal_undo_stack_undo_label(stack) == NULL);
    HAL_TEST_CHECK(hal_undo_stack_redo_label(stack) == NULL);

    int_change *c = malloc(sizeof(*c));
    c->old_value = 0;
    c->new_value = 1;
    doc.value = 1;
    HAL_TEST_CHECK(hal_undo_stack_push(stack, NULL, change_apply,
                                       change_revert, change_free, c));
    /* NULL label reads back as "". */
    HAL_TEST_CHECK(hal_undo_stack_undo_label(stack) != NULL);
    HAL_TEST_CHECK(strcmp(hal_undo_stack_undo_label(stack), "") == 0);

    hal_undo_stack_clear(stack);
    HAL_TEST_CHECK(!hal_undo_stack_can_undo(stack));
    HAL_TEST_CHECK_EQ_I(hal_undo_stack_depth(stack), 0);

    /* push with NULL functions is refused and the state is freed. */
    int_change *bad = malloc(sizeof(*bad));
    HAL_TEST_CHECK(!hal_undo_stack_push(stack, "bad", NULL, change_revert,
                                        change_free, bad));

    hal_undo_stack_destroy(stack);
    hal_undo_stack_destroy(NULL);
}

int main(void) {
    test_basic();
    test_redo_cleared_on_push();
    test_limit();
    test_clear_and_labels();
    HAL_TEST_SUMMARY("test_undo");
}
