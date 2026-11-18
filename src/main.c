/*
 * main.c — Halcyon application entry
 *
 * M0 scope: open the window through the platform module, run the frame
 * loop (pump -> decide -> draw -> present), paint the milestone
 * placeholder (a calm dreamy gradient plus the Halcyon wordmark), and
 * shut down cleanly. The real editor UI (src/ui/) replaces this
 * placeholder scene as M2 lands.
 *
 * --smoke: automated window test. Opens the window, acquires the
 * framebuffer, draws one frame, presents it, and verifies the window
 * size matches the surface it drew into. Exits 0 on success, 1 on
 * timeout. A CONFIGURE event is recorded but not required: on a WM-less
 * X server (Xvfb) a window maps at its creation size and no configure
 * is generated — present-success plus size agreement is the honest,
 * backend-neutral criterion. Used by `make test-display` and CI.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

typedef struct app_state {
    hal_platform *platform;
    hal_window *window;
    hal_surface *surface;
    hal_font *font;
    bool running;
    bool need_redraw;
    bool got_configure;
    bool presented_once;
    hal_size window_size;
    hal_f32 scale;
} app_state;

static void on_event(hal_window *window, const hal_event *event,
                     void *user_data) {
    app_state *app = user_data;

    switch (event->type) {
    case HAL_EVENT_WINDOW_CONFIGURE:
        app->window_size = event->configure.size;
        app->got_configure = true;
        app->need_redraw = true; /* a resize invalidates the frame */
        break;
    case HAL_EVENT_WINDOW_CLOSE_REQUEST:
        app->running = false;
        break;
    case HAL_EVENT_WINDOW_EXPOSE:
        app->need_redraw = true;
        break;
    case HAL_EVENT_WINDOW_FOCUS:
        app->need_redraw = true; /* the title bar hint may change */
        break;
    case HAL_EVENT_KEY_DOWN:
        if (event->key.scancode == HAL_KEY_ESC)
            app->running = false;
        break;
    default:
        break;
    }
    (void)window;
}

/* The M0 placeholder scene: a vertical dusk gradient (deep blue at the
 * bottom fading to soft violet at the top — the Faded Dream palette)
 * and the wordmark. Drawn with raw pixels through the surface info; the
 * real painter arrives with M2. */
static void draw_placeholder(app_state *app) {
    hal_surface_info info;
    if (!hal_ok(hal_surface_info_get(app->surface, &info)))
        return;

    const hal_f32 w = (hal_f32)info.width;
    for (hal_i32 y = 0; y < info.height; y++) {
        hal_f32 t = (hal_f32)y / (hal_f32)(info.height > 1 ? info.height - 1
                                                           : 1);
        /* top: soft violet #8b7ec8 -> bottom: deep dream blue #141a3c */
        hal_f32 r = 0x8b / 255.0f + t * (0x14 / 255.0f - 0x8b / 255.0f);
        hal_f32 g = 0x7e / 255.0f + t * (0x1a / 255.0f - 0x7e / 255.0f);
        hal_f32 b = 0xc8 / 255.0f + t * (0x3c / 255.0f - 0xc8 / 255.0f);
        hal_u32 packed = ((hal_u32)(r * 255.0f + 0.5f) << 16) |
                         ((hal_u32)(g * 255.0f + 0.5f) << 8) |
                         (hal_u32)(b * 255.0f + 0.5f);
        for (hal_i32 x = 0; x < info.width; x++)
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] = packed;
    }

    if (app->font && w > 1.0f) {
        static const char title[] = "halcyon";
        static const char sub[] =
            "M0 \xe2\x80\x94 platform foundation \xc2\xb7 part of the "
            "Faded Dream Project";
        hal_text_metrics tm, ts;
        hal_font_measure(app->font, title, sizeof(title) - 1, &tm);
        hal_font_measure(app->font, sub, sizeof(sub) - 1, &ts);
        hal_i32 cx = (hal_i32)(w * 0.5f);
        hal_i32 mid_y = info.height / 2;
        hal_surface_draw_text(app->surface, app->font, title,
                              sizeof(title) - 1, cx - tm.advance_width / 2,
                              mid_y - 10, hal_color_hex(0xf2eefc), NULL);
        hal_surface_draw_text(app->surface, app->font, sub, sizeof(sub) - 1,
                              cx - ts.advance_width / 2, mid_y + 24,
                              hal_color_rgba8(0xf2, 0xee, 0xfc, 200), NULL);
    }

    hal_surface_invalidate_all(app->surface);
}

int main(int argc, char **argv) {
    bool smoke = false;
    const char *screenshot = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0)
            smoke = true;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshot = argv[++i];
    }

    app_state app;
    memset(&app, 0, sizeof(app));
    app.running = true;
    app.need_redraw = true;
    app.window_size.width = 1280;
    app.window_size.height = 800;
    app.scale = 1.0f;

    hal_platform_options opts = { .app_id = "org.faded-dream.halcyon" };
    hal_result r = hal_platform_init(&opts, &app.platform);
    if (!hal_ok(r)) {
        fprintf(stderr, "halcyon: platform init failed (%d)\n", (int)r);
        return 1;
    }

    hal_window_options wopts = {
        .title = "Halcyon",
        .width = 1280,
        .height = 800,
    };
    r = hal_window_create(app.platform, &wopts, &app.window);
    if (!hal_ok(r)) {
        fprintf(stderr, "halcyon: window create failed (%d)\n", (int)r);
        hal_platform_shutdown(app.platform);
        return 1;
    }

    hal_window_set_event_callback(app.window, on_event, &app);
    r = hal_window_get_surface(app.window, &app.surface);
    if (!hal_ok(r)) {
        fprintf(stderr, "halcyon: surface acquire failed (%d)\n", (int)r);
        hal_window_destroy(app.window);
        hal_platform_shutdown(app.platform);
        return 1;
    }

    app.font = hal_font_system_default(18);
    hal_window_show(app.window);

    /* The frame loop: pump input, draw when there is a frame to draw,
     * present. 15 ms of patience keeps the CPU idle when nothing
     * changes; the whole UI redraws on demand (need_redraw). */
    const int smoke_timeout_ms = 10000;
    int waited = 0;
    while (app.running) {
        int n = hal_platform_pump(app.platform, 15);
        if (n < 0) {
            fprintf(stderr, "halcyon: platform connection lost (%d)\n", n);
            break;
        }
        if (smoke)
            waited += 15;

        if (app.need_redraw && hal_surface_frame_ready(app.surface)) {
            draw_placeholder(&app);
            if (hal_ok(hal_surface_present(app.surface)))
                app.presented_once = true;
            app.need_redraw = false;
        }

        if (smoke) {
            hal_size size_now = { 0, 0 };
            hal_surface_info info_now;
            bool size_ok = hal_ok(hal_window_get_size(app.window, &size_now)) &&
                           hal_ok(hal_surface_info_get(app.surface,
                                                       &info_now)) &&
                           size_now.width == info_now.width &&
                           size_now.height == info_now.height &&
                           size_now.width > 0;
            if (app.presented_once && size_ok) {
                printf("smoke: %dx%d surface drawn and presented, "
                       "configure=%d, scale=%.2f\n",
                       size_now.width, size_now.height,
                       (int)app.got_configure, app.scale);
                if (screenshot) {
                    hal_result ps = hal_surface_save_png(app.surface,
                                                         screenshot);
                    if (hal_ok(ps))
                        printf("smoke: screenshot saved to %s\n", screenshot);
                    else
                        fprintf(stderr, "smoke: screenshot failed (%d)\n",
                                (int)ps);
                }
                app.running = false;
            } else if (waited >= smoke_timeout_ms) {
                fprintf(stderr,
                        "smoke: FAILED (presented=%d size_ok=%d "
                        "configure=%d)\n",
                        (int)app.presented_once, (int)size_ok,
                        (int)app.got_configure);
                hal_font_destroy(app.font);
                hal_window_destroy(app.window);
                hal_platform_shutdown(app.platform);
                return 1;
            }
        }
    }

    hal_font_destroy(app.font);
    hal_window_destroy(app.window);
    hal_platform_shutdown(app.platform);
    return 0;
}
