/*
 * undo.h — Halcyon's undo/redo
 *
 * Command pattern over the scene. A command owns an opaque state blob
 * plus the pair of functions that transition the document forward
 * (apply — redo) and backward (revert — undo).
 *
 * The contract (decision D-006):
 *
 *   - The CALLER performs the forward action first, then pushes the
 *     command describing it. push() does NOT call apply — the document
 *     is already in the post-state. This avoids double-apply and keeps
 *     command construction trivial (capture old + new, revert sets old,
 *     apply sets new).
 *   - undo() runs the top command's revert; redo() re-runs its apply.
 *     Both use an idempotent "set to the captured value" style, so
 *     replay order artifacts cannot accumulate.
 *   - A new push clears the redo stack (the universal rule).
 *   - The stack has a bounded depth; the oldest commands fall off.
 *     A command that falls off or the stack dies is freed through its
 *     free_state — no leaks by construction.
 *
 * `user_ctx` is whatever the application hands the stack at creation
 * (typically the editor document); it travels to every apply/revert.
 */

#ifndef HAL_UNDO_H
#define HAL_UNDO_H

#include "../platform/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_undo_stack hal_undo_stack;

/* apply/revert receive (state, user_ctx). */
typedef void (*hal_undo_fn)(void *state, void *user_ctx);
typedef void (*hal_undo_free_fn)(void *state);

/* Creates a stack with the given capacity (commands beyond it are
 * evicted oldest-first; 0 = unbounded). user_ctx travels to every
 * apply/revert call. */
hal_undo_stack *hal_undo_stack_create(void *user_ctx, size_t limit);

/* Frees the stack and every remaining command's state. */
void hal_undo_stack_destroy(hal_undo_stack *stack);

/* Pushes a command. Takes ownership of `state` (freed with free_state
 * when evicted/undone-off/destroyed — never by the caller again).
 * `label` is copied (shown by the UI's Edit menu); NULL = "".
 * Returns false only on allocation failure (state is freed then).
 *
 * The caller must have ALREADY performed the forward action. */
bool hal_undo_stack_push(hal_undo_stack *stack, const char *label,
                         hal_undo_fn apply, hal_undo_fn revert,
                         hal_undo_free_fn free_state, void *state);

/* Runs the newest command's revert. False when there is nothing to
 * undo. */
bool hal_undo_stack_undo(hal_undo_stack *stack);

/* Re-runs the most recently undone command's apply. False when there
 * is nothing to redo. */
bool hal_undo_stack_redo(hal_undo_stack *stack);

bool hal_undo_stack_can_undo(const hal_undo_stack *stack);
bool hal_undo_stack_can_redo(const hal_undo_stack *stack);

/* The label of the next undo / redo action (for menu items); NULL
 * when there is none. */
const char *hal_undo_stack_undo_label(const hal_undo_stack *stack);
const char *hal_undo_stack_redo_label(const hal_undo_stack *stack);

/* Number of undoable commands on the stack. */
size_t hal_undo_stack_depth(const hal_undo_stack *stack);

/* Drops everything (both directions). */
void hal_undo_stack_clear(hal_undo_stack *stack);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UNDO_H */
