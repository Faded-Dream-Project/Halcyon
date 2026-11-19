/*
 * ui.c — UI lifecycle, font cache, paint/event entry points
 */

#include "ui_internal.h"

#include <stdio.h>

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

void hal_ui_set_hole(hal_ui *ui, bool has, hal_rectf rect) {
    if (!ui)
        return;
    ui->has_hole = has;
    ui->hole = rect;
}

void hal_ui_paint(hal_ui *ui, hal_canvas *canvas) {
    if (!ui || !canvas)
        return;

    /* Every paint starts with a CLEAN clip stack (a reused canvas
     * must not inherit stale pushes from a previous pass). */
    canvas->clip_count = 0;

    /* The GPU viewport hole: ERASE it to transparent each paint —
     * overlays (menus, tooltips, gizmo) painted into the hole on a
     * previous frame must not accumulate; the GL scene shows
     * through the zeroed alpha. */
    if (ui->has_hole && canvas->pixels && canvas->stride > 0) {
        int hx0 = (int)(ui->hole.x * canvas->scale);
        int hy0 = (int)(ui->hole.y * canvas->scale);
        int hw = (int)(ui->hole.width * canvas->scale);
        int hh = (int)(ui->hole.height * canvas->scale);
        if (hx0 < 0) { hw += hx0; hx0 = 0; }
        if (hy0 < 0) { hh += hy0; hy0 = 0; }
        if (hx0 + hw > (int)canvas->width)
            hw = (int)canvas->width - hx0;
        if (hy0 + hh > (int)canvas->height)
            hh = (int)canvas->height - hy0;
        if (hw > 0 && hh > 0) {
            hal_u32 *px = (hal_u32 *)canvas->pixels;
            for (int y = hy0; y < hy0 + hh; y++)
                memset(px + (size_t)y * (size_t)canvas->stride +
                           (size_t)hx0, 0,
                       (size_t)hw * sizeof(hal_u32));
        }
    }

    /* A scale change between layout and paint re-measures everything;
     * the layout scale is part of the measurement. */
    if (canvas->scale != ui->scale) {
        ui->scale = canvas->scale;
        hal_ui_layout(ui);
    }

    hal__paint_widget(ui, ui->root, canvas);
    if (ui->open_menubar)
        hal__paint_menu_overlay(ui, canvas);

    ui->needs_paint = false;    hal__paint_tooltip(ui, canvas);
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

hal_f32 hal__text_row_h(hal_ui *ui, hal_i32 px) {
    hal_font *f = hal__ui_font(ui, px);
    if (f) {
        hal_font_metrics m;
        hal_font_get_metrics(f, &m);
        /* PHYSICAL metrics -> LOGICAL units (the font is loaded at
         * px * scale; the layout is logical). */
        hal_f32 h = (hal_f32)(m.ascent + m.descent) / ui->scale;
        if (h < (hal_f32)px)
            h = (hal_f32)px;
        return h;
    }
    return (hal_f32)px + 2.0f; /* fontless fallback */
}

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
    /* HAL_UI_FONT=/path.ttf overrides the system default — used by
     * the audit tier to verify layout against a different face. */
    static const char *override_path;
    static bool override_checked = false;
    if (!override_checked) {
        override_path = getenv("HAL_UI_FONT");
        override_checked = true;
    }
    hal_font *font = NULL;
    if (override_path && override_path[0])
        font = hal_font_load(override_path, physical);
    if (!font)
        font = hal_font_system_default(physical);
    if (!font)
        return NULL;

    ui->fonts[ui->font_count].px = logical_px;
    ui->fonts[ui->font_count].scale = ui->scale;
    ui->fonts[ui->font_count].font = font;
    ui->font_count++;
    return font;
}

/* ---- STEP-2 audit ---- */

typedef struct ui_audit_state {
    int violations;
} ui_audit_state;

static int g_ui_audit_pass = 0;
static void ui_audit_text_cb(void *user, const char *utf8,
                             hal_rectf text, hal_rectf clip,
                             bool scroll_boundary) {
    ui_audit_state *st = user;
    (void)g_ui_audit_pass;
    /* EMPTY clips (scrolled-out widgets) and cuts at a SCROLL fold
     * are correct scrolling, not clipped layout. Everything else —
     * ink outside a non-empty, non-scroll clip — is a violation. */
    if (scroll_boundary)
        return;
    if (clip.width <= 0.75f || clip.height <= 0.75f)
        return;
    st->violations++;
    char short_[48];
    size_t n = strlen(utf8);
    if (n > 40)
        n = 40;
    memcpy(short_, utf8, n);
    short_[n] = 0;
    fprintf(stderr,
            "UIAUDIT clipped text: \"%s\" "
            "ink=(%.1f,%.1f %.1fx%.1f) clip=(%.1f,%.1f %.1fx%.1f)\n",
            short_, text.x, text.y, text.width, text.height, clip.x,
            clip.y, clip.width, clip.height);
}

static hal_widget *find_menubar(hal_widget *w) {
    for (hal_widget *c = w; c; c = c->next) {
        if (c->kind == HAL_WIDGET_MENUBAR)
            return c;
        hal_widget *r = find_menubar(c->first_child);
        if (r)
            return r;
    }
    return NULL;
}

int hal__ui_audit(hal_ui *ui, hal_canvas *canvas) {
    if (!ui || !canvas)
        return -1;
    ui_audit_state st = { 0 };
    hal_canvas_set_text_audit(canvas, ui_audit_text_cb, &st);

    g_ui_audit_pass = 0;
    hal_ui_paint(ui, canvas);

    /* Every menu, opened one at a time, re-painted. */
    hal_widget *bar = find_menubar(ui->root);
    if (bar) {
        hal_menubar_data *data = hal__menubar_data(bar);
        if (data) {
            hal_font *font =
                hal__ui_font(ui, ui->theme->font_size);
            for (size_t m = 0; m < data->count; m++) {
                ui->open_menubar = bar;
                ui->open_menu_index = (int)m;
                ui->menu_hover_item = -1;
                g_ui_audit_pass++;
                hal_ui_paint(ui, canvas);

                /* Popup width >= widest item + its paddings. */
                if (font) {
                    hal_rectf popup = hal__menu_popup_rect(ui);
                    hal_menu *menu = &data->menus[m];
                    for (size_t it = 0; it < menu->count; it++) {
                        if (!menu->items[it].text)
                            continue; /* separator */
                        hal_f32 need = 24.0f;
                        hal_text_metrics tm;
                        if (hal_ok(hal_font_measure(
                                font, menu->items[it].text,
                                strlen(menu->items[it].text), &tm)))
                            need += (hal_f32)tm.advance_width /
                                    ui->scale;
                        if (menu->items[it].shortcut &&
                            hal_ok(hal_font_measure(
                                font, menu->items[it].shortcut,
                                strlen(menu->items[it].shortcut),
                                &tm)))
                            need += 20.0f +
                                    (hal_f32)tm.advance_width /
                                        ui->scale;
                        if (need > popup.width + 0.75f) {
                            st.violations++;
                            fprintf(stderr,
                                    "UIAUDIT popup too narrow: menu"
                                    " %zu item %zu \"%s\" needs"
                                    " %.1f, popup %.1f\n",
                                    m, it, menu->items[it].text, need,
                                    popup.width);
                        }
                    }
                }
            }
            ui->open_menubar = NULL;
            ui->open_menu_index = -1;
            g_ui_audit_pass++;
            hal_ui_paint(ui, canvas);
        }
    }
    hal_canvas_set_text_audit(canvas, NULL, NULL);
    return st.violations;
}
