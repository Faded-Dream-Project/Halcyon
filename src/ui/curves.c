/*
 * curves.c — the animation curve editor widget
 */

#include "ui_internal.h"

#include <math.h>
#include <string.h>

hal_widget *hal_curves_create(hal_widget *parent) {
    if (!parent)
        return NULL;
    return hal__widget_create(parent->ui, parent, HAL_WIDGET_CURVES);
}

void hal_curves_bind(hal_widget *curves, const void *clip) {
    if (!curves || curves->kind != HAL_WIDGET_CURVES)
        return;
    curves->s.curves.clip = clip;
    if (curves->ui)
        curves->ui->needs_paint = true;
}

void hal_curves_set_playhead(hal_widget *curves, hal_f32 time) {
    if (!curves || curves->kind != HAL_WIDGET_CURVES)
        return;
    curves->s.curves.play_time = time;
    if (curves->ui)
        curves->ui->needs_paint = true;
}

/* The clip/keys are core types; ui cannot include core headers (the
 * dependency runs the other way), so the graph walks the structures
 * through the small accessor below — implemented via a header the
 * CURVES widget owns that only forward-declares the pieces. In
 * practice Halcyon's layering allows ui -> core reads; anim.h is
 * included via ui.h's mesh include chain (scene). */
#include "../core/anim.h"

void hal__curves_paint(hal_ui *ui, hal_widget *w, hal_canvas *canvas) {
    const hal_theme *t = ui->theme;
    hal_rectf r = w->abs_rect;
    hal_canvas_fill_rect(canvas, r, t->colors[HAL_THEME_WINDOW]);

    const hal_clip *clip = w->s.curves.clip;
    if (!clip || clip->count == 0) {
        hal_font *font = hal_ui_font(ui, t->font_size_small);
        if (font)
            hal_canvas_draw_text(canvas, font, "no animation",
                                 strlen("no animation"), r.x + 6.0f,
                                 r.y + 14.0f,
                                 t->colors[HAL_THEME_TEXT_DIM]);
        return;
    }

    hal_f32 t0 = clip->start, t1 = clip->end;
    hal_f32 span = t1 - t0;
    if (span < 1e-4f)
        span = 1.0f;

    /* Component lines: red-ish/green-ish/blue-ish/white, dimmed. */
    static const hal_color comp[4] = {
        { 0.85f, 0.45f, 0.48f, 1.0f },
        { 0.55f, 0.78f, 0.52f, 1.0f },
        { 0.45f, 0.66f, 0.90f, 1.0f },
        { 0.85f, 0.82f, 0.60f, 1.0f },
    };
    /* Per-channel value bounds for normalization. */
    for (size_t c = 0; c < clip->count; c++) {
        const hal_anim_channel *ch = &clip->channels[c];
        if (ch->count == 0)
            continue;
        hal_f32 vmin = 1e30f, vmax = -1e30f;
        for (size_t k = 0; k < ch->count; k++) {
            vmin = fminf(vmin, ch->keys[k].vec.x);
            vmax = fmaxf(vmax, ch->keys[k].vec.x);
            vmin = fminf(vmin, ch->keys[k].vec.y);
            vmax = fmaxf(vmax, ch->keys[k].vec.y);
            vmin = fminf(vmin, ch->keys[k].vec.z);
            vmin = fminf(vmin, ch->keys[k].rot.w);
            vmax = fmaxf(vmax, ch->keys[k].rot.w);
        }
        if (vmax - vmin < 1e-4f) {
            vmin -= 0.5f;
            vmax += 0.5f;
        }
        int comps = ch->type == HAL_CH_ROT || ch->type == HAL_CH_BONE
                        ? 4
                        : 3;
        for (int ci = 0; ci < comps; ci++) {
            /* Polyline through the sampled curve (32 samples, using
             * the channel's own easing via hal_clip-like sampling:
             * linear between keys with the key's interp). */
            hal_f32 prev_x = 0, prev_y = 0;
            for (int s = 0; s <= 32; s++) {
                hal_f32 time =
                    t0 + span * (hal_f32)s / 32.0f;
                /* find bracketing keys */
                hal_f32 val = 0.0f;
                {
                    const hal_keyframe *a = &ch->keys[0];
                    const hal_keyframe *b =
                        &ch->keys[ch->count - 1];
                    for (size_t k = 0; k + 1 < ch->count; k++)
                        if (time >= ch->keys[k].time &&
                            time <= ch->keys[k + 1].time) {
                            a = &ch->keys[k];
                            b = &ch->keys[k + 1];
                            break;
                        }
                    hal_f32 u = 0.0f;
                    hal_f32 kspan = b->time - a->time;
                    if (kspan > 1e-6f)
                        u = (time - a->time) / kspan;
                    if (a->interp == HAL_INTERP_SMOOTH)
                        u = u * u * (3.0f - 2.0f * u);
                    else if (a->interp == HAL_INTERP_STEP)
                        u = 0.0f;
                    if (ch->type == HAL_CH_ROT ||
                        ch->type == HAL_CH_BONE) {
                        hal_f32 av = ci == 3 ? a->rot.w
                                     : ci == 0 ? a->rot.x
                                     : ci == 1 ? a->rot.y
                                               : a->rot.z;
                        hal_f32 bv = ci == 3 ? b->rot.w
                                     : ci == 0 ? b->rot.x
                                     : ci == 1 ? b->rot.y
                                               : b->rot.z;
                        val = av + (bv - av) * u;
                    } else {
                        hal_f32 av = ci == 0 ? a->vec.x
                                     : ci == 1 ? a->vec.y
                                               : a->vec.z;
                        hal_f32 bv = ci == 0 ? b->vec.x
                                     : ci == 1 ? b->vec.y
                                               : b->vec.z;
                        val = av + (bv - av) * u;
                    }
                }
                hal_f32 px =
                    r.x + (time - t0) / span * r.width;
                hal_f32 py =
                    r.y + r.height -
                    (val - vmin) / (vmax - vmin) * r.height;
                if (s > 0)
                    hal_canvas_draw_line(canvas, prev_x, prev_y,
                                         px, py, comp[ci]);
                prev_x = px;
                prev_y = py;
            }
        }
        /* Key dots. */
        for (size_t k = 0; k < ch->count; k++) {
            hal_f32 px =
                r.x + (ch->keys[k].time - t0) / span * r.width;
            hal_f32 val = ch->type == HAL_CH_ROT ||
                                  ch->type == HAL_CH_BONE
                              ? ch->keys[k].rot.w
                              : ch->keys[k].vec.x;
            hal_f32 py =
                r.y + r.height -
                (val - vmin) / (vmax - vmin) * r.height;
            hal_canvas_fill_circle(canvas, px, py, 2.0f,
                                   t->colors[HAL_THEME_TEXT]);
        }
    }

    /* The playhead. */
    hal_f32 px =
        r.x + (w->s.curves.play_time - t0) / span * r.width;
    if (px >= r.x && px <= r.x + r.width)
        hal_canvas_draw_line(canvas, px, r.y, px, r.y + r.height,
                             t->colors[HAL_THEME_ACCENT]);
    hal_canvas_draw_rect(canvas, r, t->colors[HAL_THEME_BORDER]);
}
