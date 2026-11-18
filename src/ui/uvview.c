/*
 * uvview.c — the UV layout editor widget
 */

#include "ui_internal.h"

#include <string.h>

hal_widget *hal_uvview_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    return hal__widget_create(parent->ui, parent, HAL_WIDGET_UVVIEW);
}

void hal_uvview_bind(hal_widget *uvview, const hal_mesh *mesh) {
    if (!uvview || uvview->kind != HAL_WIDGET_UVVIEW)
        return;
    uvview->s.uvview.mesh = mesh;
    if (uvview->ui)
        uvview->ui->needs_paint = true;
}

void hal__uvview_paint(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_rectf r = w->abs_rect;

    /* Checkerboard-less calm backdrop + grid at eighths. */
    hal_canvas_fill_rect(canvas, r, t->colors[HAL_THEME_WINDOW]);
    hal_color grid = t->colors[HAL_THEME_BORDER];
    for (int i = 1; i < 8; i++) {
        hal_f32 x = r.x + r.width * (hal_f32)i / 8.0f;
        hal_canvas_draw_line(canvas, x, r.y, x, r.y + r.height, grid);
        hal_f32 y = r.y + r.height * (hal_f32)i / 8.0f;
        hal_canvas_draw_line(canvas, r.x, y, r.x + r.width, y, grid);
    }
    /* The [0,1] border. */
    hal_canvas_draw_rect(canvas, r, t->colors[HAL_THEME_TEXT_DIM]);

    const hal_mesh *m = w->s.uvview.mesh;
    if (!m || !m->uvs)
        return;

    /* Map UV (u,v in [0,1], +v down) into the rect. */
#define UVX(u) (r.x + (u) * r.width)
#define UVY(v) (r.y + (v) * r.height)

    /* Face loops as polylines; selected faces in accent. */
    for (size_t f = 0; f < m->face_count; f++) {
        bool sel = m->face_sel && m->face_sel[f];
        hal_color c = sel ? t->colors[HAL_THEME_ACCENT]
                          : t->colors[HAL_THEME_TEXT_DIM];
        size_t n = m->face_off[f + 1] - m->face_off[f];
        for (size_t i = 0; i < n; i++) {
            hal_vec2f a = m->uvs[m->face_off[f] + i];
            hal_vec2f b =
                m->uvs[m->face_off[f] + (i + 1) % n];
            hal_canvas_draw_line(canvas, UVX(a.x), UVY(a.y),
                                 UVX(b.x), UVY(b.y), c);
        }
    }
}

#undef UVX
#undef UVY
