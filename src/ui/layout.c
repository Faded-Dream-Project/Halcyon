/*
 * layout.c — the measure/arrange engine
 *
 * Measure is bottom-up (a widget's wanted size from its content and
 * children); arrange is top-down (containers place children inside
 * their rect). All sizes are LOGICAL pixels; text measurement runs
 * through the UI's font cache at the UI's current scale.
 */

#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

static hal_f32 text_width(hal_ui *ui, hal_i32 px, const char *text) {
    hal_font *font = hal__ui_font(ui, px);
    if (!font || !text)
        return (hal_f32)px * 6.0f; /* fontless fallback: rough */
    hal_text_metrics m;
    if (!hal_ok(hal_font_measure(font, text, strlen(text), &m)))
        return 0.0f;
    return (hal_f32)m.advance_width / ui->scale;
}

/* ---- Measure ---- */

static hal_sizef measure_row_children(hal_widget *w, bool vertical) {
    const hal_theme *t = w->ui->theme;
    hal_f32 pad = t->padding * 0.5f;
    hal_f32 gap = t->gap;

    hal_f32 main = pad * 2.0f;
    hal_f32 cross = 0.0f;
    for (hal_widget *c = w->first_child; c; c = c->next) {
        if (!c->visible)
            continue;
        hal_sizef m = c->measured;
        if (vertical) {
            main += m.height + gap;
            if (m.width + pad * 2.0f > cross)
                cross = m.width + pad * 2.0f;
        } else {
            main += m.width + gap;
            if (m.height + pad * 2.0f > cross)
                cross = m.height + pad * 2.0f;
        }
    }
    if (w->first_child)
        main -= gap; /* no trailing gap */

    hal_sizef out = { vertical ? cross : main,
                      vertical ? main : cross };
    return out;
}

void hal__widget_measure(hal_widget *w) {
    if (!w)
        return;

    for (hal_widget *c = w->first_child; c; c = c->next)
        hal__widget_measure(c);

    hal_ui *ui = w->ui;
    const hal_theme *t = ui->theme;
    hal_f32 pad = t->padding;

    switch (w->kind) {
    case HAL_WIDGET_ROOT:
        break; /* the shell sizes the root */
    case HAL_WIDGET_LABEL:
        w->measured.width = text_width(ui, t->font_size, w->text) + pad;
        w->measured.height = (hal_f32)t->font_size + pad * 0.5f;
        break;
    case HAL_WIDGET_BUTTON:
        w->measured.width =
            text_width(ui, t->font_size, w->text) + pad * 2.5f;
        w->measured.height = (hal_f32)t->font_size + pad * 1.2f;
        break;
    case HAL_WIDGET_TOGGLE: {
        hal_f32 box = (hal_f32)t->font_size + 4.0f;
        w->measured.width =
            box + t->gap + text_width(ui, t->font_size, w->text) + pad;
        w->measured.height = (hal_f32)t->font_size + pad * 0.5f;
        break;
    }
    case HAL_WIDGET_SLIDER:
        w->measured.width = 90.0f;
        w->measured.height = (hal_f32)t->font_size + pad;
        break;
    case HAL_WIDGET_ENTRY:
        w->measured.width =
            text_width(ui, t->font_size, w->s.entry.buf) + pad * 2.0f;
        if (w->measured.width < 80.0f)
            w->measured.width = 80.0f;
        w->measured.height = (hal_f32)t->font_size + pad * 0.8f;
        break;
    case HAL_WIDGET_PANEL: {
        hal_sizef inner = measure_row_children(w, true);
        hal_f32 title = w->text[0] ? (hal_f32)t->font_size + 8.0f : 0.0f;
        w->measured.width = inner.width + pad;
        w->measured.height = inner.height + title + pad * 0.5f;
        break;
    }
    case HAL_WIDGET_ROW:
    case HAL_WIDGET_COLUMN:
        w->measured = measure_row_children(w, w->kind == HAL_WIDGET_COLUMN);
        break;
    case HAL_WIDGET_TABS: {
        hal_f32 strip = (hal_f32)t->font_size + 10.0f;
        hal_f32 width = 0.0f, height = strip;
        for (hal_widget *c = w->first_child; c; c = c->next) {
            width += text_width(ui, t->font_size, c->text) + pad * 2.0f;
            if (c->measured.height + strip > height)
                height = c->measured.height + strip;
            if (c->measured.width > width)
                width = width; /* pages may be wider than their tab */
        }
        w->measured.width = width > 0.0f ? width : 120.0f;
        w->measured.height = height;
        break;
    }
    case HAL_WIDGET_TREE:
        hal__tree_measure(w);
        break;
    case HAL_WIDGET_UVVIEW:
        w->measured.width = 160.0f;
        w->measured.height = 160.0f; /* square-ish editor */
        break;
    case HAL_WIDGET_CURVES:
        w->measured.width = 160.0f;
        w->measured.height = 120.0f; /* wide graph */
        break;
    case HAL_WIDGET_MENUBAR: {
        hal_f32 width = 0.0f;
        for (hal_widget *c = w->first_child; c; c = c->next)
            width += text_width(ui, t->font_size, c->text) + pad * 2.0f;
        w->measured.width = width;
        w->measured.height = (hal_f32)t->font_size + 10.0f;
        break;
    }
    }
}

/* Menubar item rects: computed from the ABSOLUTE rect during the
 * layout walk (arrange runs before absolute rects exist; input needs
 * the geometry before the first paint). */
void hal__menubar_arrange_items(hal_widget *w) {
    hal_ui *ui = w->ui;
    const hal_theme *t = ui->theme;
    hal_menubar_data *data = hal__menubar_data(w);
    if (!data)
        return;
    hal_f32 x = w->abs_rect.x;
    for (size_t m = 0; m < data->count; m++) {
        hal_f32 text_w = text_width(ui, t->font_size, data->menus[m].title);
        hal_f32 item_w = text_w + t->padding * 2.0f;
        data->menus[m].item_rect =
            (hal_rectf){ x, w->abs_rect.y, item_w, w->abs_rect.height };
        x += item_w;
    }
}

/* ---- Arrange ---- */

static void arrange_row_children(hal_widget *w, bool vertical) {
    const hal_theme *t = w->ui->theme;
    hal_f32 pad = t->padding * 0.5f;
    hal_f32 gap = t->gap;

    hal_f32 avail =
        vertical ? w->rect.height - pad * 2.0f : w->rect.width - pad * 2.0f;

    /* Fixed-size children take their measured extent; expand children
     * share whatever is left. */
    hal_f32 fixed = 0.0f;
    size_t expand_count = 0;
    for (hal_widget *c = w->first_child; c; c = c->next) {
        if (!c->visible)
            continue;
        hal_f32 m = vertical ? c->measured.height : c->measured.width;
        if (c->expand)
            expand_count++;
        else
            fixed += m;
    }
    size_t gaps = 0;
    for (hal_widget *c = w->first_child; c; c = c->next)
        if (c->visible)
            gaps++;
    if (gaps > 1)
        fixed += (hal_f32)(gaps - 1) * gap;
    hal_f32 share = 0.0f;
    if (expand_count > 0 && avail > fixed) {
        share = (avail - fixed) / (hal_f32)expand_count;
        if (share < 0.0f)
            share = 0.0f;
    }

    hal_f32 pos = pad;
    for (hal_widget *c = w->first_child; c; c = c->next) {
        if (!c->visible)
            continue;
        if (vertical) {
            hal_f32 h = c->expand
                            ? share
                            : c->measured.height;
            c->rect = (hal_rectf){ pad, pos, w->rect.width - pad * 2.0f, h };
            pos += h + gap;
        } else {
            hal_f32 wd = c->expand ? share : c->measured.width;
            c->rect = (hal_rectf){ pos, pad, wd, w->rect.height - pad * 2.0f };
            pos += wd + gap;
        }
    }
}

void hal__widget_arrange(hal_widget *w) {
    if (!w)
        return;

    const hal_theme *t = w->ui->theme;
    hal_f32 pad = t->padding;

    switch (w->kind) {
    case HAL_WIDGET_ROOT:
        /* Top-level children are placed by the shell. */
        break;
    case HAL_WIDGET_PANEL: {
        hal_f32 title = w->text[0] ? (hal_f32)t->font_size + 8.0f : 0.0f;
        hal_f32 inner = pad * 0.5f;
        for (hal_widget *c = w->first_child; c; c = c->next) {
            c->rect = (hal_rectf){ inner, title + inner,
                                   w->rect.width - inner * 2.0f,
                                   w->rect.height - title - inner * 2.0f };
        }
        break;
    }
    case HAL_WIDGET_ROW:
    case HAL_WIDGET_COLUMN:
        arrange_row_children(w, w->kind == HAL_WIDGET_COLUMN);
        break;
    case HAL_WIDGET_TABS: {
        hal_f32 strip = (hal_f32)t->font_size + 10.0f;
        hal_f32 x = 0.0f;
        for (hal_widget *c = w->first_child; c; c = c->next) {
            c->rect = (hal_rectf){ x, strip, w->rect.width,
                                   w->rect.height - strip };
            x += 0.0f; /* tab widths only matter for the strip paint */
        }
        break;
    }
    default:
        break;
    }

    for (hal_widget *c = w->first_child; c; c = c->next)
        hal__widget_arrange(c);
}
