/*
 * main.c — Halcyon application entry
 *
 * Owns the frame loop (decision D-004): pump events, paint when there
 * is something to paint, present. The editor shell (src/shell.c) holds
 * the UI, the scene and the project state.
 *
 * --smoke: automated window test — opens, paints one frame, presents,
 * verifies geometry, exits 0 (1 on timeout). --screenshot <path>
 * saves the presented frame as PNG (used by the display tier and by
 * visual verification during development).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"
#include "shell.h"

typedef struct app_frame {
    hal_platform *platform;
    hal_window *window;
    hal_surface *surface;
    hal_app *app;
    bool running;
    bool need_redraw;
    bool presented_once;
    double last_autosave_tick;
} app_frame;

static void on_event(hal_window *window, const hal_event *event,
                     void *user_data) {
    app_frame *frame = user_data;

    switch (event->type) {
    case HAL_EVENT_WINDOW_CLOSE_REQUEST:
        hal_app_quit(frame->app);
        break;
    case HAL_EVENT_WINDOW_CONFIGURE:
    case HAL_EVENT_WINDOW_EXPOSE:
    case HAL_EVENT_WINDOW_FOCUS:
        frame->need_redraw = true;
        break;
    default:
        break;
    }

    /* Everything else flows through the shell (UI first, then
     * app-level shortcuts). Events that change UI state mark the UI
     * dirty; the loop checks both flags. */
    hal_app_handle_event(frame->app, event);
    if (hal_ui_needs_paint(hal_app_ui(frame->app)))
        frame->need_redraw = true;
    (void)window;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    bool smoke = false;
    const char *screenshot = NULL;
    bool ui_audit = false;
    bool zorder_test = false;
    bool entries_test = false;
    int audit_w = 1920, audit_h = 1080;
    int win_w = 1280, win_h = 800; /* --size overrides any mode */
    hal_f32 audit_scale = 0.0f; /* 0 = the window's real scale */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0)
            smoke = true;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshot = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            win_w = atoi(argv[++i]);
            win_h = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--test-zorder") == 0) {
            zorder_test = true;
        } else if (strcmp(argv[i], "--test-entries") == 0) {
            entries_test = true;
        } else if (strcmp(argv[i], "--ui-audit") == 0) {
            ui_audit = true;
            if (i + 1 < argc)
                audit_w = atoi(argv[++i]);
            if (i + 1 < argc)
                audit_h = atoi(argv[++i]);
            if (i + 1 < argc)
                audit_scale = (hal_f32)atof(argv[++i]);
        }
    }

    app_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.running = true;
    frame.need_redraw = true;

    hal_platform_options opts = { .app_id = "org.faded-dream.halcyon" };
    if (!hal_ok(hal_platform_init(&opts, &frame.platform))) {
        fprintf(stderr, "halcyon: platform init failed\n");
        return 1;
    }

    hal_window_options wopts = {
        .title = "Halcyon",
        .width = ui_audit ? audit_w : win_w,
        .height = ui_audit ? audit_h : win_h,
    };
    if (!hal_ok(hal_window_create(frame.platform, &wopts, &frame.window))) {
        fprintf(stderr, "halcyon: window create failed\n");
        hal_platform_shutdown(frame.platform);
        return 1;
    }
    if (!hal_ok(hal_window_get_surface(frame.window, &frame.surface))) {
        fprintf(stderr, "halcyon: surface acquire failed\n");
        hal_window_destroy(frame.window);
        hal_platform_shutdown(frame.platform);
        return 1;
    }

    frame.app = hal_app_create(frame.platform, frame.window);
    if (!frame.app) {
        fprintf(stderr, "halcyon: editor shell init failed\n");
        hal_window_destroy(frame.window);
        hal_platform_shutdown(frame.platform);
        return 1;
    }

    hal_window_set_event_callback(frame.window, on_event, &frame);
    hal_window_show(frame.window);

    const int smoke_timeout_ms = 10000;
    int waited_ms = 0;
    int audit_exit_code = 0; /* set by --ui-audit (2 = violations) */
    while (hal_app_running(frame.app)) {
        int n = hal_platform_pump(frame.platform, 15);
        if (n < 0) {
            fprintf(stderr, "halcyon: platform connection lost\n");
            break;
        }

        /* Playback + autosave ride the frame loop's clock. */
        double now = now_seconds();
        hal_app_tick(frame.app, now);
        if (now - frame.last_autosave_tick > 1.0) {
            frame.last_autosave_tick = now;
            hal_autosave_tick(hal_app_autosave(frame.app),
                              hal_app_scene(frame.app), now);
        }

        if (frame.need_redraw || hal_ui_needs_paint(hal_app_ui(frame.app))) {
            if (hal_surface_frame_ready(frame.surface)) {
                hal_app_paint(frame.app, frame.surface);
                frame.presented_once = true;
                frame.need_redraw = false;
            }
        }

        if (entries_test && frame.presented_once) {
            int rc = hal_app_run_entries_test(frame.app,
                                              frame.surface);
            audit_exit_code = rc == 0 ? 0 : 3;
            hal_app_quit(frame.app);
        }

        if (zorder_test && frame.presented_once) {
            int rc = hal_app_run_zorder_test(frame.app,
                                             frame.surface);
            audit_exit_code = rc == 0 ? 0 : 3;
            hal_app_quit(frame.app);
        }

        if (ui_audit && frame.presented_once) {
            int violations = hal_app_run_ui_audit(frame.app,
                                                  frame.surface,
                                                  audit_scale);
            printf("ui-audit: %d violation(s) at %dx%d scale %.2f\n",
                   violations, audit_w, audit_h,
                   audit_scale > 0 ? audit_scale : 1.0f);
            audit_exit_code = violations > 0 ? 2 : 0;
            hal_app_quit(frame.app);
        }

        if (smoke) {
            waited_ms += 15;
            hal_size size_now = { 0, 0 };
            bool size_ok =
                hal_ok(hal_window_get_size(frame.window, &size_now)) &&
                size_now.width > 0;
            if (frame.presented_once && size_ok) {
                hal_rectf vp = hal_app_viewport_rect(frame.app);
                hal_f32 sc = 1.0f;
                hal_window_get_scale(frame.window, &sc);
                printf("smoke: %dx%d window painted and presented; "
                       "viewport %.0fx%.0f scale=%.2f\n",
                       size_now.width, size_now.height, vp.width,
                       vp.height, sc);
                if (screenshot) {
                    if (hal_ok(hal_surface_save_png(frame.surface,
                                                    screenshot)))
                        printf("smoke: screenshot saved to %s\n",
                               screenshot);
                    else
                        fprintf(stderr, "smoke: screenshot failed\n");
                }
                hal_app_quit(frame.app);
            } else if (waited_ms >= smoke_timeout_ms) {
                fprintf(stderr, "smoke: FAILED (presented=%d size_ok=%d)\n",
                        (int)frame.presented_once, (int)size_ok);
                hal_app_destroy(frame.app);
                hal_window_destroy(frame.window);
                hal_platform_shutdown(frame.platform);
                return 1;
            }
        }
    }

    hal_app_destroy(frame.app);
    hal_window_destroy(frame.window);
    hal_platform_shutdown(frame.platform);
    return audit_exit_code;
}
