/*
 * widget.c — widget tree core: creation, destruction, common API,
 * hit-testing, absolute-rect propagation
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

static char *dup_or_empty(const char *src) {
    const char *s = src ? src : "";
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out)
        memcpy(out, s, n);
    return out;
}

static void widget_append(hal_widget *parent, hal_widget *w) {
    w->parent = parent;
    if (!parent)
        return;
    if (parent->last_child) {
        parent->last_child->next = w;
        w->prev = parent->last_child;
        parent->last_child = w;
    } else {
        parent->first_child = w;
        parent->last_child = w;
    }
}

hal_widget *hal__widget_create(hal_ui *ui, hal_widget *parent,
                               hal_widget_kind kind) {
    if (!ui)
        return NULL;
    hal_widget *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->ui = ui;
    w->kind = kind;
    w->visible = true;
    w->text = dup_or_empty(NULL);
    if (!w->text) {
        free(w);
        return NULL;
    }
    widget_append(parent, w);
    if (ui)
        ui->needs_paint = true;
    return w;
}

static void menubar_data_free(hal_widget *w);

void hal__widget_destroy_tree(hal_widget *w) {
    if (!w)
        return;
    hal_widget *child = w->first_child;
    while (child) {
        hal_widget *next = child->next;
        hal__widget_destroy_tree(child);
        child = next;
    }
    if (w->kind == HAL_WIDGET_MENUBAR)
        menubar_data_free(w);
    if (w->kind == HAL_WIDGET_TREE)
        hal__tree_clear_rows(w);
    if (w->ui) {
        if (w->ui->focus == w)
            w->ui->focus = NULL;
        if (w->ui->grab == w)
            w->ui->grab = NULL;
        if (w->ui->press_target == w)
            w->ui->press_target = NULL;
        if (w->ui->hover == w)
            w->ui->hover = NULL;
        if (w->ui->open_menubar == w) {
            w->ui->open_menubar = NULL;
            w->ui->open_menu_index = -1;
        }
    }
    free(w->text);
    free(w);
}

/* ---- Common API ---- */

void hal_widget_set_text(hal_widget *w, const char *text) {
    if (!w)
        return;
    char *owned = dup_or_empty(text);
    if (!owned)
        return;
    free(w->text);
    w->text = owned;
    if (w->ui)
        w->ui->needs_paint = true;
}

const char *hal_widget_text(const hal_widget *w) {
    return w ? w->text : "";
}

void hal_widget_set_user(hal_widget *w, void *user_data) {
    if (w)
        w->user_data = user_data;
}

void *hal_widget_user(hal_widget *w) {
    return w ? w->user_data : NULL;
}

hal_widget_kind hal_widget_kind_of(const hal_widget *w) {
    return w ? w->kind : HAL_WIDGET_ROOT;
}

hal_widget *hal_widget_parent(const hal_widget *w) {
    return w ? w->parent : NULL;
}

hal_widget *hal_widget_first_child(const hal_widget *w) {
    return w ? w->first_child : NULL;
}

hal_widget *hal_widget_next_sibling(const hal_widget *w) {
    return w ? w->next : NULL;
}

void hal_widget_set_rect(hal_widget *w, hal_rectf rect) {
    if (!w)
        return;
    w->rect = rect;
    if (w->ui)
        w->ui->needs_paint = true;
}

hal_rectf hal_widget_rect(const hal_widget *w) {
    if (!w)
        return (hal_rectf){ 0, 0, 0, 0 };
    return w->rect;
}

hal_rectf hal_widget_abs_rect(const hal_widget *w) {
    if (!w)
        return (hal_rectf){ 0, 0, 0, 0 };
    return w->abs_rect;
}

void hal_widget_set_visible(hal_widget *w, bool visible) {
    if (!w || w->visible == visible)
        return;
    w->visible = visible;
    if (w->ui)
        w->ui->needs_paint = true;
}

bool hal_widget_visible(const hal_widget *w) {
    return w ? w->visible : false;
}

bool hal_widget_hovered(const hal_widget *w) {
    return w ? w->hovered : false;
}

void hal_widget_set_expand(hal_widget *w, bool expand) {
    if (w)
        w->expand = expand;
}

void hal_widget_set_fixed_size(hal_widget *w, hal_f32 width,
                               hal_f32 height) {
    if (!w)
        return;
    w->measured.width = width > 0.0f ? width : w->measured.width;
    w->measured.height = height > 0.0f ? height : w->measured.height;
}

hal_sizef hal_widget_measured(const hal_widget *w) {
    if (!w)
        return (hal_sizef){ 0, 0 };
    return w->measured;
}

/* ---- Absolute rects ----
 * Fills abs_rect for the whole tree from the root's own rect (which
 * the shell keeps equal to the window). Called from hal_ui_layout
 * before arrange so hit-testing works against final geometry. */

void hal__widget_walk_absolute(hal_widget *w, hal_f32 off_x, hal_f32 off_y) {
    if (!w)
        return;
    w->abs_rect.x = off_x + w->rect.x;
    w->abs_rect.y = off_y + w->rect.y;
    w->abs_rect.width = w->rect.width;
    w->abs_rect.height = w->rect.height;
    if (w->kind == HAL_WIDGET_MENUBAR)
        hal__menubar_arrange_items(w);
    for (hal_widget *c = w->first_child; c; c = c->next)
        hal__widget_walk_absolute(c, w->abs_rect.x, w->abs_rect.y);
}

/* ---- Hit testing ----
 * Deepest visible child whose rect contains the point; the walk is
 * front-to-back in child order (later children draw on top, so they
 * win the hit test too — the paint and hit orders agree). */

hal_widget *hal__hit_test(hal_widget *w, hal_f32 x, hal_f32 y) {
    if (!w || !w->visible)
        return NULL;
    hal_rectf r = w->abs_rect;
    if (x < r.x || y < r.y || x >= r.x + r.width || y >= r.y + r.height)
        return NULL;

    hal_widget *hit = NULL;
    for (hal_widget *c = w->last_child; c; c = c->prev) {
        hit = hal__hit_test(c, x, y);
        if (hit)
            break;
    }
    if (hit)
        return hit;

    /* Containers (root, rows, columns, panels, tabs) hit-test their
     * children but do not "consume" the point themselves — the
     * application gets unconsumed events over empty areas. */
    switch (w->kind) {
    case HAL_WIDGET_ROOT:
    case HAL_WIDGET_ROW:
    case HAL_WIDGET_COLUMN:
    case HAL_WIDGET_TABS:
        return NULL;
    case HAL_WIDGET_PANEL:
        /* The panel's chrome (title band) is the panel; the client
         * area belongs to the child (or falls through when empty). */
        return hit; /* hit is NULL here */
    default:
        return w;
    }
}

/* ---- Menu bar storage (user_data on the menubar widget) ---- */

static void menubar_data_free(hal_widget *w) {
    hal_menubar_data *data = w->user_data;
    if (!data)
        return;
    for (size_t m = 0; m < data->count; m++) {
        for (size_t i = 0; i < data->menus[m].count; i++) {
            free(data->menus[m].items[i].text);
            free(data->menus[m].items[i].shortcut);
        }
        free(data->menus[m].items);
        free(data->menus[m].title);
    }
    free(data->menus);
    free(data);
    w->user_data = NULL;
}
