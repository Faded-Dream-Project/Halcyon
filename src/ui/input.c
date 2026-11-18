/*
 * input.c — event routing: hit-testing, hover, press, focus, the
 * implicit grab (slider drags), menu interaction, entry editing
 *
 * All of this is pure state over the widget tree — the headless tests
 * drive it with synthetic hal_events and assert on tree state.
 */

#include "ui_internal.h"

#include <string.h>

/* ---- Helpers ---- */

static bool point_in(hal_rectf r, hal_f32 x, hal_f32 y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

void hal__input_menu_close(hal_ui *ui) {
    if (ui->open_menubar) {
        ui->open_menubar = NULL;
        ui->open_menu_index = -1;
        ui->menu_hover_item = -1;
        ui->needs_paint = true;
    }
}

static void clear_hover(hal_widget *w) {
    if (!w)
        return;
    w->hovered = false;
    for (hal_widget *c = w->first_child; c; c = c->next)
        clear_hover(c);
}

static void set_hover_chain(hal_widget *w) {
    for (; w; w = w->parent)
        w->hovered = true;
}

/* Which menu-bar item (if any) is under (x, y)? Returns index or -1. */
static int menubar_item_at(hal_widget *menubar, hal_f32 x, hal_f32 y) {
    hal_menubar_data *data = hal__menubar_data(menubar);
    if (!data)
        return -1;
    for (size_t m = 0; m < data->count; m++)
        if (point_in(data->menus[m].item_rect, x, y))
            return (int)m;
    return -1;
}

/* ---- Entry editing ---- */

/* UTF-8: byte length of the codepoint starting at byte `i`. */
static size_t utf8_len(const unsigned char *s, size_t i, size_t cap) {
    if (i >= cap)
        return 0;
    unsigned char c = s[i];
    size_t n = 1;
    if ((c & 0xE0u) == 0xC0u)
        n = 2;
    else if ((c & 0xF0u) == 0xE0u)
        n = 3;
    else if ((c & 0xF8u) == 0xF0u)
        n = 4;
    if (i + n > cap)
        return 1;
    return n;
}

static size_t utf8_prev(const unsigned char *s, size_t caret) {
    size_t i = caret - 1;
    while (i > 0 && (s[i] & 0xC0u) == 0x80u)
        i--;
    return i;
}

static bool entry_insert(hal_widget *w, hal_u32 codepoint) {
    if (!codepoint)
        return false;
    unsigned char tmp[4];
    size_t n = 0;
    if (codepoint < 0x80u) {
        tmp[n++] = (unsigned char)codepoint;
    } else if (codepoint < 0x800u) {
        tmp[n++] = (unsigned char)(0xC0u | (codepoint >> 6));
        tmp[n++] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint < 0x10000u) {
        tmp[n++] = (unsigned char)(0xE0u | (codepoint >> 12));
        tmp[n++] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
    } else {
        tmp[n++] = (unsigned char)(0xF0u | (codepoint >> 18));
        tmp[n++] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
    }

    size_t len = strlen(w->s.entry.buf);
    size_t caret = w->s.entry.caret;
    if (len + n + 1 > w->s.entry.cap)
        return true; /* full: not an error, just nothing to add */

    memmove(w->s.entry.buf + caret + n, w->s.entry.buf + caret,
            len - caret + 1);
    memcpy(w->s.entry.buf + caret, tmp, n);
    w->s.entry.caret = caret + n;
    return true;
}

static void entry_key(hal_ui *ui, hal_widget *w, const hal_event *event) {
    const unsigned char *buf =
        (const unsigned char *)w->s.entry.buf;
    size_t len = strlen(w->s.entry.buf);
    size_t caret = w->s.entry.caret;

    switch (event->key.scancode) {
    case HAL_KEY_BACKSPACE:
        if (caret > 0) {
            size_t prev = utf8_prev(buf, caret);
            memmove(w->s.entry.buf + prev, w->s.entry.buf + caret,
                    len - caret + 1);
            w->s.entry.caret = prev;
        }
        break;
    case HAL_KEY_DELETE:
        if (caret < len) {
            size_t next = caret + utf8_len(buf, caret, len);
            memmove(w->s.entry.buf + caret, w->s.entry.buf + next,
                    len - next + 1);
        }
        break;
    case HAL_KEY_LEFT:
        if (caret > 0)
            w->s.entry.caret = utf8_prev(buf, caret);
        break;
    case HAL_KEY_RIGHT:
        if (caret < len)
            w->s.entry.caret =
                caret + utf8_len(buf, caret, len);
        break;
    case HAL_KEY_HOME:
        w->s.entry.caret = 0;
        break;
    case HAL_KEY_END:
        w->s.entry.caret = len;
        break;
    default:
        if (event->key.codepoint >= 0x20u &&
            event->key.codepoint != 0x7Fu)
            entry_insert(w, event->key.codepoint);
        break;
    }
    ui->needs_paint = true;
}

/* Tab cycles focus through the entries in tree order. */
static void focus_cycle(hal_ui *ui, bool backward) {
    hal_widget *entries[256];
    size_t count = 0;

    /* Iterative depth-first walk. */
    hal_widget *stack[256];
    int depth = 0;
    if (ui->root)
        stack[depth++] = ui->root;
    while (depth > 0 && count < 256) {
        hal_widget *w = stack[--depth];
        if (w->visible && w->kind == HAL_WIDGET_ENTRY)
            entries[count++] = w;
        for (hal_widget *c = w->last_child; c; c = c->prev)
            if (depth < 256)
                stack[depth++] = c;
    }
    if (count == 0)
        return;

    int cur = -1;
    for (size_t i = 0; i < count; i++)
        if (entries[i] == ui->focus)
            cur = (int)i;

    int next;
    if (cur < 0)
        next = backward ? (int)count - 1 : 0;
    else
        next = backward ? (cur + (int)count - 1) % (int)count
                        : (cur + 1) % (int)count;
    ui->focus = entries[next];
    ui->needs_paint = true;
}

/* ---- The router ---- */

bool hal__input_event(hal_ui *ui, const hal_event *event) {
    if (!ui || !event)
        return false;

    switch (event->type) {
    case HAL_EVENT_POINTER_MOTION: {
        hal_f32 x = event->pointer.position.x;
        hal_f32 y = event->pointer.position.y;
        ui->last_pointer = event->pointer.position;

        /* An open menu owns the pointer over its popup and the bar. */
        if (ui->open_menubar) {
            hal_rectf popup = hal__menu_popup_rect(ui);
            if (point_in(popup, x, y)) {
                ui->menu_hover_item = -1;
                hal_menubar_data *data =
                    hal__menubar_data(ui->open_menubar);
                if (data && ui->open_menu_index >= 0 &&
                    (size_t)ui->open_menu_index < data->count) {
                    hal_menu *menu =
                        &data->menus[ui->open_menu_index];
                    for (size_t i = 0; i < menu->count; i++) {
                        if (point_in(hal__menu_item_rect(ui, (int)i), x,
                                     y)) {
                            ui->menu_hover_item = (int)i;
                            break;
                        }
                    }
                }
                ui->needs_paint = true;
                return true;
            }
            int bar_item = menubar_item_at(ui->open_menubar, x, y);
            if (bar_item >= 0) {
                /* Hovering another menu's bar item switches to it. */
                if (bar_item != ui->open_menu_index) {
                    ui->open_menu_index = bar_item;
                    ui->menu_hover_item = -1;
                    ui->needs_paint = true;
                }
                return true;
            }
        }

        /* The implicit grab (a dragging slider) owns all motion. */
        if (ui->grab && ui->grab->kind == HAL_WIDGET_SLIDER) {
            hal_widget *s = ui->grab;
            hal_f32 norm = (x - s->abs_rect.x) / s->abs_rect.width;
            if (norm < 0.0f)
                norm = 0.0f;
            if (norm > 1.0f)
                norm = 1.0f;
            *s->s.slider.value =
                s->s.slider.min +
                norm * (s->s.slider.max - s->s.slider.min);
            ui->needs_paint = true;
            return true;
        }

        /* Hover bookkeeping. */
        clear_hover(ui->root);
        hal_widget *hit = hal__hit_test(ui->root, x, y);
        if (hit)
            set_hover_chain(hit);
        ui->hover = hit;
        ui->needs_paint = true;
        return hit != NULL;
    }

    case HAL_EVENT_POINTER_BUTTON_DOWN: {
        if (event->pointer_button.button != HAL_BUTTON_LEFT)
            return false;
        hal_f32 x = event->pointer_button.position.x;
        hal_f32 y = event->pointer_button.position.y;
        ui->last_pointer = event->pointer_button.position;

        /* Open menu: the popup activates, the bar switches, anything
         * else closes (the outside click never falls through — the
         * standard menu dismissal contract). */
        if (ui->open_menubar) {
            hal_rectf popup = hal__menu_popup_rect(ui);
            if (point_in(popup, x, y)) {
                hal_menubar_data *data =
                    hal__menubar_data(ui->open_menubar);
                if (data && ui->open_menu_index >= 0 &&
                    (size_t)ui->open_menu_index < data->count) {
                    hal_menu *menu =
                        &data->menus[ui->open_menu_index];
                    for (size_t i = 0; i < menu->count; i++) {
                        if (point_in(hal__menu_item_rect(ui, (int)i), x,
                                     y)) {
                            if (menu->items[i].text &&
                                menu->items[i].on_activate) {
                                menu->items[i].on_activate(
                                    ui->open_menubar,
                                    ui->open_menu_index, (int)i,
                                    menu->items[i].user_data);
                            }
                            hal__input_menu_close(ui);
                            return true;
                        }
                    }
                }
                return true; /* dead zone inside the popup */
            }
            int bar_item = menubar_item_at(ui->open_menubar, x, y);
            if (bar_item >= 0) {
                ui->open_menu_index = bar_item;
                ui->menu_hover_item = -1;
                ui->needs_paint = true;
                return true;
            }
            hal__input_menu_close(ui);
            return true;
        }

        hal_widget *hit = hal__hit_test(ui->root, x, y);
        if (!hit) {
            /* A click on empty space releases keyboard focus. */
            if (ui->focus) {
                ui->focus = NULL;
                ui->needs_paint = true;
            }
            return false;
        }

        switch (hit->kind) {
        case HAL_WIDGET_BUTTON:
        case HAL_WIDGET_TOGGLE:
            hit->pressed = true;
            ui->press_target = hit;
            ui->needs_paint = true;
            return true;
        case HAL_WIDGET_SLIDER: {
            hal_widget *s = hit;
            hal_f32 norm = (x - s->abs_rect.x) / s->abs_rect.width;
            if (norm < 0.0f)
                norm = 0.0f;
            if (norm > 1.0f)
                norm = 1.0f;
            *s->s.slider.value =
                s->s.slider.min +
                norm * (s->s.slider.max - s->s.slider.min);
            s->s.slider.dragging = true;
            ui->grab = s;
            ui->needs_paint = true;
            return true;
        }
        case HAL_WIDGET_ENTRY:
            if (ui->focus != hit) {
                ui->focus = hit;
                hit->s.entry.caret = strlen(hit->s.entry.buf);
                ui->needs_paint = true;
            }
            return true;
        case HAL_WIDGET_MENUBAR: {
            int item = menubar_item_at(hit, x, y);
            if (item >= 0) {
                ui->open_menubar = hit;
                ui->open_menu_index = item;
                ui->menu_hover_item = -1;
                ui->needs_paint = true;
                return true;
            }
            return true; /* the bar's empty tail */
        }
        case HAL_WIDGET_TREE: {
            hal_tree_row *row = hal__tree_row_at(hit, y);
            if (row) {
                /* The expander triangle toggles; the row selects. */
                hal_f32 indent = 6.0f + (hal_f32)row->depth * 14.0f;
                hal_f32 tri_x = hit->abs_rect.x + indent + 6.0f;
                if (row->first_child && x < tri_x + 8.0f) {
                    row->expanded = !row->expanded;
                } else {
                    hit->s.tree.selected = row;
                    if (hit->s.tree.on_select)
                        hit->s.tree.on_select(hit, row,
                                              hit->s.tree.on_select_user);
                }
                ui->needs_paint = true;
            }
            return true;
        }
        default:
            return true;
        }
    }

    case HAL_EVENT_POINTER_BUTTON_UP: {
        if (event->pointer_button.button != HAL_BUTTON_LEFT)
            return false;

        /* The slider drag ends wherever the release lands. */
        if (ui->grab && ui->grab->kind == HAL_WIDGET_SLIDER) {
            ui->grab->s.slider.dragging = false;
            ui->grab = NULL;
            ui->needs_paint = true;
            return true;
        }

        /* Activate the pressed control when the RELEASE lands inside
         * it (the release position, not the hover flag — a press
         * followed by a far-away release with no motion event in
         * between must not fire). */
        hal_widget *pressed = ui->press_target;
        if (pressed) {
            ui->press_target = NULL;
            pressed->pressed = false;
            hal_f32 rx = event->pointer_button.position.x;
            hal_f32 ry = event->pointer_button.position.y;
            hal_rectf pr = pressed->abs_rect;
            bool over = rx >= pr.x && ry >= pr.y && rx < pr.x + pr.width &&
                        ry < pr.y + pr.height;
            if (over && pressed->kind == HAL_WIDGET_BUTTON &&
                pressed->s.button.on_click) {
                pressed->s.button.on_click(
                    pressed, pressed->s.button.user_data);
            }
            if (over && pressed->kind == HAL_WIDGET_TOGGLE &&
                pressed->s.toggle.bound) {
                *pressed->s.toggle.bound = !*pressed->s.toggle.bound;
            }
            ui->needs_paint = true;
            return true;
        }
        return false;
    }

    case HAL_EVENT_POINTER_LEAVE:
        clear_hover(ui->root);
        ui->hover = NULL;
        ui->needs_paint = true;
        return false;

    case HAL_EVENT_KEY_DOWN: {
        /* Escape closes an open menu first. */
        if (event->key.scancode == HAL_KEY_ESC && ui->open_menubar) {
            hal__input_menu_close(ui);
            return true;
        }

        if (event->key.scancode == HAL_KEY_TAB) {
            focus_cycle(ui, (event->key.modifiers & HAL_MOD_SHIFT) != 0);
            return true;
        }

        if (ui->focus && ui->focus->kind == HAL_WIDGET_ENTRY) {
            if (event->key.scancode == HAL_KEY_ESC) {
                ui->focus = NULL;
                ui->needs_paint = true;
                return true;
            }
            entry_key(ui, ui->focus, event);
            if (ui->focus->s.entry.on_change)
                ui->focus->s.entry.on_change(ui->focus,
                                             ui->focus->s.entry.on_change_user);
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}
