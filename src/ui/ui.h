/*
 * ui.h — Halcyon's own widget toolkit (the module's whole public API)
 *
 * A retained widget tree the application builds once and mutates:
 * labels, buttons, toggles, sliders, entries, panels, rows/columns,
 * tabs, menu bars, tree views. Halcyon paints it itself through the
 * canvas (src/ui/canvas.h) and routes platform events into it — no
 * platform widget machinery anywhere (decision D-002/D-003).
 *
 * Layout is measure-then-arrange: containers compute their size from
 * children (hal_widget_layout) and the application places top-level
 * panels (the editor shell). Coordinates are LOGICAL pixels; HiDPI is
 * the canvas's business.
 *
 * Input: hal_ui_handle_event() hit-tests, tracks hover/press, drives
 * focus and the implicit mouse grab (slider drags, open menus), and
 * returns true when the event was consumed. It is fully headless —
 * the tests feed synthetic events and assert behavior.
 *
 * Menus are OVERLAYS inside the window (decision D-008), not OS popup
 * windows: simpler, testable, and the app owns its whole surface.
 */

#ifndef HAL_UI_H
#define HAL_UI_H

#include "../core/mesh.h"
#include "../platform/platform.h"
#include "../platform/types.h"
#include "canvas.h"
#include "theme.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_ui hal_ui;
typedef struct hal_widget hal_widget;

/* ---- UI lifecycle ---- */

hal_ui *hal_ui_create(void);
void hal_ui_destroy(hal_ui *ui);

/* The theme the UI paints with (owned by the UI; replaced by the
 * setters below). Never NULL. */
const hal_theme *hal_ui_theme(const hal_ui *ui);
void hal_ui_set_theme(hal_ui *ui, hal_theme *theme); /* takes ownership */

/* The root container (an invisible column filling the window). */
hal_widget *hal_ui_root(hal_ui *ui);

/* Repaint bookkeeping: any state change marks the UI dirty; the frame
 * loop repaints only when needed (or when the app animates). */
bool hal_ui_needs_paint(const hal_ui *ui);
void hal_ui_invalidate(hal_ui *ui);

/* The GPU viewport hole: when set, the UI's base fill leaves `rect`
 * transparent — the GL scene composited under the UI layer shows
 * through exactly there, and every UI overlay painted later (menus,
 * popups, tooltips, gizmo) still lands over it. */
void hal_ui_set_hole(hal_ui *ui, bool has, hal_rectf rect);

/* Paints the tree (and any open menu overlay) onto the canvas. */
void hal_ui_paint(hal_ui *ui, hal_canvas *canvas);

/* Feeds one platform event; returns true when a widget consumed it. */
bool hal_ui_handle_event(hal_ui *ui, const hal_event *event);

/* The keyboard-focused widget (entries) or NULL. */
hal_widget *hal_ui_focus(hal_ui *ui);

/* A cached UI font at `logical_px` (loaded at px * the UI's scale;
 * owned by the UI, destroyed with it). NULL when no font exists. */
hal_font *hal_ui_font(hal_ui *ui, hal_i32 logical_px);

/* ---- Widget kinds ---- */

typedef enum hal_widget_kind {
    HAL_WIDGET_ROOT = 0,
    HAL_WIDGET_LABEL,
    HAL_WIDGET_BUTTON,
    HAL_WIDGET_TOGGLE,   /* checkbox bound to a bool            */
    HAL_WIDGET_SLIDER,   /* drag slider bound to a float        */
    HAL_WIDGET_ENTRY,    /* single-line text input              */
    HAL_WIDGET_PANEL,    /* titled surface, one client child    */
    HAL_WIDGET_ROW,      /* horizontal layout container         */
    HAL_WIDGET_COLUMN,   /* vertical layout container           */
    HAL_WIDGET_TABS,     /* tab strip + stacked pages           */
    HAL_WIDGET_TREE,     /* tree view (outliner)                */
    HAL_WIDGET_MENUBAR,  /* the menu bar                        */
    HAL_WIDGET_UVVIEW,   /* bound UV layout viewer              */
    HAL_WIDGET_CURVES,   /* bound animation curve graph         */
} hal_widget_kind;

/* ---- Constructors (each appends to `parent`) ---- */

hal_widget *hal_label_create(hal_widget *parent, const char *text);
hal_widget *hal_button_create(hal_widget *parent, const char *text,
                              void (*on_click)(hal_widget *w, void *user),
                              void *user_data);
/* The toggle flips *bound on click (the app reads it back). */
hal_widget *hal_toggle_create(hal_widget *parent, const char *text,
                              bool *bound);
/* min < max; the slider writes *value continuously while dragging. */
hal_widget *hal_slider_create(hal_widget *parent, hal_f32 min, hal_f32 max,
                              hal_f32 *value);
/* Single-line editable text in `buf` (cap incl. NUL). */
hal_widget *hal_entry_create(hal_widget *parent, char *buf, size_t cap);
/* Fired after every edit (the buffer already holds the new text). */
typedef void (*hal_entry_change_fn)(hal_widget *entry, void *user_data);
void hal_entry_set_on_change(hal_widget *entry,
                             hal_entry_change_fn on_change,
                             void *user_data);

/* Panel: themed surface with an optional title band; the first child
 * added becomes the client content (laid out inside the padding). */
hal_widget *hal_panel_create(hal_widget *parent, const char *title);

/* Layout containers. Children keep their measured size; children with
 * expand set share the leftover space along the container axis. */
hal_widget *hal_row_create(hal_widget *parent);
hal_widget *hal_column_create(hal_widget *parent);

/* Tabs: children are pages; the first child's text is its tab label.
 * hal_tabs_set_active switches pages. */
hal_widget *hal_tabs_create(hal_widget *parent);
void hal_tabs_set_active(hal_widget *tabs, int index);
int hal_tabs_active(const hal_widget *tabs);

/* Tree view: hierarchical rows the application populates. */
typedef struct hal_tree_row hal_tree_row;
hal_widget *hal_tree_create(hal_widget *parent);
/* Fired when the user selects a row (after the tree updated its own
 * selection). row is NULL when the click hit no row. */
typedef void (*hal_tree_select_fn)(hal_widget *tree, hal_tree_row *row,
                                   void *user_data);
void hal_tree_set_on_select(hal_widget *tree, hal_tree_select_fn on_select,
                            void *user_data);

/* Adds a row under `parent_row` (NULL = root level); depth, expand
 * state and selection are the tree's business. Returns the row. */
hal_tree_row *hal_tree_add(hal_widget *tree, hal_tree_row *parent_row,
                           const char *text, void *user_data);
void hal_tree_clear(hal_widget *tree);
/* The last row the user clicked (or NULL). */
hal_tree_row *hal_tree_selected(const hal_widget *tree);
hal_tree_row *hal_tree_row_next(const hal_widget *tree, hal_tree_row *row);
const char *hal_tree_row_text(const hal_tree_row *row);
void *hal_tree_row_user(const hal_tree_row *row);
/* Whether a row's children are shown. */
bool hal_tree_row_expanded(const hal_tree_row *row);
/* A row's window-logical rect (the tree positions rows on demand). */
hal_rectf hal_tree_row_rect(hal_widget *tree, const hal_tree_row *row);

/* UV editor: paints the bound mesh's UV layout (grid, face loops,
 * selected faces highlighted). The mesh is BORROWED (the app keeps it
 * alive); NULL clears. */
hal_widget *hal_uvview_create(hal_widget *parent);
void hal_uvview_bind(hal_widget *uvview, const hal_mesh *mesh);

/* Scrolls a COLUMN widget by dy pixels (clamped to its content).
 * Returns true when the scroll changed. */
bool hal_widget_scroll_by(hal_widget *column, hal_f32 dy);

/* A hover tooltip (shown after the pointer rests on the widget). */
void hal_widget_set_tooltip(hal_widget *w, const char *text);

/* Installs a generic per-widget event hook (pressed/dragged/
 * released; true consumes). Used by the curve editor. */
void hal_widget_set_event_hook(
    hal_widget *w,
    bool (*on_event)(hal_widget *w, const void *event, void *user),
    void *user);

/* Curve editor: draws the bound clip's channels as value-over-time
 * graphs (vec channels: x/y/z lines; rotation: x/y/z/w), key dots,
 * and a playhead at `play_time`. READ-ONLY visualization — scrubbing
 * and key insertion live in the timeline (honest scope; per-key
 * easing is edited in data). The clip is BORROWED; NULL clears. */
hal_widget *hal_curves_create(hal_widget *parent);
void hal_curves_bind(hal_widget *curves, const void *clip);
void hal_curves_set_playhead(hal_widget *curves, hal_f32 time);

/* Curve EDITING phases, reported through on_edit. */
typedef enum hal_curves_edit_phase {
    HAL_CURVES_EDIT_BEGIN, /* about to change: snapshot for undo */
    HAL_CURVES_EDIT_END,   /* edit finished: push the undo cmd  */
    HAL_CURVES_EDIT_INFO   /* transient message text in `text`   */
} hal_curves_edit_phase;

/* Makes the curve editor interactive:
 *   - drag a key dot: moves it in time AND value (re-sorted)
 *   - double-click a key: cycles its easing (step/linear/smooth)
 *   - double-click empty graph: inserts a key into the channel
 *     whose value band contains the click
 *   - right-click a key: deletes it
 * `clip` becomes a NON-CONST binding (editing); on_edit(BEGIN/END)
 * brackets each change for undo, INFO reports status text. */
void hal_curves_interactive(
    hal_widget *curves, void *clip,
    void (*on_edit)(int phase, const char *text, void *user),
    void *user);

/* Menu bar: menus with items. Items act through on_activate. */
hal_widget *hal_menubar_create(hal_widget *parent);
/* Adds a menu (e.g. "File") and returns its handle (an opaque index). */
int hal_menu_add(hal_widget *menubar, const char *title);
/* Adds an item to a menu. NULL text inserts a separator. shortcut
 * (may be NULL) is displayed right-aligned, e.g. "Ctrl+S". */
void hal_menu_item_add(hal_widget *menubar, int menu, const char *text,
                       const char *shortcut,
                       void (*on_activate)(hal_widget *menubar, int menu,
                                           int item, void *user),
                       void *user_data);
/* Closes any open menu (e.g. after an action). */
void hal_menu_close(hal_widget *menubar);

/* Open-menu state (tests and the shell's outside-click logic). */
bool hal_ui_menu_open(const hal_ui *ui);
/* The window-logical rect of open-menu item `item` (valid while a
 * menu is open; an empty rect otherwise). */
hal_rectf hal_ui_menu_item_rect(const hal_ui *ui, int item);

/* ---- Widget common API ---- */

void hal_widget_set_text(hal_widget *w, const char *text);
const char *hal_widget_text(const hal_widget *w);
void hal_widget_set_user(hal_widget *w, void *user_data);
void *hal_widget_user(hal_widget *w);
hal_widget_kind hal_widget_kind_of(const hal_widget *w);
hal_widget *hal_widget_parent(const hal_widget *w);
hal_widget *hal_widget_first_child(const hal_widget *w);
hal_widget *hal_widget_next_sibling(const hal_widget *w);
/* Sets the widget's arranged rect (parent-relative, logical). Manual
 * placement for top-level panels; containers arrange their children. */
void hal_widget_set_rect(hal_widget *w, hal_rectf rect);
hal_rectf hal_widget_rect(const hal_widget *w);
/* Absolute (window-logical) rect — valid after hal_ui_layout. */
hal_rectf hal_widget_abs_rect(const hal_widget *w);
void hal_widget_set_visible(hal_widget *w, bool visible);
bool hal_widget_visible(const hal_widget *w);
/* Whether the pointer was over the widget at the last motion event. */
bool hal_widget_hovered(const hal_widget *w);
/* Layout flags for row/column children. */
void hal_widget_set_expand(hal_widget *w, bool expand);
void hal_widget_set_fixed_size(hal_widget *w, hal_f32 width,
                               hal_f32 height);

/* ---- Layout ----
 * Measures the subtree bottom-up, then arranges top-down from each
 * container's rect. Call after tree mutations; the shell calls it on
 * resize. After layout, abs rects are valid for hit-testing.
 * Layout measures text with the UI's scale — set it to the window's
 * HiDPI scale before laying out (paint re-layouts automatically when
 * the canvas's scale disagrees). */
void hal_ui_set_scale(hal_ui *ui, hal_f32 scale);
hal_f32 hal_ui_scale(const hal_ui *ui);
void hal_ui_layout(hal_ui *ui);

/* The size a widget wants (measured; label width, button padding, ...).
 * Public because the shell needs it for splitters. */
hal_sizef hal_widget_measured(const hal_widget *w);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UI_H */
