/*
 * menu.c — the menu bar and its in-window overlay (decision D-008:
 * menus are overlays Halcyon paints itself, not OS popups)
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* Measures with a UI font and returns LOGICAL pixels. */
static hal_f32 menu_text_width(hal_ui *ui, hal_font *font,
                               const char *text) {
    if (!text)
        return 0.0f;
    hal_text_metrics m;
    if (!hal_ok(hal_font_measure(font, text, strlen(text), &m)))
        return 0.0f;
    return (hal_f32)m.advance_width / ui->scale;
}

hal_menubar_data *hal__menubar_data(hal_widget *menubar) {
    if (!menubar || menubar->kind != HAL_WIDGET_MENUBAR)
        return NULL;
    if (!menubar->user_data) {
        hal_menubar_data *data = calloc(1, sizeof(*data));
        if (data)
            menubar->user_data = data;
    }
    return menubar->user_data;
}

hal_widget *hal_menubar_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    hal_widget *w =
        hal__widget_create(parent->ui, parent, HAL_WIDGET_MENUBAR);
    if (!w)
        return NULL;
    if (!hal__menubar_data(w)) {
        free(w->text);
        free(w);
        return NULL;
    }
    return w;
}

int hal_menu_add(hal_widget *menubar, const char *title) {
    hal_menubar_data *data = hal__menubar_data(menubar);
    if (!data || !title)
        return -1;

    if (data->count == data->capacity) {
        size_t cap = data->capacity ? data->capacity * 2 : 4;
        hal_menu *grown = realloc(data->menus, cap * sizeof(*grown));
        if (!grown)
            return -1;
        data->menus = grown;
        data->capacity = cap;
    }

    hal_menu *menu = &data->menus[data->count];
    memset(menu, 0, sizeof(*menu));
    menu->title = malloc(strlen(title) + 1);
    if (!menu->title)
        return -1;
    strcpy(menu->title, title);
    return (int)data->count++;
}

void hal_menu_item_add(hal_widget *menubar, int menu_index,
                       const char *text, const char *shortcut,
                       void (*on_activate)(hal_widget *menubar, int menu,
                                           int item, void *user),
                       void *user_data) {
    hal_menubar_data *data = hal__menubar_data(menubar);
    if (!data || menu_index < 0 || (size_t)menu_index >= data->count)
        return;

    hal_menu *menu = &data->menus[menu_index];
    if (menu->count == menu->capacity) {
        size_t cap = menu->capacity ? menu->capacity * 2 : 8;
        hal_menu_item *grown = realloc(menu->items, cap * sizeof(*grown));
        if (!grown)
            return;
        menu->items = grown;
        menu->capacity = cap;
    }

    hal_menu_item *item = &menu->items[menu->count];
    memset(item, 0, sizeof(*item));
    if (text) {
        item->text = malloc(strlen(text) + 1);
        if (item->text)
            strcpy(item->text, text);
    }
    if (shortcut) {
        item->shortcut = malloc(strlen(shortcut) + 1);
        if (item->shortcut)
            strcpy(item->shortcut, shortcut);
    }
    item->on_activate = on_activate;
    item->user_data = user_data;
    menu->count++;

    if (menubar->ui)
        menubar->ui->needs_paint = true;
}

void hal_menu_close(hal_widget *menubar) {
    if (!menubar || !menubar->ui)
        return;
    hal__input_menu_close(menubar->ui);
}

/* ---- Overlay geometry ---- */

hal_rectf hal__menu_popup_rect(hal_ui *ui) {
    if (!ui->open_menubar || ui->open_menu_index < 0)
        return (hal_rectf){ 0, 0, 0, 0 };

    hal_menubar_data *data = hal__menubar_data(ui->open_menubar);
    if (!data || (size_t)ui->open_menu_index >= data->count)
        return (hal_rectf){ 0, 0, 0, 0 };

    const hal_theme *t = ui->theme;
    hal_menu *menu = &data->menus[ui->open_menu_index];

    hal_f32 width = 140.0f;
    hal_f32 item_h = hal__text_row_h(ui, t->font_size) + 10.0f;
    hal_f32 height = 4.0f + (hal_f32)menu->count * item_h;

    /* The popup hangs from the bar item; width fits the widest line
     * (label + shortcut), which measuring needs a font for — the
     * cached one. */
    hal_font *font = hal__ui_font(ui, t->font_size);
    if (font) {
        for (size_t i = 0; i < menu->count; i++) {
            hal_f32 w = 24.0f;
            w += menu_text_width(ui, font, menu->items[i].text);
            if (menu->items[i].shortcut)
                w += 20.0f + menu_text_width(ui, font,
                                             menu->items[i].shortcut);
            if (w > width)
                width = w;
        }
    }

    hal_rectf anchor = menu->item_rect;
    return (hal_rectf){ anchor.x, anchor.y + anchor.height, width, height };
}

hal_rectf hal__menu_item_rect(hal_ui *ui, int item_index) {
    hal_rectf popup = hal__menu_popup_rect(ui);
    const hal_theme *t = ui->theme;
    hal_f32 item_h = hal__text_row_h(ui, t->font_size) + 10.0f;
    return (hal_rectf){ popup.x, popup.y + 4.0f + (hal_f32)item_index * item_h,
                        popup.width, item_h };
}

/* ---- Public geometry queries ---- */

bool hal_ui_menu_open(const hal_ui *ui) {
    return ui && ui->open_menubar && ui->open_menu_index >= 0;
}

hal_rectf hal_ui_menu_item_rect(const hal_ui *ui, int item) {
    if (!hal_ui_menu_open(ui) || item < 0)
        return (hal_rectf){ 0, 0, 0, 0 };
    return hal__menu_item_rect((hal_ui *)(uintptr_t)ui, item);
}
