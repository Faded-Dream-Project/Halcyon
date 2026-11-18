/*
 * controls.c — widget constructors: labels, buttons, toggles,
 * sliders, entries, panels, rows/columns, tabs
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- Simple controls ---- */

hal_widget *hal_label_create(hal_widget *parent, const char *text) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_LABEL);
    if (!w)
        return NULL;
    hal_widget_set_text(w, text);
    return w;
}

hal_widget *hal_button_create(hal_widget *parent, const char *text,
                              void (*on_click)(hal_widget *w, void *user),
                              void *user_data) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_BUTTON);
    if (!w)
        return NULL;
    hal_widget_set_text(w, text);
    w->s.button.on_click = on_click;
    w->s.button.user_data = user_data;
    return w;
}

hal_widget *hal_toggle_create(hal_widget *parent, const char *text,
                              bool *bound) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_TOGGLE);
    if (!w)
        return NULL;
    hal_widget_set_text(w, text);
    w->s.toggle.bound = bound;
    return w;
}

hal_widget *hal_slider_create(hal_widget *parent, hal_f32 min, hal_f32 max,
                              hal_f32 *value) {
    if (!parent || !(max > min))
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_SLIDER);
    if (!w)
        return NULL;
    w->s.slider.min = min;
    w->s.slider.max = max;
    w->s.slider.value = value;
    return w;
}

hal_widget *hal_entry_create(hal_widget *parent, char *buf, size_t cap) {
    if (!parent || !buf || cap == 0)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_ENTRY);
    if (!w)
        return NULL;
    w->s.entry.buf = buf;
    w->s.entry.cap = cap;
    w->s.entry.caret = strlen(buf);
    return w;
}

/* ---- Containers ---- */

hal_widget *hal_panel_create(hal_widget *parent, const char *title) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_PANEL);
    if (!w)
        return NULL;
    /* An empty title means "no band"; the panel's text carries it. */
    if (title && title[0])
        hal_widget_set_text(w, title);
    return w;
}

hal_widget *hal_row_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    return hal__widget_create(parent->ui, parent, HAL_WIDGET_ROW);
}

hal_widget *hal_column_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    return hal__widget_create(parent->ui, parent, HAL_WIDGET_COLUMN);
}

hal_widget *hal_tabs_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    hal_widget *w = hal__widget_create(parent->ui, parent, HAL_WIDGET_TABS);
    if (!w)
        return NULL;
    w->s.tabs.active = 0;
    return w;
}

void hal_tabs_set_active(hal_widget *tabs, int index) {
    if (!tabs || tabs->kind != HAL_WIDGET_TABS || index < 0)
        return;
    /* Clamp to the page count. */
    int count = 0;
    for (hal_widget *c = tabs->first_child; c; c = c->next)
        count++;
    if (index >= count)
        index = count - 1;
    if (index < 0)
        index = 0;
    if (tabs->s.tabs.active != index) {
        tabs->s.tabs.active = index;
        if (tabs->ui)
            tabs->ui->needs_paint = true;
    }
}

int hal_tabs_active(const hal_widget *tabs) {
    if (!tabs || tabs->kind != HAL_WIDGET_TABS)
        return 0;
    return tabs->s.tabs.active;
}

void hal_entry_set_on_change(hal_widget *entry, hal_entry_change_fn on_change,
                             void *user_data) {
    if (!entry || entry->kind != HAL_WIDGET_ENTRY)
        return;
    entry->s.entry.on_change = on_change;
    entry->s.entry.on_change_user = user_data;
}
