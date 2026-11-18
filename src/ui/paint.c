/*
 * paint.c — the paint walk and per-widget drawing
 *
 * Every widget resolves its colors through the theme; nothing here
 * hardcodes a value. Drawing is clipped to the widget's rect (the
 * canvas clip stack), so children cannot paint outside parents.
 */

#include "ui_internal.h"

#include <string.h>

static void paint_label(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_font_metrics m;
    hal_font_get_metrics(font, &m);
    hal_f32 baseline = w->abs_rect.y + (w->abs_rect.height +
                                            (hal_f32)(m.ascent - m.descent)) /
                                           2.0f -
                       (hal_f32)m.ascent;
    hal_canvas_draw_text(canvas, font, w->text, strlen(w->text),
                         w->abs_rect.x, baseline, t->colors[HAL_THEME_TEXT]);
}

static void paint_button(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_color bg = t->colors[HAL_THEME_WIDGET];
    if (w->pressed)
        bg = t->colors[HAL_THEME_WIDGET_ACTIVE];
    else if (w->hovered)
        bg = t->colors[HAL_THEME_WIDGET_HOVER];
    hal_canvas_fill_rounded_rect(canvas, w->abs_rect, t->radius, bg);

    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_f32 text_w = hal_canvas_measure_text(canvas, font, w->text,
                                             strlen(w->text));
    hal_font_metrics m;
    hal_font_get_metrics(font, &m);
    hal_f32 cx = w->abs_rect.x + (w->abs_rect.width - text_w) * 0.5f;
    hal_f32 baseline = w->abs_rect.y + (w->abs_rect.height +
                                            (hal_f32)(m.ascent - m.descent)) /
                                           2.0f -
                       (hal_f32)m.ascent;
    hal_canvas_draw_text(canvas, font, w->text, strlen(w->text), cx,
                         baseline, t->colors[HAL_THEME_TEXT]);
}

static void paint_toggle(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_f32 box = (hal_f32)t->font_size + 2.0f;
    hal_rectf checkbox = { w->abs_rect.x, w->abs_rect.y + 1.0f, box, box };
    bool on = w->s.toggle.bound && *w->s.toggle.bound;
    hal_canvas_fill_rounded_rect(
        canvas, checkbox, t->radius * 0.6f,
        on ? t->colors[HAL_THEME_ACCENT]
           : t->colors[HAL_THEME_WIDGET]);
    if (on) {
        /* A calm check mark: two lines inside the box. */
        hal_f32 cx = checkbox.x + box * 0.5f;
        hal_f32 cy = checkbox.y + box * 0.5f;
        hal_color ink = t->colors[HAL_THEME_ACCENT_TEXT];
        hal_canvas_draw_line(canvas, cx - box * 0.22f, cy + box * 0.02f,
                             cx - box * 0.05f, cy + box * 0.2f, ink);
        hal_canvas_draw_line(canvas, cx - box * 0.05f, cy + box * 0.2f,
                             cx + box * 0.26f, cy - box * 0.18f, ink);
    }

    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_font_metrics m;
    hal_font_get_metrics(font, &m);
    hal_f32 baseline = w->abs_rect.y + 1.0f + box * 0.5f +
                       (hal_f32)(m.ascent - m.descent) * 0.5f -
                       (hal_f32)m.ascent;
    hal_canvas_draw_text(canvas, font, w->text, strlen(w->text),
                         checkbox.x + box + t->gap, baseline,
                         t->colors[HAL_THEME_TEXT]);
}

static void paint_slider(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_f32 v = w->s.slider.value ? *w->s.slider.value : 0.0f;
    hal_f32 norm = (v - w->s.slider.min) /
                   (w->s.slider.max - w->s.slider.min);
    if (norm < 0.0f)
        norm = 0.0f;
    if (norm > 1.0f)
        norm = 1.0f;

    hal_f32 track_h = 6.0f;
    hal_rectf track = { w->abs_rect.x,
                        w->abs_rect.y + (w->abs_rect.height - track_h) * 0.5f,
                        w->abs_rect.width, track_h };
    hal_canvas_fill_rounded_rect(canvas, track, track_h * 0.5f,
                                 t->colors[HAL_THEME_WIDGET]);
    if (norm > 0.001f) {
        hal_rectf fill = track;
        fill.width = track.width * norm;
        hal_canvas_fill_rounded_rect(canvas, fill, track_h * 0.5f,
                                     t->colors[HAL_THEME_ACCENT]);
    }

    hal_f32 knob_r = (hal_f32)t->font_size * 0.35f;
    hal_f32 kx = track.x + track.width * norm;
    hal_f32 ky = track.y + track_h * 0.5f;
    hal_canvas_fill_circle(canvas, kx, ky, knob_r,
                           w->hovered || w->s.slider.dragging
                               ? t->colors[HAL_THEME_TEXT]
                               : t->colors[HAL_THEME_ACCENT]);
}

static void paint_entry(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    bool focused = ui->focus == w;
    hal_canvas_fill_rounded_rect(
        canvas, w->abs_rect, t->radius, t->colors[HAL_THEME_WIDGET]);
    hal_canvas_draw_rounded_rect(
        canvas, w->abs_rect, t->radius,
        focused ? t->colors[HAL_THEME_ACCENT]
                : t->colors[HAL_THEME_BORDER]);

    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_font_metrics m;
    hal_font_get_metrics(font, &m);
    hal_f32 baseline =
        w->abs_rect.y + (w->abs_rect.height - (hal_f32)m.line_height) *
                            0.5f +
        (hal_f32)m.ascent;
    hal_f32 pad = t->padding * 0.6f;

    const char *buf = w->s.entry.buf;
    size_t caret = w->s.entry.caret;
    /* Text left of the caret (M2 clips at the field edge only — no
     * scrolling yet; entries are short). */
    hal_canvas_draw_text(canvas, font, buf, strlen(buf),
                         w->abs_rect.x + pad, baseline,
                         t->colors[HAL_THEME_TEXT]);

    if (focused) {
        hal_f32 cx = w->abs_rect.x + pad +
                     hal_canvas_measure_text(canvas, font, buf, caret);
        hal_canvas_fill_rect(
            canvas,
            (hal_rectf){ cx, baseline - (hal_f32)m.ascent + 1.0f, 1.0f,
                         (hal_f32)(m.ascent + m.descent) - 2.0f },
            t->colors[HAL_THEME_TEXT]);
    }
}

static void paint_panel(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_canvas_fill_rounded_rect(canvas, w->abs_rect, t->radius,
                                 t->colors[HAL_THEME_PANEL]);
    if (w->text[0]) {
        hal_f32 title_h = (hal_f32)t->font_size + 8.0f;
        hal_rectf band = w->abs_rect;
        band.height = title_h;
        /* Square the band's bottom corners against the panel. */
        hal_canvas_fill_rect(canvas, band, t->colors[HAL_THEME_PANEL_TITLE]);
        hal_rectf band_top = band;
        band_top.height = title_h * 0.5f + t->radius;
        hal_canvas_fill_rounded_rect(canvas, band_top, t->radius,
                                     t->colors[HAL_THEME_PANEL_TITLE]);

        hal_font *font = hal__ui_font(ui, t->font_size_small);
        if (font) {
            hal_font_metrics m;
            hal_font_get_metrics(font, &m);
            hal_canvas_draw_text(
                canvas, font, w->text, strlen(w->text), w->abs_rect.x + 8.0f,
                band.y + (title_h + (hal_f32)(m.ascent - m.descent)) *
                             0.5f -
                    (hal_f32)m.ascent,
                t->colors[HAL_THEME_TEXT_DIM]);
        }
    }
    hal_canvas_draw_rounded_rect(canvas, w->abs_rect, t->radius,
                                 t->colors[HAL_THEME_BORDER]);
}

static void paint_tabs(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_f32 strip = (hal_f32)t->font_size + 10.0f;
    hal_f32 x = w->abs_rect.x;
    int index = 0;

    hal_font *font = hal__ui_font(ui, t->font_size);
    for (hal_widget *c = w->first_child; c; c = c->next, index++) {
        if (!font)
            break;
        hal_f32 text_w =
            hal_canvas_measure_text(canvas, font, c->text, strlen(c->text));
        hal_f32 tab_w = text_w + t->padding * 2.0f;
        bool active = (index == w->s.tabs.active);
        hal_rectf tab = { x, w->abs_rect.y, tab_w, strip };
        hal_canvas_fill_rounded_rect(
            canvas, tab, t->radius,
            active ? t->colors[HAL_THEME_PANEL]
                   : t->colors[HAL_THEME_WINDOW]);
        hal_font_metrics m;
        hal_font_get_metrics(font, &m);
        hal_canvas_draw_text(
            canvas, font, c->text, strlen(c->text), tab.x + t->padding,
            tab.y + (strip + (hal_f32)(m.ascent - m.descent)) * 0.5f -
                (hal_f32)m.ascent,
            active ? t->colors[HAL_THEME_TEXT]
                   : t->colors[HAL_THEME_TEXT_DIM]);
        x += tab_w;
    }

    /* Only the active page paints. */
    hal_rectf body = { w->abs_rect.x, w->abs_rect.y + strip,
                       w->abs_rect.width, w->abs_rect.height - strip };
    hal_canvas_push_clip(canvas, body);
    index = 0;
    for (hal_widget *c = w->first_child; c; c = c->next, index++)
        if (index == w->s.tabs.active)
            hal__paint_widget(ui, c, canvas);
    hal_canvas_pop_clip(canvas);
}

static void paint_menubar(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_canvas_fill_rect(canvas, w->abs_rect, t->colors[HAL_THEME_WINDOW]);

    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_f32 x = w->abs_rect.x;

    hal_menubar_data *data = hal__menubar_data(w);
    if (!data)
        return;
    for (size_t m = 0; m < data->count; m++) {
        hal_f32 item_w = data->menus[m].item_rect.width;
        bool open = (ui->open_menubar == w &&
                     (int)m == ui->open_menu_index);
        hal_pointf pt = ui->last_pointer;
        hal_rectf ir = data->menus[m].item_rect;
        bool hover = pt.x >= ir.x && pt.y >= ir.y &&
                     pt.x < ir.x + ir.width && pt.y < ir.y + ir.height;
        if (open || hover)
            hal_canvas_fill_rounded_rect(
                canvas, data->menus[m].item_rect, t->radius,
                open ? t->colors[HAL_THEME_WIDGET_ACTIVE]
                     : t->colors[HAL_THEME_WIDGET_HOVER]);
        hal_font_metrics fm;
        hal_font_get_metrics(font, &fm);
        hal_canvas_draw_text(
            canvas, font, data->menus[m].title,
            strlen(data->menus[m].title), x + t->padding,
            w->abs_rect.y +
                (w->abs_rect.height +
                 (hal_f32)(fm.ascent - fm.descent)) * 0.5f -
                (hal_f32)fm.ascent,
            t->colors[HAL_THEME_TEXT]);
        x += item_w;
    }
}

/* The paint walk. */
void hal__paint_widget(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    if (!w || !w->visible)
        return;

    hal_canvas_push_clip(canvas, w->abs_rect);

    switch (w->kind) {
    case HAL_WIDGET_ROOT:
        hal_canvas_fill_rect(canvas, w->abs_rect,
                             ui->theme->colors[HAL_THEME_WINDOW]);
        break;
    case HAL_WIDGET_LABEL:
        paint_label(ui, w, canvas);
        break;
    case HAL_WIDGET_BUTTON:
        paint_button(ui, w, canvas);
        break;
    case HAL_WIDGET_TOGGLE:
        paint_toggle(ui, w, canvas);
        break;
    case HAL_WIDGET_SLIDER:
        paint_slider(ui, w, canvas);
        break;
    case HAL_WIDGET_ENTRY:
        paint_entry(ui, w, canvas);
        break;
    case HAL_WIDGET_PANEL:
        paint_panel(ui, w, canvas);
        break;
    case HAL_WIDGET_ROW:
    case HAL_WIDGET_COLUMN:
        break; /* invisible containers */
    case HAL_WIDGET_TABS:
        paint_tabs(ui, w, canvas);
        break;
    case HAL_WIDGET_TREE:
        hal__tree_paint(ui, w, canvas);
        break;
    case HAL_WIDGET_MENUBAR:
        paint_menubar(ui, w, canvas);
        break;
    case HAL_WIDGET_UVVIEW:
        hal__uvview_paint(ui, w, canvas);
        break;
    case HAL_WIDGET_CURVES:
        hal__curves_paint(ui, w, canvas);
        break;
    }

    for (hal_widget *c = w->first_child; c; c = c->next)
        hal__paint_widget(ui, c, canvas);

    hal_canvas_pop_clip(canvas);
}

/* ---- The open-menu overlay (painted last, on top of everything) ---- */

void hal__paint_menu_overlay(hal_ui *ui, hal_canvas *canvas) {
    if (!ui->open_menubar || ui->open_menu_index < 0)
        return;

    hal_menubar_data *data = hal__menubar_data(ui->open_menubar);
    if (!data || (size_t)ui->open_menu_index >= data->count)
        return;

    const hal_theme *t = ui->theme;
    hal_menu *menu = &data->menus[ui->open_menu_index];
    hal_rectf popup = hal__menu_popup_rect(ui);
    hal_f32 item_h = (hal_f32)t->font_size + 10.0f;

    /* Panel surface with a soft border; a faint shadow band under it. */
    hal_rectf shadow = popup;
    shadow.x += 2.0f;
    shadow.y += 3.0f;
    hal_canvas_fill_rounded_rect(canvas, shadow, t->radius,
                                 t->colors[HAL_THEME_WINDOW]);
    hal_canvas_fill_rounded_rect(canvas, popup, t->radius,
                                 t->colors[HAL_THEME_PANEL]);
    hal_canvas_draw_rounded_rect(canvas, popup, t->radius,
                                 t->colors[HAL_THEME_BORDER]);

    hal_font *font = hal__ui_font(ui, t->font_size);
    if (!font)
        return;
    hal_font_metrics fm;
    hal_font_get_metrics(font, &fm);

    for (size_t i = 0; i < menu->count; i++) {
        hal_menu_item *item = &menu->items[i];
        hal_rectf row = { popup.x + 2.0f,
                          popup.y + 4.0f + (hal_f32)i * item_h,
                          popup.width - 4.0f, item_h };

        if (!item->text) {
            /* Separator. */
            hal_rectf sep = { row.x + 8.0f, row.y + row.height * 0.5f,
                              row.width - 16.0f, 1.0f };
            hal_canvas_fill_rect(canvas, sep, t->colors[HAL_THEME_BORDER]);
            continue;
        }

        if ((int)i == ui->menu_hover_item) {
            hal_canvas_fill_rounded_rect(canvas, row, t->radius * 0.6f,
                                         t->colors[HAL_THEME_SELECT]);
        }

        hal_f32 baseline = row.y + (row.height +
                                    (hal_f32)(fm.ascent - fm.descent)) *
                                       0.5f -
                           (hal_f32)fm.ascent;
        hal_canvas_draw_text(canvas, font, item->text, strlen(item->text),
                             row.x + 10.0f, baseline,
                             t->colors[HAL_THEME_TEXT]);
        if (item->shortcut) {
            hal_f32 sw = hal_canvas_measure_text(canvas, font,
                                                 item->shortcut,
                                                 strlen(item->shortcut));
            hal_canvas_draw_text(canvas, font, item->shortcut,
                                 strlen(item->shortcut),
                                 row.x + row.width - sw - 10.0f, baseline,
                                 t->colors[HAL_THEME_TEXT_DIM]);
        }
    }
}
