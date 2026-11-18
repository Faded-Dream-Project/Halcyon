/*
 * ui.c — UI lifecycle, font cache, paint/event entry points
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

hal_ui *hal_ui_create(void) {
    hal_ui *ui = calloc(1, sizeof(*ui));
    if (!ui)
        return NULL;
    ui->theme = hal_theme_default();
    if (!ui->theme) {
        free(ui);
        return NULL;
    }
    ui->root = hal__widget_create(ui, NULL, HAL_WIDGET_ROOT);
    if (!ui->root) {
        hal_theme_destroy(ui->theme);
        free(ui);
        return NULL;
    }
    ui->open_menu_index = -1;
    ui->menu_hover_item = -1;
    ui->scale = 1.0f;
    ui->needs_paint = true;
    return ui;
}

void hal_ui_destroy(hal_ui *ui) {
    if (!ui)
        return;
    hal__widget_destroy_tree(ui->root);
    for (int i = 0; i < ui->font_count; i++)
        hal_font_destroy(ui->fonts[i].font);
    hal_theme_destroy(ui->theme);
    free(ui);
}

const hal_theme *hal_ui_theme(const hal_ui *ui) {
    return ui ? ui->theme : NULL;
}

void hal_ui_set_theme(hal_ui *ui, hal_theme *theme) {
    if (!ui)
        return;
    if (!theme)
        theme = hal_theme_default();
    hal_theme_destroy(ui->theme);
    ui->theme = theme;
    ui->needs_paint = true;
}

hal_widget *hal_ui_root(hal_ui *ui) {
    return ui ? ui->root : NULL;
}

bool hal_ui_needs_paint(const hal_ui *ui) {
    return ui ? ui->needs_paint : false;
}

void hal_ui_invalidate(hal_ui *ui) {
    if (ui)
        ui->needs_paint = true;
}

void hal_ui_paint(hal_ui *ui, hal_canvas *canvas) {
    if (!ui || !canvas)
        return;

    /* A scale change between layout and paint re-measures everything;
     * the layout scale is part of the measurement. */
    if (canvas->scale != ui->scale) {
        ui->scale = canvas->scale;
        hal_ui_layout(ui);
    }

    hal__paint_widget(ui, ui->root, canvas);
    if (ui->open_menubar)
        hal__paint_menu_overlay(ui, canvas);

    ui->needs_paint = false;
}

bool hal_ui_handle_event(hal_ui *ui, const hal_event *event) {
    if (!ui || !event)
        return false;
    return hal__input_event(ui, event);
}

hal_widget *hal_ui_focus(hal_ui *ui) {
    return ui ? ui->focus : NULL;
}

hal_font *hal_ui_font(hal_ui *ui, hal_i32 logical_px) {
    return hal__ui_font(ui, logical_px);
}

void hal_ui_set_scale(hal_ui *ui, hal_f32 scale) {
    if (ui && scale > 0.0f)
        ui->scale = scale;
}

hal_f32 hal_ui_scale(const hal_ui *ui) {
    return ui ? ui->scale : 1.0f;
}

void hal_ui_layout(hal_ui *ui) {
    if (!ui)
        return;
    hal__widget_measure(ui->root);
    /* The root's rect is the whole canvas; the shell sets it via
     * hal_widget_set_rect before layout (and on resize). Arrange runs
     * BEFORE the absolute walk: arranging moves children, so absolute
     * rects must be derived from the final relative geometry. */
    hal__widget_arrange(ui->root);
    hal__widget_walk_absolute(ui->root, 0.0f, 0.0f);
    ui->needs_paint = true;
}

/* ---- Font cache ---- */

hal_font *hal__ui_font(hal_ui *ui, hal_i32 logical_px) {
    if (!ui || logical_px < 4)
        return NULL;

    for (int i = 0; i < ui->font_count; i++)
        if (ui->fonts[i].px == logical_px &&
            ui->fonts[i].scale == ui->scale)
            return ui->fonts[i].font;

    if (ui->font_count >= HAL_UI_FONT_CACHE_MAX) {
        /* Simple recycling: the UI uses a handful of sizes; drop all
         * (the live ones reload on demand) rather than an LRU. */
        for (int i = 0; i < ui->font_count; i++)
            hal_font_destroy(ui->fonts[i].font);
        ui->font_count = 0;
    }

    hal_i32 physical = (hal_i32)((hal_f32)logical_px * ui->scale + 0.5f);
    if (physical < 4)
        physical = 4;
    hal_font *font = hal_font_system_default(physical);
    if (!font)
        return NULL;

    ui->fonts[ui->font_count].px = logical_px;
    ui->fonts[ui->font_count].scale = ui->scale;
    ui->fonts[ui->font_count].font = font;
    ui->font_count++;
    return font;
}
