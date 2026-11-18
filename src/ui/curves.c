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
#include <time.h>

bool curves_on_event(hal_widget *w, const void *event_v,
                     void *user);

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

/* ---- Interactive editing ---- */

static double curves_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The channel's value bounds (the same computation the painter
 * uses, factored so hit-testing and drawing cannot drift). */
static void curves_channel_bounds(const hal_anim_channel *ch,
                                  hal_f32 *vmin, hal_f32 *vmax) {
    hal_f32 lo = 1e30f, hi = -1e30f;
    for (size_t k = 0; k < ch->count; k++) {
        lo = fminf(lo, ch->keys[k].vec.x);
        hi = fmaxf(hi, ch->keys[k].vec.x);
        lo = fminf(lo, ch->keys[k].vec.y);
        hi = fmaxf(hi, ch->keys[k].vec.y);
        lo = fminf(lo, ch->keys[k].vec.z);
        lo = fminf(lo, ch->keys[k].rot.w);
        hi = fmaxf(hi, ch->keys[k].rot.w);
    }
    if (hi - lo < 1e-4f) {
        lo -= 0.5f;
        hi += 0.5f;
    }
    *vmin = lo;
    *vmax = hi;
}

void hal_curves_interactive(
    hal_widget *curves, void *clip,
    void (*on_edit)(int phase, const char *text, void *user),
    void *user) {
    if (!curves || curves->kind != HAL_WIDGET_CURVES)
        return;
    curves->s.curves.clip = clip;
    curves->s.curves.on_edit = on_edit;
    curves->s.curves.on_edit_user = user;
    curves->s.curves.drag_channel = -1;
    curves->s.curves.editing = false;
    hal_widget_set_event_hook(curves, curves_on_event, curves);
    if (curves->ui)
        curves->ui->needs_paint = true;
}

/* Screen position of a key's dot for its channel's bounds. */
static hal_vec2f curves_key_pos(hal_rectf r, const hal_clip *clip,
                                const hal_anim_channel *ch,
                                size_t k, hal_f32 vmin, hal_f32 vmax) {
    hal_f32 t0 = clip->start, span = clip->end - clip->start;
    if (span < 1e-4f)
        span = 1.0f;
    hal_f32 val = ch->type == HAL_CH_ROT || ch->type == HAL_CH_BONE
                      ? ch->keys[k].rot.w
                      : ch->keys[k].vec.x;
    return (hal_vec2f){
        r.x + (ch->keys[k].time - t0) / span * r.width,
        r.y + r.height -
            (val - vmin) / (vmax - vmin) * r.height
    };
}

bool curves_on_event(hal_widget *w, const void *event_v,
                     void *user) {
    (void)user;
    const hal_event *event = (const hal_event *)event_v;
    hal_curves_state *st = &w->s.curves;
    /* The interactive binding was handed to us as void* by the app
     * (which owns the clip mutably); the draw path stores it as
     * const. Going through const here would discard nothing the app
     * has not already granted. */
    hal_clip *clip = (hal_clip *)(size_t)st->clip;
    if (!clip || clip->count == 0)
        return false;
    hal_rectf r = w->abs_rect;

    if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN) {
        hal_f32 x = event->pointer_button.position.x;
        hal_f32 y = event->pointer_button.position.y;

        /* Key hit-test (any channel, 8px radius). */
        int hit_ch = -1, hit_key = -1;
        hal_f32 best = 8.0f;
        for (size_t c = 0; c < clip->count; c++) {
            const hal_anim_channel *ch = &clip->channels[c];
            hal_f32 vmin, vmax;
            curves_channel_bounds(ch, &vmin, &vmax);
            for (size_t k = 0; k < ch->count; k++) {
                hal_vec2f p =
                    curves_key_pos(r, clip, ch, k, vmin, vmax);
                hal_f32 d = sqrtf((p.x - x) * (p.x - x) +
                                  (p.y - y) * (p.y - y));
                if (d < best) {
                    best = d;
                    hit_ch = (int)c;
                    hit_key = (int)k;
                }
            }
        }

        bool double_click =
            curves_now_s() - st->last_click_s < 0.35 &&
            fabsf(x - st->last_click_x) < 6.0f &&
            fabsf(y - st->last_click_y) < 6.0f;
        st->last_click_s = curves_now_s();
        st->last_click_x = x;
        st->last_click_y = y;

        if (event->pointer_button.button == HAL_BUTTON_RIGHT) {
            /* Right-click: delete the hit key. */
            if (hit_ch >= 0 && hit_key >= 0) {
                if (!st->editing && st->on_edit) {
                    st->on_edit(HAL_CURVES_EDIT_BEGIN, NULL,
                                st->on_edit_user);
                    st->editing = true;
                }
                hal_clip_key_delete(clip, (size_t)hit_ch,
                                    (size_t)hit_key);
                if (st->on_edit) {
                    st->on_edit(HAL_CURVES_EDIT_END, "key deleted",
                                st->on_edit_user);
                    st->editing = false;
                }
                if (w->ui)
                    w->ui->needs_paint = true;
            }
            return hit_ch >= 0;
        }

        if (hit_ch < 0) {
            if (double_click) {
                /* Insert into the channel whose band contains y. */
                hal_f32 t0 = clip->start,
                        span = clip->end - clip->start;
                if (span < 1e-4f)
                    span = 1.0f;
                hal_f32 time =
                    t0 + (x - r.x) / r.width * span;
                int best_ch = -1;
                hal_f32 best_dist = 1e30f;
                for (size_t c = 0; c < clip->count; c++) {
                    const hal_anim_channel *ch =
                        &clip->channels[c];
                    hal_f32 vmin, vmax;
                    curves_channel_bounds(ch, &vmin, &vmax);
                    hal_f32 val =
                        vmin + (r.y + r.height - y) / r.height *
                                   (vmax - vmin);
                    /* The sampled channel value at this time. */
                    hal_f32 sample = 0.0f;
                    if (ch->count > 0) {
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
                        hal_f32 av = ch->type == HAL_CH_ROT ||
                                             ch->type == HAL_CH_BONE
                                         ? a->rot.w
                                         : a->vec.x;
                        hal_f32 bv = ch->type == HAL_CH_ROT ||
                                             ch->type == HAL_CH_BONE
                                         ? b->rot.w
                                         : b->vec.x;
                        sample = av + (bv - av) * u;
                    }
                    hal_f32 d = fabsf(sample - val);
                    if (d < best_dist) {
                        best_dist = d;
                        best_ch = (int)c;
                    }
                }
                if (best_ch >= 0) {
                    hal_anim_channel *ch =
                        &clip->channels[best_ch];
                    hal_f32 vmin, vmax;
                    curves_channel_bounds(ch, &vmin, &vmax);
                    hal_f32 val =
                        vmin + (r.y + r.height - y) / r.height *
                                   (vmax - vmin);
                    if (val < vmin)
                        val = vmin;
                    if (val > vmax)
                        val = vmax;
                    if (!st->editing && st->on_edit) {
                        st->on_edit(HAL_CURVES_EDIT_BEGIN, NULL,
                                    st->on_edit_user);
                        st->editing = true;
                    }
                    /* The new key carries the clicked value in its
                     * component slot; other components 0. */
                    hal_vec3f v = hal_vec3f_zero();
                    if (ch->type == HAL_CH_LOC_Y ||
                        ch->type == HAL_CH_SCALE_Y)
                        v.y = val;
                    else if (ch->type == HAL_CH_LOC_Z ||
                             ch->type == HAL_CH_SCALE_Z)
                        v.z = val;
                    else
                        v.x = val;
                    hal_clip_key_vec(clip, ch->type, ch->object,
                                     ch->bone, time, v,
                                     HAL_INTERP_SMOOTH);
                    if (st->on_edit) {
                        st->on_edit(HAL_CURVES_EDIT_END,
                                    "key inserted",
                                    st->on_edit_user);
                        st->editing = false;
                    }
                    if (w->ui)
                        w->ui->needs_paint = true;
                    return true;
                }
            }
            return false;
        }

        if (double_click) {
            /* Cycle the key's easing. */
            hal_anim_channel *ch = &clip->channels[hit_ch];
            hal_keyframe *k2 = &ch->keys[hit_key];
            const char *name = "linear";
            switch (k2->interp) {
            case HAL_INTERP_STEP:
                k2->interp = HAL_INTERP_LINEAR;
                name = "linear";
                break;
            case HAL_INTERP_LINEAR:
                k2->interp = HAL_INTERP_SMOOTH;
                name = "smooth";
                break;
            default:
                k2->interp = HAL_INTERP_STEP;
                name = "step";
                break;
            }
            if (st->on_edit)
                st->on_edit(HAL_CURVES_EDIT_INFO, name,
                            st->on_edit_user);
            if (w->ui)
                w->ui->needs_paint = true;
            return true;
        }

        /* Begin a drag. */
        st->drag_channel = hit_ch;
        st->drag_key = hit_key;
        curves_channel_bounds(&clip->channels[hit_ch],
                              &st->vmin, &st->vmax);
        if (st->on_edit) {
            st->on_edit(HAL_CURVES_EDIT_BEGIN, NULL,
                        st->on_edit_user);
            st->editing = true;
        }
        return true;
    }

    if (event->type == HAL_EVENT_POINTER_MOTION) {
        if (st->drag_channel < 0)
            return false;
        hal_f32 t0 = clip->start, span = clip->end - clip->start;
        if (span < 1e-4f)
            span = 1.0f;
        hal_f32 x = event->pointer.position.x;
        hal_f32 y = event->pointer.position.y;
        hal_f32 time = t0 + (x - r.x) / r.width * span;
        if (time < clip->start)
            time = clip->start;
        hal_f32 val =
            st->vmin +
            (r.y + r.height - y) / r.height * (st->vmax - st->vmin);
        if (val < st->vmin)
            val = st->vmin;
        if (val > st->vmax)
            val = st->vmax;
        hal_anim_channel *ch =
            &clip->channels[st->drag_channel];
        /* Keep the dragged key identified through the re-sort by
         * moving via time: find it again by nearest old time... the
         * drag tracks the key INDEX, and hal_clip_key_move re-sorts
         * — so re-find the key by its (pre-move) time each step. */
        hal_f32 drag_time = ch->keys[st->drag_key].time;
        hal_vec3f v = ch->keys[st->drag_key].vec;
        hal_quatf q = ch->keys[st->drag_key].rot;
        if (ch->type == HAL_CH_LOC_Y || ch->type == HAL_CH_SCALE_Y)
            v.y = val;
        else if (ch->type == HAL_CH_LOC_Z ||
                 ch->type == HAL_CH_SCALE_Z)
            v.z = val;
        else if (ch->type == HAL_CH_ROT || ch->type == HAL_CH_BONE)
            q.w = val;
        else
            v.x = val;
        hal_clip_key_move(clip, (size_t)st->drag_channel,
                          (size_t)st->drag_key, time, v, q);
        /* After the sort the key may have moved index; re-find it by
         * the time we just wrote (closest). */
        int found = st->drag_key;
        hal_f32 best_d = 1e30f;
        for (size_t k = 0; k < ch->count; k++) {
            hal_f32 d = fabsf(ch->keys[k].time - time);
            if (d < best_d) {
                best_d = d;
                found = (int)k;
            }
        }
        st->drag_key = found;
        (void)drag_time;
        if (w->ui)
            w->ui->needs_paint = true;
        return true;
    }

    if (event->type == HAL_EVENT_POINTER_BUTTON_UP) {
        if (st->drag_channel >= 0) {
            st->drag_channel = -1;
            if (st->on_edit && st->editing) {
                st->on_edit(HAL_CURVES_EDIT_END, "key moved",
                            st->on_edit_user);
                st->editing = false;
            }
            if (w->ui)
                w->ui->needs_paint = true;
            return true;
        }
        return false;
    }
    return false;
}
