/*
 * tree.c — the tree view widget (the outliner's substrate)
 *
 * Rows are application data (hal_tree_add/clear); the tree measures,
 * lays out, paints and hit-tests them. Rows show an expander triangle
 * when they have children; clicking a row selects it, clicking the
 * triangle toggles expansion.
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

static hal_f32 row_height(hal_ui *ui) {
    /* Real metrics: a taller face must never be clipped by its own
     * row. */
    return hal__text_row_h(ui, ui->theme->font_size_small) + 10.0f;
}

static size_t count_visible_rows(hal_tree_row *row, bool *overflow) {
    size_t n = 0;
    for (; row; row = row->next) {
        n++;
        if (n > 100000) {
            *overflow = true;
            return n;
        }
        if (row->expanded && row->first_child)
            n += count_visible_rows(row->first_child, overflow);
        if (*overflow)
            return n;
    }
    return n;
}

void hal__tree_measure(hal_widget *w) {
    if (!w->ui)
        return;
    bool overflow = false;
    size_t n = count_visible_rows(w->s.tree.roots, &overflow);
    w->s.tree.content_height = (hal_f32)n * row_height(w->ui);
    w->measured.width = 140.0f;
    w->measured.height = w->s.tree.content_height;
}

static void free_rows(hal_tree_row *row) {
    while (row) {
        hal_tree_row *next = row->next;
        free_rows(row->first_child);
        free(row->text);
        free(row);
        row = next;
    }
}

void hal__tree_clear_rows(hal_widget *w) {
    free_rows(w->s.tree.roots);
    w->s.tree.roots = NULL;
    w->s.tree.last_root = NULL;
    w->s.tree.selected = NULL;
}

hal_tree_row *hal_tree_add(hal_widget *tree, hal_tree_row *parent_row,
                           const char *text, void *user_data) {
    if (!tree || tree->kind != HAL_WIDGET_TREE || !text)
        return NULL;

    hal_tree_row *row = calloc(1, sizeof(*row));
    if (!row)
        return NULL;
    row->text = malloc(strlen(text) + 1);
    if (!row->text) {
        free(row);
        return NULL;
    }
    strcpy(row->text, text);
    row->user_data = user_data;
    row->expanded = true;

    if (parent_row) {
        row->parent = parent_row;
        row->depth = parent_row->depth + 1;
        if (parent_row->last_child) {
            parent_row->last_child->next = row;
            row->prev = parent_row->last_child;
            parent_row->last_child = row;
        } else {
            parent_row->first_child = row;
            parent_row->last_child = row;
        }
    } else {
        if (tree->s.tree.last_root) {
            tree->s.tree.last_root->next = row;
            row->prev = tree->s.tree.last_root;
            tree->s.tree.last_root = row;
        } else {
            tree->s.tree.roots = row;
            tree->s.tree.last_root = row;
        }
    }

    if (tree->ui)
        tree->ui->needs_paint = true;
    return row;
}

void hal_tree_clear(hal_widget *tree) {
    if (!tree || tree->kind != HAL_WIDGET_TREE)
        return;
    hal__tree_clear_rows(tree);
    if (tree->ui)
        tree->ui->needs_paint = true;
}

hal_tree_row *hal_tree_selected(const hal_widget *tree) {
    if (!tree || tree->kind != HAL_WIDGET_TREE)
        return NULL;
    return tree->s.tree.selected;
}

hal_tree_row *hal_tree_row_next(const hal_widget *tree, hal_tree_row *row) {
    (void)tree; /* rows link themselves; the tree is not needed */
    if (!row)
        return NULL;
    return row->next;
}

const char *hal_tree_row_text(const hal_tree_row *row) {
    return row ? row->text : NULL;
}

void *hal_tree_row_user(const hal_tree_row *row) {
    return row ? row->user_data : NULL;
}

bool hal_tree_row_expanded(const hal_tree_row *row) {
    return row ? row->expanded : false;
}

/* Assigns y positions to visible rows; returns the next y. */
static hal_f32 layout_rows(hal_tree_row *row, hal_f32 y, hal_f32 rh) {
    for (; row; row = row->next) {
        row->y = y;
        y += rh;
        if (row->expanded && row->first_child)
            y = layout_rows(row->first_child, y, rh);
    }
    return y;
}

void hal__tree_layout_rows(hal_widget *w) {
    layout_rows(w->s.tree.roots, w->abs_rect.y, row_height(w->ui));
}

/* A flat cursor over VISIBLE rows (respecting expansion), in y order:
 * yields each row once; used by hit-testing and painting. */

typedef struct row_cursor {
    hal_tree_row *stack[64];
    int depth;
    hal_f32 next_y; /* not used; rows carry y from layout_rows */
} row_cursor;

static void row_cursor_begin(row_cursor *cur, hal_widget *w) {
    cur->depth = 0;
    if (w->s.tree.roots) {
        cur->stack[cur->depth++] = w->s.tree.roots;
    }
}

static hal_tree_row *row_cursor_next(row_cursor *cur) {
    while (cur->depth > 0) {
        hal_tree_row *row = cur->stack[cur->depth - 1];
        /* Advance the stack top to its sibling. */
        cur->stack[cur->depth - 1] = row->next;
        if (!cur->stack[cur->depth - 1])
            cur->depth--;
        /* Descend into expanded children first (they follow in y). */
        if (row->expanded && row->first_child &&
            cur->depth < 64) {
            cur->stack[cur->depth++] = row->first_child;
        }
        if (!row->expanded && row->first_child) {
            /* Children are invisible; skip them entirely (the cursor
             * never descended). */
        }
        return row;
    }
    return NULL;
}

/* The visible row at window-y, or NULL. */
hal_tree_row *hal__tree_row_at(hal_widget *w, hal_f32 y) {
    hal_f32 rh = row_height(w->ui);
    hal__tree_layout_rows(w);
    row_cursor cur;
    row_cursor_begin(&cur, w);
    hal_tree_row *row;
    while ((row = row_cursor_next(&cur)) != NULL) {
        if (y >= row->y && y < row->y + rh)
            return row;
    }
    return NULL;
}

/* ---- Paint ---- */

static void paint_row(hal_ui *ui, hal_widget *w, hal_tree_row *row,
                      hal_canvas *canvas, hal_font *font,
                      hal_font_metrics *fm, hal_f32 rh) {
    const hal_theme *t = ui->theme;

    hal_rectf row_rect = { w->abs_rect.x, row->y, w->abs_rect.width, rh };
    if (row_rect.y + rh < w->abs_rect.y ||
        row_rect.y > w->abs_rect.y + w->abs_rect.height)
        return; /* clipped: outside the widget */

    if (row == w->s.tree.selected)
        hal_canvas_fill_rect(canvas, row_rect,
                             t->colors[HAL_THEME_SELECT]);

    hal_f32 indent = 6.0f + (hal_f32)row->depth * 14.0f;
    hal_f32 text_x = w->abs_rect.x + indent + 16.0f;
    hal_f32 baseline = hal__baseline(row_rect.y, rh, fm, w->ui->scale);

    if (row->first_child) {
        hal_f32 cx = w->abs_rect.x + indent + 6.0f;
        hal_f32 cy = row_rect.y + rh * 0.5f;
        hal_f32 r = 3.5f;
        hal_pointf a, b, c;
        if (row->expanded) {
            a = (hal_pointf){ cx - r, cy - r * 0.7f };
            b = (hal_pointf){ cx + r, cy - r * 0.7f };
            c = (hal_pointf){ cx, cy + r };
        } else {
            a = (hal_pointf){ cx - r * 0.7f, cy - r };
            b = (hal_pointf){ cx - r * 0.7f, cy + r };
            c = (hal_pointf){ cx + r, cy };
        }
        hal_canvas_fill_triangle(canvas, a, b, c,
                                 t->colors[HAL_THEME_TEXT_DIM]);
    }

    hal_canvas_draw_text(canvas, font, row->text, strlen(row->text), text_x,
                         baseline,
                         row == w->s.tree.selected
                             ? t->colors[HAL_THEME_TEXT]
                             : t->colors[HAL_THEME_TEXT]);
}

void hal__tree_paint(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_f32 rh = row_height(w->ui);
    hal_font *font = hal__ui_font(ui, t->font_size_small);
    if (!font)
        return;

    hal_font_metrics fm;
    hal_font_get_metrics(font, &fm);

    /* Rows are laid out at paint time from the widget's abs rect. */
    hal__tree_layout_rows(w);

    row_cursor cur;
    row_cursor_begin(&cur, w);
    hal_tree_row *row;
    while ((row = row_cursor_next(&cur)) != NULL)
        paint_row(ui, w, row, canvas, font, &fm, rh);
}

hal_rectf hal_tree_row_rect(hal_widget *tree, const hal_tree_row *row) {
    if (!tree || tree->kind != HAL_WIDGET_TREE || !row)
        return (hal_rectf){ 0, 0, 0, 0 };
    hal__tree_layout_rows(tree);
    hal_f32 rh = row_height(tree->ui);
    return (hal_rectf){ tree->abs_rect.x, row->y, tree->abs_rect.width, rh };
}

hal_widget *hal_tree_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_TREE);
    if (!w)
        return NULL;
    w->s.tree.roots = NULL;
    w->s.tree.last_root = NULL;
    w->s.tree.selected = NULL;
    return w;
}

void hal_tree_set_on_select(hal_widget *tree, hal_tree_select_fn on_select,
                            void *user_data) {
    if (!tree || tree->kind != HAL_WIDGET_TREE)
        return;
    tree->s.tree.on_select = on_select;
    tree->s.tree.on_select_user = user_data;
}
