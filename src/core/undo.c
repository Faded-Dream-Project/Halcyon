/*
 * undo.c — Halcyon undo/redo implementation
 */

#include "undo.h"

#include <stdlib.h>
#include <string.h>

typedef struct hal_undo_cmd {
    char *label;
    hal_undo_fn apply;
    hal_undo_fn revert;
    hal_undo_free_fn free_state;
    void *state;
} hal_undo_cmd;

struct hal_undo_stack {
    hal_undo_cmd *done;    /* oldest .. newest                    */
    size_t done_count;
    size_t done_capacity;
    hal_undo_cmd *redo;    /* newest-undone .. oldest-undone      */
    size_t redo_count;
    size_t redo_capacity;
    size_t limit;          /* 0 = unbounded done stack            */
    void *user_ctx;
};

static void cmd_free(hal_undo_cmd *cmd) {
    if (cmd->free_state && cmd->state)
        cmd->free_state(cmd->state);
    free(cmd->label);
}

hal_undo_stack *hal_undo_stack_create(void *user_ctx, size_t limit) {
    hal_undo_stack *stack = calloc(1, sizeof(*stack));
    if (!stack)
        return NULL;
    stack->limit = limit;
    stack->user_ctx = user_ctx;
    return stack;
}

void hal_undo_stack_destroy(hal_undo_stack *stack) {
    if (!stack)
        return;
    for (size_t i = 0; i < stack->done_count; i++)
        cmd_free(&stack->done[i]);
    for (size_t i = 0; i < stack->redo_count; i++)
        cmd_free(&stack->redo[i]);
    free(stack->done);
    free(stack->redo);
    free(stack);
}

static void redo_clear(hal_undo_stack *stack) {
    for (size_t i = 0; i < stack->redo_count; i++)
        cmd_free(&stack->redo[i]);
    stack->redo_count = 0;
}

bool hal_undo_stack_push(hal_undo_stack *stack, const char *label,
                         hal_undo_fn apply, hal_undo_fn revert,
                         hal_undo_free_fn free_state, void *state) {
    if (!stack || !apply || !revert) {
        if (free_state && state)
            free_state(state);
        return false;
    }

    char *owned_label = NULL;
    if (label) {
        size_t n = strlen(label) + 1;
        owned_label = malloc(n);
        if (!owned_label) {
            if (free_state && state)
                free_state(state);
            return false;
        }
        memcpy(owned_label, label, n);
    }

    if (stack->done_count == stack->done_capacity) {
        size_t cap = stack->done_capacity ? stack->done_capacity * 2 : 16;
        hal_undo_cmd *grown = realloc(stack->done, cap * sizeof(*grown));
        if (!grown) {
            free(owned_label);
            if (free_state && state)
                free_state(state);
            return false;
        }
        stack->done = grown;
        stack->done_capacity = cap;
    }

    /* Evict the oldest when the bounded limit is hit. */
    if (stack->limit > 0 && stack->done_count == stack->limit) {
        cmd_free(&stack->done[0]);
        memmove(&stack->done[0], &stack->done[1],
                (stack->done_count - 1) * sizeof(hal_undo_cmd));
        stack->done_count--;
    }

    stack->done[stack->done_count++] = (hal_undo_cmd){
        .label = owned_label,
        .apply = apply,
        .revert = revert,
        .free_state = free_state,
        .state = state,
    };

    redo_clear(stack);
    return true;
}

bool hal_undo_stack_undo(hal_undo_stack *stack) {
    if (!stack || stack->done_count == 0)
        return false;

    hal_undo_cmd *cmd = &stack->done[stack->done_count - 1];

    if (stack->redo_count == stack->redo_capacity) {
        size_t cap = stack->redo_capacity ? stack->redo_capacity * 2 : 16;
        hal_undo_cmd *grown = realloc(stack->redo, cap * sizeof(*grown));
        if (!grown)
            return false; /* not undone; the command stays */
        stack->redo = grown;
        stack->redo_capacity = cap;
    }

    cmd->revert(cmd->state, stack->user_ctx);

    /* Move to the redo stack (prepend: newest-undone first). */
    memmove(&stack->redo[1], &stack->redo[0],
            stack->redo_count * sizeof(hal_undo_cmd));
    stack->redo[0] = *cmd;
    stack->redo_count++;
    stack->done_count--;
    return true;
}

bool hal_undo_stack_redo(hal_undo_stack *stack) {
    if (!stack || stack->redo_count == 0)
        return false;

    hal_undo_cmd *cmd = &stack->redo[0];
    cmd->apply(cmd->state, stack->user_ctx);

    if (stack->done_count == stack->done_capacity) {
        size_t cap = stack->done_capacity ? stack->done_capacity * 2 : 16;
        hal_undo_cmd *grown = realloc(stack->done, cap * sizeof(*grown));
        if (!grown)
            return false; /* applied but not re-pushed: refuse loudly by
                            returning false — the state machine stays
                            consistent because redo was consumed below */
        stack->done = grown;
        stack->done_capacity = cap;
    }

    stack->done[stack->done_count++] = *cmd;
    memmove(&stack->redo[0], &stack->redo[1],
            (stack->redo_count - 1) * sizeof(hal_undo_cmd));
    stack->redo_count--;
    return true;
}

bool hal_undo_stack_can_undo(const hal_undo_stack *stack) {
    return stack && stack->done_count > 0;
}

bool hal_undo_stack_can_redo(const hal_undo_stack *stack) {
    return stack && stack->redo_count > 0;
}

const char *hal_undo_stack_undo_label(const hal_undo_stack *stack) {
    if (!stack || stack->done_count == 0)
        return NULL;
    return stack->done[stack->done_count - 1].label
               ? stack->done[stack->done_count - 1].label
               : "";
}

const char *hal_undo_stack_redo_label(const hal_undo_stack *stack) {
    if (!stack || stack->redo_count == 0)
        return NULL;
    return stack->redo[0].label ? stack->redo[0].label : "";
}

size_t hal_undo_stack_depth(const hal_undo_stack *stack) {
    return stack ? stack->done_count : 0;
}

void hal_undo_stack_clear(hal_undo_stack *stack) {
    if (!stack)
        return;
    redo_clear(stack);
    for (size_t i = 0; i < stack->done_count; i++)
        cmd_free(&stack->done[i]);
    stack->done_count = 0;
}
