/*
 * platform.c — the FDK implementation of Halcyon's platform layer
 *
 * The ONLY translation unit in Halcyon that includes FDK headers (see
 * platform.h's contract). Everything here is a mechanical value
 * translation: FDK types in, Halcyon types out, never the reverse.
 *
 * If the platform is ever swapped, this file (and only this file) is
 * rewritten against the new backend; docs/FDK-REQUESTS.md records what
 * Halcyon needs that FDK does not yet expose.
 */

#include "platform.h"

#include <stdlib.h>
#include <string.h>

#include "fdk/fdk.h"

/* ---- Internal structs ---- */

struct hal_platform {
    fdk_context *fdk;
};

struct hal_window {
    hal_platform *platform;
    fdk_window *fdk;
    hal_surface *surface;   /* lazily created, owned by the window */
    hal_event_fn callback;
    void *callback_user;
};

struct hal_surface {
    fdk_surface *fdk;
    bool window_owned;      /* true: belongs to a hal_window        */
};

struct hal_font {
    fdk_font *fdk;
};

/* ---- FDK <-> Halcyon value translation ---- */

static hal_result hal_result_from_fdk(fdk_result r) {
    /* Explicit mapping, not numeric coincidence: if FDK grows a code
     * Halcyon does not know, it degrades to HAL_ERR_UNKNOWN and the
     * log says the original. */
    switch (r) {
    case FDK_OK:                     return HAL_OK;
    case FDK_ERR_INVALID_ARGUMENT:   return HAL_ERR_INVALID_ARGUMENT;
    case FDK_ERR_OUT_OF_MEMORY:      return HAL_ERR_OUT_OF_MEMORY;
    case FDK_ERR_NOT_INITIALIZED:    return HAL_ERR_NOT_INITIALIZED;
    case FDK_ERR_UNSUPPORTED:        return HAL_ERR_UNSUPPORTED;
    case FDK_ERR_NOT_FOUND:          return HAL_ERR_NOT_FOUND;
    case FDK_ERR_LIMIT:              return HAL_ERR_LIMIT;
    case FDK_ERR_INVALID_STATE:      return HAL_ERR_INVALID_STATE;
    case FDK_ERR_PLATFORM_INIT:      return HAL_ERR_PLATFORM_INIT;
    case FDK_ERR_NO_DISPLAY:         return HAL_ERR_NO_DISPLAY;
    case FDK_ERR_WINDOW_CREATE:      return HAL_ERR_WINDOW_CREATE;
    case FDK_ERR_PLATFORM:           return HAL_ERR_PLATFORM;
    case FDK_ERR_SURFACE_CREATE:     return HAL_ERR_SURFACE_CREATE;
    case FDK_ERR_IO:                 return HAL_ERR_IO;
    case FDK_ERR_NOT_A_FILE:         return HAL_ERR_NOT_FOUND;
    case FDK_ERR_FONT_LOAD:          return HAL_ERR_FONT_LOAD;
    default:                         return HAL_ERR_UNKNOWN;
    }
}

static fdk_color hal_color_to_fdk(hal_color c) {
    fdk_color out = { c.r, c.g, c.b, c.a };
    return out;
}

static fdk_rect hal_rect_to_fdk(hal_rect r) {
    fdk_rect out = { r.x, r.y, r.width, r.height };
    return out;
}

static hal_rect hal_rect_from_fdk(fdk_rect r) {
    return hal_rect_make(r.x, r.y, r.width, r.height);
}


/* ---- Event translation ---- */

static void hal_translate_event(fdk_window *fdk_win,
                                const fdk_event_data *event,
                                void *user_data);

static void hal_dispatch(hal_window *win, const hal_event *event) {
    if (win->callback)
        win->callback(win, event, win->callback_user);
}

static void hal_translate_key(const fdk_key_event *fk, hal_key_event *hk) {
    hk->scancode = fk->scancode;
    hk->codepoint = fk->codepoint;
    hk->modifiers = fk->modifiers; /* bit layout identical (1,2,4,8) */
    hk->is_repeat = fk->is_repeat;
}

static void hal_translate_event(fdk_window *fdk_win,
                                const fdk_event_data *event,
                                void *user_data) {
    (void)fdk_win;
    hal_window *win = user_data;
    if (!win)
        return;

    hal_event ev;
    memset(&ev, 0, sizeof(ev));

    switch (event->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        ev.type = HAL_EVENT_WINDOW_CONFIGURE;
        ev.configure.size.width = event->configure.size.width;
        ev.configure.size.height = event->configure.size.height;
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        ev.type = HAL_EVENT_WINDOW_CLOSE_REQUEST;
        break;
    case FDK_EVENT_WINDOW_FOCUS:
        ev.type = HAL_EVENT_WINDOW_FOCUS;
        ev.focus.focused = event->focus.focused;
        break;
    case FDK_EVENT_WINDOW_EXPOSE:
        ev.type = HAL_EVENT_WINDOW_EXPOSE;
        ev.expose.area = hal_rect_from_fdk(event->expose.area);
        break;
    case FDK_EVENT_WINDOW_STATE:
        ev.type = HAL_EVENT_WINDOW_STATE;
        ev.state.maximized = event->state.maximized;
        ev.state.minimized = event->state.minimized;
        ev.state.fullscreen = event->state.fullscreen;
        break;
    case FDK_EVENT_KEY_DOWN:
        ev.type = HAL_EVENT_KEY_DOWN;
        hal_translate_key(&event->key, &ev.key);
        break;
    case FDK_EVENT_KEY_UP:
        ev.type = HAL_EVENT_KEY_UP;
        hal_translate_key(&event->key, &ev.key);
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER:
    case FDK_EVENT_POINTER_LEAVE:
        ev.type = (event->type == FDK_EVENT_POINTER_MOTION)
                      ? HAL_EVENT_POINTER_MOTION
                      : (event->type == FDK_EVENT_POINTER_ENTER)
                            ? HAL_EVENT_POINTER_ENTER
                            : HAL_EVENT_POINTER_LEAVE;
        ev.pointer.position.x = event->pointer.position.x;
        ev.pointer.position.y = event->pointer.position.y;
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
    case FDK_EVENT_POINTER_BUTTON_UP:
        ev.type = (event->type == FDK_EVENT_POINTER_BUTTON_DOWN)
                      ? HAL_EVENT_POINTER_BUTTON_DOWN
                      : HAL_EVENT_POINTER_BUTTON_UP;
        ev.pointer_button.position.x = event->pointer_button.position.x;
        ev.pointer_button.position.y = event->pointer_button.position.y;
        ev.pointer_button.button = event->pointer_button.button;
        ev.pointer_button.modifiers = event->pointer_button.modifiers;
        break;
    case FDK_EVENT_POINTER_SCROLL:
        ev.type = HAL_EVENT_POINTER_SCROLL;
        ev.scroll.position.x = event->scroll.position.x;
        ev.scroll.position.y = event->scroll.position.y;
        ev.scroll.delta_x = event->scroll.delta_x;
        ev.scroll.delta_y = event->scroll.delta_y;
        break;
    case FDK_EVENT_IME_COMMIT:
        ev.type = HAL_EVENT_IME_COMMIT;
        ev.ime.text = event->ime.text;
        ev.ime.cursor = 0;
        break;
    case FDK_EVENT_IME_PREEDIT:
        ev.type = HAL_EVENT_IME_PREEDIT;
        ev.ime.text = event->ime.text;
        ev.ime.cursor = event->ime.cursor;
        break;
    default:
        /* An FDK event Halcyon does not model yet (touch, tablet,
         * gestures, drag-and-drop). Dropped here on purpose; each one
         * is added to the Halcyon event enum when a feature needs it. */
        return;
    }

    hal_dispatch(win, &ev);
}

/* ---- Lifecycle ---- */

hal_result hal_platform_init(const hal_platform_options *options,
                             hal_platform **out_platform) {
    if (!out_platform)
        return HAL_ERR_INVALID_ARGUMENT;

    hal_platform *p = calloc(1, sizeof(*p));
    if (!p)
        return HAL_ERR_OUT_OF_MEMORY;

    fdk_init_options fdk_opts;
    memset(&fdk_opts, 0, sizeof(fdk_opts));
    fdk_opts.app_id = options && options->app_id
                          ? options->app_id
                          : "org.faded-dream.halcyon";

    fdk_result r = fdk_init(&p->fdk, &fdk_opts);
    if (!fdk_ok(r)) {
        free(p);
        return hal_result_from_fdk(r);
    }

    *out_platform = p;
    return HAL_OK;
}

void hal_platform_shutdown(hal_platform *platform) {
    if (!platform)
        return;
    fdk_shutdown(platform->fdk);
    free(platform);
}

int hal_platform_pump(hal_platform *platform, int timeout_ms) {
    if (!platform)
        return HAL_ERR_INVALID_ARGUMENT;
    int n = fdk_pump_events(platform->fdk, timeout_ms);
    if (n < 0)
        return hal_result_from_fdk((fdk_result)n);
    return n;
}

/* ---- Windows ---- */

hal_result hal_window_create(hal_platform *platform,
                             const hal_window_options *options,
                             hal_window **out_window) {
    if (!platform || !out_window)
        return HAL_ERR_INVALID_ARGUMENT;

    hal_window *win = calloc(1, sizeof(*win));
    if (!win)
        return HAL_ERR_OUT_OF_MEMORY;

    fdk_window_options fdk_opts;
    memset(&fdk_opts, 0, sizeof(fdk_opts));
    fdk_opts.title = options && options->title ? options->title : "Halcyon";
    fdk_opts.width = options && options->width > 0 ? options->width : 1280;
    fdk_opts.height = options && options->height > 0 ? options->height : 800;

    fdk_result r = fdk_window_create(platform->fdk, &fdk_opts, &win->fdk);
    if (!fdk_ok(r)) {
        free(win);
        return hal_result_from_fdk(r);
    }

    win->platform = platform;
    fdk_window_set_event_callback(win->fdk, hal_translate_event, win);

    *out_window = win;
    return HAL_OK;
}

void hal_window_show(hal_window *window) {
    if (window)
        fdk_window_show(window->fdk);
}

void hal_window_destroy(hal_window *window) {
    if (!window)
        return;
    /* The surface dies with the FDK window's own surface; drop the
     * wrapper without destroying the FDK object it borrows. */
    if (window->surface)
        free(window->surface);
    fdk_window_destroy(window->fdk);
    free(window);
}

void hal_window_set_title(hal_window *window, const char *title) {
    if (window)
        fdk_window_set_title(window->fdk, title);
}

void hal_window_resize(hal_window *window, hal_i32 width, hal_i32 height) {
    if (window && width > 0 && height > 0)
        fdk_window_resize(window->fdk, width, height);
}

hal_result hal_window_get_size(const hal_window *window, hal_size *out_size) {
    if (!window || !out_size)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_size s;
    fdk_result r = fdk_window_get_size(window->fdk, &s);
    if (!fdk_ok(r))
        return hal_result_from_fdk(r);
    out_size->width = s.width;
    out_size->height = s.height;
    return HAL_OK;
}

hal_result hal_window_get_scale(const hal_window *window, hal_f32 *out_scale) {
    if (!window || !out_scale)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_f32 scale = 1.0f;
    fdk_result r = fdk_window_get_scale(window->fdk, &scale);
    if (!fdk_ok(r))
        return hal_result_from_fdk(r);
    *out_scale = scale;
    return HAL_OK;
}

hal_result hal_window_get_surface(hal_window *window,
                                  hal_surface **out_surface) {
    if (!window || !out_surface)
        return HAL_ERR_INVALID_ARGUMENT;

    if (!window->surface) {
        fdk_surface *fdk_surf = NULL;
        fdk_result r = fdk_window_get_surface(window->fdk, &fdk_surf);
        if (!fdk_ok(r))
            return hal_result_from_fdk(r);

        hal_surface *surf = calloc(1, sizeof(*surf));
        if (!surf)
            return HAL_ERR_OUT_OF_MEMORY;
        surf->fdk = fdk_surf;
        surf->window_owned = true;
        window->surface = surf;
    }

    *out_surface = window->surface;
    return HAL_OK;
}

void hal_window_set_cursor(hal_window *window, hal_cursor cursor) {
    if (!window)
        return;
    fdk_cursor_shape shape = FDK_CURSOR_DEFAULT;
    switch (cursor) {
    case HAL_CURSOR_DEFAULT:   shape = FDK_CURSOR_DEFAULT;   break;
    case HAL_CURSOR_POINTER:   shape = FDK_CURSOR_POINTER;   break;
    case HAL_CURSOR_TEXT:      shape = FDK_CURSOR_TEXT;      break;
    case HAL_CURSOR_CROSSHAIR: shape = FDK_CURSOR_CROSSHAIR; break;
    case HAL_CURSOR_GRAB:      shape = FDK_CURSOR_GRAB;      break;
    case HAL_CURSOR_GRABBING:  shape = FDK_CURSOR_GRABBING;  break;
    }
    fdk_window_set_cursor(window->fdk, shape);
}

void hal_window_set_event_callback(hal_window *window,
                                   hal_event_fn callback, void *user_data) {
    if (!window)
        return;
    window->callback = callback;
    window->callback_user = user_data;
}

/* ---- Surfaces ---- */

hal_result hal_surface_info_get(hal_surface *surface,
                                hal_surface_info *out_info) {
    if (!surface || !out_info)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_surface_info info;
    fdk_result r = fdk_surface_get_info(surface->fdk, &info);
    if (!fdk_ok(r))
        return hal_result_from_fdk(r);
    out_info->pixels = info.pixels;
    out_info->width = info.width;
    out_info->height = info.height;
    out_info->stride = info.stride;
    return HAL_OK;
}

hal_result hal_surface_present(hal_surface *surface) {
    if (!surface)
        return HAL_ERR_INVALID_ARGUMENT;
    return hal_result_from_fdk(fdk_surface_present(surface->fdk));
}

bool hal_surface_frame_ready(hal_surface *surface) {
    if (!surface)
        return false;
    return fdk_surface_frame_ready(surface->fdk);
}

void hal_surface_invalidate_all(hal_surface *surface) {
    if (surface)
        fdk_surface_invalidate_all(surface->fdk);
}

void hal_surface_invalidate(hal_surface *surface, hal_rect rect) {
    if (surface && !hal_rect_empty(rect))
        fdk_surface_invalidate(surface->fdk, hal_rect_to_fdk(rect));
}

hal_result hal_surface_offscreen_create(hal_i32 width, hal_i32 height,
                                        hal_surface **out_surface) {
    if (!out_surface)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_surface *fdk_surf = NULL;
    fdk_result r = fdk_surface_create(width, height, &fdk_surf);
    if (!fdk_ok(r))
        return hal_result_from_fdk(r);
    hal_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) {
        fdk_surface_destroy(fdk_surf);
        return HAL_ERR_OUT_OF_MEMORY;
    }
    surf->fdk = fdk_surf;
    surf->window_owned = false;
    *out_surface = surf;
    return HAL_OK;
}

void hal_surface_destroy(hal_surface *surface) {
    if (!surface)
        return;
    if (surface->window_owned) {
        /* Window surfaces belong to their window; refuse by design —
         * freeing the wrapper without the FDK object would dangle. */
        return;
    }
    fdk_surface_destroy(surface->fdk);
    free(surface);
}

hal_result hal_surface_save_png(const hal_surface *surface, const char *path) {
    if (!surface || !path)
        return HAL_ERR_INVALID_ARGUMENT;
    return hal_result_from_fdk(fdk_surface_save_png(surface->fdk, path));
}

/* ---- Text ---- */

hal_font *hal_font_load(const char *path, hal_i32 pixel_size) {
    if (!path)
        return NULL;
    fdk_font *fdk_face = fdk_font_load(path, pixel_size);
    if (!fdk_face)
        return NULL;
    hal_font *font = calloc(1, sizeof(*font));
    if (!font) {
        fdk_font_destroy(fdk_face);
        return NULL;
    }
    font->fdk = fdk_face;
    return font;
}

hal_font *hal_font_system_default(hal_i32 pixel_size) {
    fdk_font *fdk_face = fdk_font_load_system_default(pixel_size);
    if (!fdk_face)
        return NULL;
    hal_font *font = calloc(1, sizeof(*font));
    if (!font) {
        fdk_font_destroy(fdk_face);
        return NULL;
    }
    font->fdk = fdk_face;
    return font;
}

void hal_font_destroy(hal_font *font) {
    if (!font)
        return;
    fdk_font_destroy(font->fdk);
    free(font);
}

void hal_font_get_metrics(const hal_font *font, hal_font_metrics *out) {
    if (!font || !out)
        return;
    fdk_font_metrics m;
    fdk_font_get_metrics(font->fdk, &m);
    out->ascent = m.ascent;
    out->descent = m.descent;
    out->line_height = m.line_height;
}

hal_result hal_font_measure(const hal_font *font, const char *utf8,
                            size_t byte_len, hal_text_metrics *out) {
    if (!font || !utf8 || !out)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_text_metrics m;
    fdk_result r = fdk_font_measure_utf8(font->fdk, utf8, byte_len, &m);
    if (!fdk_ok(r))
        return hal_result_from_fdk(r);
    out->advance_width = m.advance_width;
    out->ink_top = m.ink_top;
    out->ink_bottom = m.ink_bottom;
    return HAL_OK;
}

hal_result hal_surface_draw_text(hal_surface *surface, hal_font *font,
                                 const char *utf8, size_t byte_len,
                                 hal_i32 pen_x, hal_i32 baseline_y,
                                 hal_color color, const hal_rect *clip) {
    if (!surface || !font || !utf8)
        return HAL_ERR_INVALID_ARGUMENT;

    if (clip && !hal_rect_empty(*clip)) {
        /* FDK's text honors its surface clip stack; ride it for the
         * call, then restore. Push never expands, so the intersection
         * is exactly what the caller asked for. */
        if (!fdk_ok(fdk_surface_push_clip(surface->fdk,
                                          hal_rect_to_fdk(*clip))))
            return HAL_ERR_LIMIT;
        fdk_result r = fdk_surface_draw_utf8(
            surface->fdk, font->fdk, utf8, byte_len, pen_x, baseline_y,
            hal_color_to_fdk(color));
        fdk_surface_pop_clip(surface->fdk);
        return hal_result_from_fdk(r);
    }

    return hal_result_from_fdk(fdk_surface_draw_utf8(
        surface->fdk, font->fdk, utf8, byte_len, pen_x, baseline_y,
        hal_color_to_fdk(color)));
}

/* ---- Clipboard ---- */

hal_result hal_clipboard_set_text(hal_platform *platform, const char *text) {
    if (!platform || !text)
        return HAL_ERR_INVALID_ARGUMENT;
    return hal_result_from_fdk(fdk_clipboard_set_text(platform->fdk, text));
}

char *hal_clipboard_get_text(hal_platform *platform) {
    if (!platform)
        return NULL;
    return fdk_clipboard_get_text(platform->fdk);
}
