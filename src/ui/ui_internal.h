/*
 * ui_internal.h — shared internals of the UI module
 *
 * The widget struct, the UI object's private state, and the internal
 * cross-file seams. Nothing here is included outside src/ui/.
 */

#ifndef HAL_UI_INTERNAL_H
#define HAL_UI_INTERNAL_H

#include "ui.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- Tree rows (hal_tree view) ---- */

struct hal_tree_row {
    char *text;
    void *user_data;
    hal_tree_row *parent;
    hal_tree_row *first_child, *last_child, *next, *prev;
    bool expanded;
    bool selected;
    int depth;
    hal_f32 y; /* absolute row top, computed at paint/layout time */
};

/* ---- Widgets ---- */

typedef union hal_widget_state {
    struct {
        void (*on_click)(hal_widget *w, void *user);
        void *user_data;
    } button;
    struct {
        bool *bound;
    } toggle;
    struct {
        hal_f32 min, max;
        hal_f32 *value;
        bool dragging;
    } slider;
    struct {
        char *buf;
        size_t cap;
        size_t caret; /* byte offset into buf */
        void (*on_change)(hal_widget *entry, void *user);
        void *on_change_user;
    } entry;
    struct {
        bool vertical; /* column vs row */
    } box;
    struct {
        const hal_mesh *mesh; /* borrowed */
    } uvview;
    struct {
        int active;
    } tabs;
    struct {
        hal_tree_row *roots;   /* first root row                    */
        hal_tree_row *last_root;
        hal_tree_row *selected;
        hal_f32 content_height;
        void (*on_select)(hal_widget *tree, hal_tree_row *row,
                          void *user);
        void *on_select_user;
    } tree;
} hal_widget_state;

struct hal_widget {
    hal_ui *ui;
    hal_widget_kind kind;
    hal_widget *parent;
    hal_widget *first_child, *last_child, *next, *prev;

    hal_rectf rect;     /* parent-relative, logical                  */
    hal_rectf abs_rect; /* window-logical (valid after layout)       */
    hal_sizef measured; /* measured min size                         */

    bool visible;
    bool hovered;
    bool pressed;
    bool expand;        /* share leftover space in a row/column      */

    char *text;         /* owned; label/button/tab/menu-bar item     */
    void *user_data;

    hal_widget_state s;
};

/* ---- Menus (data on the menubar widget, via user_data) ---- */

typedef struct hal_menu_item {
    char *text;       /* NULL = separator                          */
    char *shortcut;   /* owned; may be NULL                        */
    void (*on_activate)(hal_widget *menubar, int menu, int item,
                        void *user);
    void *user_data;
} hal_menu_item;

typedef struct hal_menu {
    char *title;
    hal_menu_item *items;
    size_t count, capacity;
    hal_rectf item_rect; /* the bar item's abs rect (layout)        */
} hal_menu;

typedef struct hal_menubar_data {
    hal_menu *menus;
    size_t count, capacity;
    int open_menu; /* -1 = closed */
} hal_menubar_data;

/* ---- The UI object ---- */

#define HAL_UI_FONT_CACHE_MAX 16

struct hal_ui {
    hal_theme *theme;
    hal_widget *root;

    hal_widget *focus;      /* keyboard focus (entry)                */
    hal_widget *grab;       /* implicit pointer grab (slider drag)   */
    hal_widget *hover;      /* deepest hovered widget                */
    hal_widget *press_target; /* button/toggle held down             */

    hal_widget *open_menubar; /* the menubar with an open menu      */
    int open_menu_index;
    int menu_hover_item;   /* item index under the pointer           */
    hal_rectf menu_popup_rect;

    bool needs_paint;
    hal_f32 scale;         /* the scale layout measured with         */
    hal_pointf last_pointer; /* window-logical, from motion events   */

    /* Font cache: logical px -> font loaded at px * scale. */
    struct {
        hal_i32 px;
        hal_f32 scale;
        hal_font *font;
    } fonts[HAL_UI_FONT_CACHE_MAX];
    int font_count;
};

/* ---- Cross-file seams ---- */

/* widget.c */
hal_widget *hal__widget_create(hal_ui *ui, hal_widget *parent,
                               hal_widget_kind kind);
void hal__widget_destroy_tree(hal_widget *w);
hal_widget *hal__hit_test(hal_widget *w, hal_f32 x, hal_f32 y);
void hal__widget_walk_absolute(hal_widget *w, hal_f32 off_x, hal_f32 off_y);

/* font.c: the font cache (ui-owned). */
hal_font *hal__ui_font(hal_ui *ui, hal_i32 logical_px);

/* layout.c */
void hal__widget_measure(hal_widget *w);
void hal__widget_arrange(hal_widget *w);
void hal__menubar_arrange_items(hal_widget *w);

/* paint.c */
void hal__paint_widget(hal_ui *ui, hal_widget *w, hal_canvas *canvas);
void hal__paint_menu_overlay(hal_ui *ui, hal_canvas *canvas);

/* input.c */
bool hal__input_event(hal_ui *ui, const hal_event *event);
void hal__input_menu_close(hal_ui *ui);
bool hal__input_menu_pointer(hal_ui *ui, const hal_event *event,
                             bool *consumed);

/* menu.c */
hal_menubar_data *hal__menubar_data(hal_widget *menubar);
hal_rectf hal__menu_popup_rect(hal_ui *ui);
hal_rectf hal__menu_item_rect(hal_ui *ui, int item_index);

/* uvview.c */
void hal__uvview_paint(hal_ui *ui, hal_widget *w, hal_canvas *canvas);

/* tree.c */
void hal__tree_paint(hal_ui *ui, hal_widget *w, hal_canvas *canvas);
void hal__tree_measure(hal_widget *w);
hal_tree_row *hal__tree_row_at(hal_widget *w, hal_f32 y);
void hal__tree_clear_rows(hal_widget *w);
void hal__tree_layout_rows(hal_widget *w);

#endif /* HAL_UI_INTERNAL_H */
