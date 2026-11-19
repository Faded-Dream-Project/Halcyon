/*
 * platform.c — Halcyon's platform layer.
 *
 * GPU-ONLY WINDOW (the 2026-11-19 session): Halcyon's editor window
 * is an FDK EXTENSION SURFACE (fdk_extend.h) on an FDK_RENDER_GPU
 * context, with the GL frame callback owning the whole buffer —
 * FDK's app-owned GL API (fdk_gl.h's family). Why not the
 * fdk_gl_viewport widget: on FDK 7fbba8a/X11 the widget's GL content
 * is composited UNDER the CPU frame (pixel-proven — the lane's
 * present draws a fullscreen CPU quad over the region; see
 * docs/FDK-REQUESTS.md R-003), while the ext-surface frame callback
 * path pixel-verified end to end (shaders, VBO/IBO, readback,
 * server-visible output). The frame model stays application-driven:
 * Halcyon presents only when something changed.
 *
 * Frame flow per presented frame: hal_surface_present(ui_surface)
 * [the shell paints the UI into the offscreen UI surface first]
 * -> fdk_surface_present(ext render target)
 * -> FDK's present arbitration (src/platform/x11/x11_surface.c:
 *    "the add-on's GL frame callback owns this window's buffer")
 * -> make_current(app core context) + Halcyon's render callback
 *    (3D scene with the GL 3.3 core renderer, then the UI texture
 *    composited LAST — menus and popups over the viewport by
 *    construction) + FDK swaps.
 *
 * FDK main (3414d94, 2026-11-19) answered the recorded requests; the
 * workarounds this file carried are retired:
 *  - The ext GL frame callback's swapped invocation is FIXED — FDK
 *    now calls the documented fn(surface, user) order on every site
 *    (docs/FDK-REQUESTS.md R-003), and hal_gpu_frame_trampoline
 *    matches it directly.
 *  - The lane still engages lazily: one ordinary fill+present through
 *    the render target must precede fdk_ext_gl_set_frame_callback
 *    (examples/22_ext_complete.c's documented dance), and the
 *    connection's lane loads at the first GPU-mode fdk_window —
 *    hal_platform_init engages through a hidden probe window (which
 *    also reads fdk_gl_status / fdk_window_gl_info for --gpu-info
 *    and the honest startup refusal).
 *  - The Wayland EGL lane loader crash is FIXED (R-007): native
 *    Wayland sessions run the GPU path again; HALCYON_BACKEND
 *    (wayland|x11|auto) remains the explicit override.
 */
#include "platform.h"
#include "gl_loader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* wayland-client.h's generated inline wrappers contain const casts
 * (upstream code) — locally silence -Wcast-qual around them. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <wayland-client.h>
/* Generated from wayland-protocols' stable/xdg-shell/xdg-shell.xml
 * with wayland-scanner (client-header + private-code) — the same
 * protocol FDK's own toplevels speak. Committed because Halcyon's
 * build should not depend on wayland-protocols being installed. */
#include "platform/xdg-shell-client-protocol.h"
#pragma GCC diagnostic pop

#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_gl.h"

/* ---- Internal structs ---- */

struct hal_platform {
    fdk_context *fdk;
    fdk_window *probe;      /* hidden; engages + reports the lane */
    fdk_ext_surface_kind *kind;
    int gl_reason;          /* FDK_GL_REASON_* of the probe engage */
    fdk_gl_info gl_info;    /* flavor/version/vendor/renderer */
    char gl_reason_text[160];
    /* The Wayland kind's native role state (xdg_toplevel). The queue
     * and globals are set up at the first window on a Wayland session
     * (hal_wl_kind_setup, the f-session-lock-raw.c pattern); the role
     * objects live per-surface but Halcyon is a single-window app. */
    fdk_wl_queue *wl_queue;
    struct wl_registry *wl_registry;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *wl_wm_base;
    fdk_ext_surface *wl_ext;
    struct xdg_surface *wl_xdg_surface;
    struct xdg_toplevel *wl_toplevel;
    hal_window *wl_win;     /* close delivery target */
    fdk_u32 wl_compositor_name, wl_wm_base_name;
    fdk_u32 wl_compositor_ver;
    bool wl_have_compositor, wl_have_wm_base;
    bool wl_close_requested;
    bool wl_close_delivered;
    hal_i32 wl_cfg_w, wl_cfg_h; /* last toplevel configure (0=ours) */
    hal_i32 wl_req_w, wl_req_h; /* requested size (0-size fallback) */
    /* --gpu-info identity buffers (Wayland: read at the first window
     * through the engaged context; X11: the probe's fdk_gl_info
     * pointers are lane-owned statics and need no copy). */
    char gl_vendor[64];
    char gl_renderer[128];
};

struct hal_window {
    hal_platform *platform;
    fdk_ext_surface *ext;
    fdk_surface *target;    /* the ext render target (present trig) */
    hal_surface *ui;        /* offscreen; the shell paints HERE */
    hal_event_fn callback;
    void *callback_user;
    hal_gpu_frame_fn gpu_frame;
    void *gpu_frame_user;
    hal_size size;
    hal_f32 scale;
    char title[128];
    unsigned long xid;      /* X11 window (0 on Wayland) */
    bool shown;
    bool close_requested;
};

struct hal_surface {
    fdk_surface *fdk;
    bool window_owned;      /* true: belongs to a hal_window */
    hal_window *ui_owner;   /* set: presenting routes to the GPU
                             * frame (the UI layer of that window) */
};

struct hal_font {
    fdk_font *fdk;
};

/* ---- FDK <-> Halcyon value translation ---- */

static hal_result hal_result_from_fdk(fdk_result r) {
    /* Explicit mapping, not numeric coincidence: if FDK grows a code
     * Halcyon has not named, the default branch fails loudly in the
     * tests rather than aliasing a meaning. */
    switch (r) {
    case FDK_OK: return HAL_OK;
    case FDK_ERR_INVALID_ARGUMENT: return HAL_ERR_INVALID_ARGUMENT;
    case FDK_ERR_OUT_OF_MEMORY: return HAL_ERR_OUT_OF_MEMORY;
    case FDK_ERR_NOT_INITIALIZED: return HAL_ERR_PLATFORM_INIT;
    case FDK_ERR_PLATFORM_INIT: return HAL_ERR_PLATFORM_INIT;
    case FDK_ERR_NO_DISPLAY: return HAL_ERR_NO_DISPLAY;
    case FDK_ERR_WINDOW_CREATE: return HAL_ERR_WINDOW_CREATE;
    case FDK_ERR_UNSUPPORTED: return HAL_ERR_UNSUPPORTED;
    default: return HAL_ERR_PLATFORM;
    }
}

static fdk_color hal_color_to_fdk(hal_color c) {
    return (fdk_color){ c.r, c.g, c.b, c.a };
}

static fdk_rect hal_rect_to_fdk(hal_rect r) {
    return (fdk_rect){ r.x, r.y, r.width, r.height };
}

static hal_rect hal_rect_from_fdk(fdk_rect r) {
    return (hal_rect){ r.x, r.y, r.width, r.height };
}

/* ---- Events (fdk_event_data -> hal_event; the ext surface's stream
 * carries the same shapes a plain window's does) ---- */

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

static void hal_translate_event(fdk_ext_surface *ext,
                                const fdk_event_data *event,
                                void *user_data) {
    (void)ext;
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
        win->size = ev.configure.size;
        hal_window_reflow_ui(win);
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        ev.type = HAL_EVENT_WINDOW_CLOSE_REQUEST;
        win->close_requested = true;
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

/* ---- The ext-surface kind (Halcyon's own native window) ---- */

/* ---- Wayland: the native xdg_toplevel role ----
 *
 * FDK's documented add-on pattern (fdk_extend.h P3 + the
 * f-session-lock-raw.c example): the kind owns a private event queue
 * (fdk_wl_queue_*), binds the globals it needs through its OWN
 * registry listener, and builds the wl_surface + role itself. FDK
 * adopts the surface (ownership transfer), DEFERS every present
 * until the role's first configure is reported via
 * fdk_ext_surface_notify_configure (the present gate — an eager
 * paint cannot trip the "never been configured" protocol error), and
 * its show()/hide() performs the empty state commits. */

static void hal_wl_registry_global(void *data, struct wl_registry *r,
                                   fdk_u32 name, const char *iface,
                                   fdk_u32 version) {
    (void)r;
    hal_platform *p = data;
    if (strcmp(iface, "wl_compositor") == 0) {
        p->wl_compositor_name = name;
        p->wl_compositor_ver = version;
        p->wl_have_compositor = true;
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        p->wl_wm_base_name = name;
        p->wl_have_wm_base = true;
    }
}

static const struct wl_registry_listener hal_wl_registry_listener = {
    .global = hal_wl_registry_global,
};

static void hal_wl_wm_base_ping(void *data, struct xdg_wm_base *base,
                                fdk_u32 serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener hal_wl_wm_base_listener = {
    .ping = hal_wl_wm_base_ping,
};

/* xdg_toplevel::configure carries the size (0 = the compositor
 * leaves it to the client); xdg_surface::configure (the ack serial)
 * follows it — the spec's ordering — so the ack handler reads the
 * size stashed here. */
static void hal_wl_toplevel_configure(void *data,
                                      struct xdg_toplevel *toplevel,
                                      hal_i32 width, hal_i32 height,
                                      struct wl_array *states) {
    (void)toplevel;
    (void)states;
    hal_platform *p = data;
    if (width > 0)
        p->wl_cfg_w = width;
    if (height > 0)
        p->wl_cfg_h = height;
}

static void hal_wl_toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)t;
    hal_platform *p = data;
    p->wl_close_requested = true;
}

static const struct xdg_toplevel_listener hal_wl_toplevel_listener = {
    .configure = hal_wl_toplevel_configure,
    .close = hal_wl_toplevel_close,
};

static void hal_wl_xdg_surface_configure(void *data,
                                         struct xdg_surface *xdg,
                                         fdk_u32 serial) {
    hal_platform *p = data;
    xdg_surface_ack_configure(xdg, serial);
    hal_i32 w = p->wl_cfg_w > 0 ? p->wl_cfg_w : p->wl_req_w;
    hal_i32 h = p->wl_cfg_h > 0 ? p->wl_cfg_h : p->wl_req_h;
    if (p->wl_ext != NULL)
        fdk_ext_surface_notify_configure(p->wl_ext, w, h);
}

static const struct xdg_surface_listener hal_wl_xdg_surface_listener = {
    .configure = hal_wl_xdg_surface_configure,
};

/* Binds the queue + the kind's globals on a Wayland session (the
 * f-session-lock-raw.c pattern: private registry, attach to FDK's
 * pump, pump until the globals arrive, bind by name). Returns false
 * with the reason printed — the honest refusal path. */
static bool hal_wl_kind_setup(hal_platform *p) {
    if (p->wl_queue != NULL)
        return p->wl_compositor != NULL; /* set up before */

    if (!fdk_ext_has_capability(p->fdk, FDK_EXT_CAP_WL_RAW_EVENTS)) {
        fprintf(stderr,
                "halcyon-gpu: FDK reports no Wayland raw-event "
                "capability on this connection\n");
        return false;
    }
    p->wl_queue = fdk_wl_queue_create(p->fdk, "halcyon-kind");
    if (p->wl_queue == NULL) {
        fprintf(stderr,
                "halcyon-gpu: fdk_wl_queue_create refused the "
                "kind's private event queue\n");
        return false;
    }
    p->wl_registry = fdk_wl_queue_registry(p->wl_queue);
    wl_registry_add_listener(p->wl_registry, &hal_wl_registry_listener,
                             p);
    fdk_wl_queue_attach(p->wl_queue, 1);
    for (int i = 0; i < 40 &&
                    !(p->wl_have_compositor && p->wl_have_wm_base);
         i++)
        fdk_pump_events(p->fdk, 16);
    if (!(p->wl_have_compositor && p->wl_have_wm_base)) {
        fprintf(stderr,
                "halcyon-gpu: the compositor does not advertise "
                "wl_compositor + xdg_wm_base (queue never saw both "
                "globals)\n");
        fdk_wl_queue_destroy(p->wl_queue);
        p->wl_queue = NULL;
        p->wl_registry = NULL;
        return false;
    }
    /* Bind at the compositor's offered version, capped at 4 (what
     * wl_compositor_create_surface needs); xdg_wm_base stays at v1 —
     * toplevel + configure + close are all v1 requests/events. */
    fdk_u32 cver = p->wl_compositor_ver < 4 ? p->wl_compositor_ver : 4;
    p->wl_compositor = fdk_wl_raw_bind(
        p->wl_queue, p->wl_compositor_name, &wl_compositor_interface,
        cver);
    p->wl_wm_base = fdk_wl_raw_bind(p->wl_queue, p->wl_wm_base_name,
                                     &xdg_wm_base_interface, 1);
    if (p->wl_compositor == NULL || p->wl_wm_base == NULL) {
        fprintf(stderr,
                "halcyon-gpu: the raw global bind failed "
                "(wl_compositor or xdg_wm_base)\n");
        return false;
    }
    xdg_wm_base_add_listener(p->wl_wm_base, &hal_wl_wm_base_listener,
                             p);
    return true;
}

static fdk_result hal_kind_create(fdk_ext_surface *surface,
                                  fdk_ext_native_handle *out_native,
                                  void *user) {
    hal_platform *p = user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    if (fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_X11) {
        fdk_ext_native_display disp;
        if (fdk_ext_get_native_display(ctx, &disp) != FDK_OK)
            return FDK_ERR_UNSUPPORTED;
        Display *dpy = (Display *)disp.display;
        int screen = DefaultScreen(dpy);
        Window root = RootWindow(dpy, screen);
        XSetWindowAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        attrs.event_mask = ExposureMask | StructureNotifyMask;
        attrs.background_pixel = BlackPixel(dpy, screen);
        Window w = XCreateWindow(dpy, root, 0, 0, 1280, 800, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWEventMask | CWBackPixel, &attrs);
        if (!w)
            return FDK_ERR_WINDOW_CREATE;
        XStoreName(dpy, w, "Halcyon");
        static char res_name[] = "halcyon";
        static char res_class[] = "Halcyon";
        XClassHint ch;
        memset(&ch, 0, sizeof(ch));
        ch.res_name = res_name;
        ch.res_class = res_class;
        XSetClassHint(dpy, w, &ch);
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = PBaseSize | PMinSize;
        sh.min_width = 640;
        sh.min_height = 480;
        sh.base_width = 1280;
        sh.base_height = 800;
        XSetWMNormalHints(dpy, w, &sh);
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)w;
        return FDK_OK;
    }
    /* Wayland: the wl_surface + xdg_toplevel role through the kind's
     * own raw-event queue (globals bound in hal_wl_kind_setup). No
     * commit here — FDK's present gate defers the first attach until
     * the configure is reported, and show() performs the empty
     * state commits. */
    if (p->wl_compositor == NULL || p->wl_wm_base == NULL)
        return FDK_ERR_UNSUPPORTED; /* setup failed; reason printed */
    struct wl_surface *wl = wl_compositor_create_surface(p->wl_compositor);
    if (wl == NULL)
        return FDK_ERR_WINDOW_CREATE;
    struct xdg_surface *xdg =
        xdg_wm_base_get_xdg_surface(p->wl_wm_base, wl);
    struct xdg_toplevel *toplevel =
        xdg != NULL ? xdg_surface_get_toplevel(xdg) : NULL;
    if (xdg == NULL || toplevel == NULL) {
        if (toplevel != NULL)
            xdg_toplevel_destroy(toplevel);
        if (xdg != NULL)
            xdg_surface_destroy(xdg);
        wl_surface_destroy(wl);
        return FDK_ERR_WINDOW_CREATE;
    }
    xdg_toplevel_set_title(toplevel, "Halcyon");
    xdg_toplevel_set_app_id(toplevel, "org.faded-dream.halcyon");
    xdg_toplevel_set_min_size(toplevel, 640, 480);
    xdg_surface_add_listener(xdg, &hal_wl_xdg_surface_listener, p);
    xdg_toplevel_add_listener(toplevel, &hal_wl_toplevel_listener, p);
    p->wl_ext = surface;
    p->wl_xdg_surface = xdg;
    p->wl_toplevel = toplevel;
    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = wl;
    return FDK_OK;
}

/* Role teardown: the toplevel + xdg_surface die with the surface;
 * the wl_surface itself belongs to FDK since adoption. */
static void hal_kind_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    hal_platform *p = user;
    if (p->wl_toplevel != NULL) {
        xdg_toplevel_destroy(p->wl_toplevel);
        p->wl_toplevel = NULL;
    }
    if (p->wl_xdg_surface != NULL) {
        xdg_surface_destroy(p->wl_xdg_surface);
        p->wl_xdg_surface = NULL;
    }
    p->wl_ext = NULL;
}

/* The GL frame callback. FDK main (3414d94) invokes the documented
 * fn(surface, user) order on every arbitration site — the old
 * 7fbba8a swap (FDK-REQUESTS R-003) is fixed, so this matches the
 * public typedef directly. */
static void hal_gpu_frame_trampoline(void *surface, void *user) {
    (void)surface;
    hal_window *win = user;
    if (win && win->gpu_frame)
        win->gpu_frame(win, win->gpu_frame_user);
}

/* ---- Lifecycle ---- */

static const char *gl_reason_name(int reason) {
    switch (reason) {
    case FDK_GL_REASON_OK: return "OK";
    case FDK_GL_REASON_NOT_YET: return "NOT_YET";
    case FDK_GL_REASON_SIZE_GATE: return "SIZE_GATE";
    case FDK_GL_REASON_CPU_MODE: return "CPU_MODE";
    case FDK_GL_REASON_DRIVER: return "DRIVER";
    case FDK_GL_REASON_BACKEND: return "BACKEND";
    case FDK_GL_REASON_DOWNGRADED: return "DOWNGRADED";
    default: return "?";
    }
}

const char *hal_platform_gl_reason_text(const hal_platform *p) {
    if (!p)
        return "<no platform>";
    return p->gl_reason_text;
}

const char *hal_platform_gl_info_text(const hal_platform *p,
                                      char *buf, size_t n) {
    if (!p || !buf || n == 0)
        return buf ? buf : "";
    snprintf(buf, n, "GL %s %d.%d — %s / %s", p->gl_info.flavor,
             p->gl_info.version_major, p->gl_info.version_minor,
             p->gl_info.vendor, p->gl_info.renderer);
    return buf;
}

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
    /* GPU-ONLY: the classic framebuffer present path is disabled for
     * the context's windows; a refused GL stack fails window creation
     * instead of silently falling back (fdk_core.h:53-79). */
    fdk_opts.render_mode = FDK_RENDER_GPU;

    /* Backend policy: FDK main (3414d94) fixed the Wayland EGL lane
     * loader crash (FDK-REQUESTS R-007 — eglGetProcAddress was called
     * before it was resolved; the session-8 forced-X11 default is
     * retired). AUTO is the default again: native Wayland on Wayland
     * sessions, X11 otherwise. HALCYON_BACKEND (wayland|x11) remains
     * the explicit override for testing. */
    {
        const char *be = getenv("HALCYON_BACKEND");
        if (be != NULL && strcmp(be, "wayland") == 0) {
            fdk_opts.backend = FDK_PLATFORM_WAYLAND;
        } else if (be != NULL && strcmp(be, "x11") == 0) {
            fdk_opts.backend = FDK_PLATFORM_X11;
        }
    }

    fdk_result r = fdk_init(&p->fdk, &fdk_opts);
    if (!fdk_ok(r)) {
        free(p);
        return hal_result_from_fdk(r);
    }

    /* Engage the lane through a hidden probe window BEFORE anything
     * else: the connection's GL lane loads at the first GPU-mode
     * window, and the probe's fdk_gl_status / fdk_window_gl_info are
     * the honest startup verdict + the --gpu-info identity.
     *
     * X11 ONLY: a hidden Wayland window never receives its xdg
     * configure, so it never has a size — pixel_area 0 against the
     * 131072-px size gate, NOT_YET forever. On Wayland the verdict
     * and the identity move to the first REAL window: the lane now
     * engages ON DEMAND at the ext surface (FDK R-001 fixed), the
     * fill+present dance + frame-callback registration there are the
     * honest startup gates, and hal_window_create reads the identity
     * through the engaged context (fdk_ext_gl_make_current +
     * glGetString) for --gpu-info. */
    if (fdk_ext_active_backend(p->fdk) == FDK_EXT_BACKEND_WAYLAND) {
        p->gl_reason = FDK_GL_REASON_NOT_YET; /* verdict at 1st window */
        snprintf(p->gl_reason_text, sizeof(p->gl_reason_text),
                 "the Wayland lane engages at the first window "
                 "(windows size at configure — a hidden probe cannot "
                 "engage)");
        *out_platform = p;
        return HAL_OK;
    }
    fdk_window_options po;
    memset(&po, 0, sizeof(po));
    po.title = "halcyon-gl-probe";
    /* >= FDK_GL_MIN_PIXELS (131072 px): a window under the size gate
     * never engages the lane (fdk_gl.h:88) — 640x320 = 204800 px. */
    po.width = 640;
    po.height = 320;
    fdk_window *probe = NULL;
    r = fdk_window_create(p->fdk, &po, &probe);
    if (!fdk_ok(r)) {
        snprintf(p->gl_reason_text, sizeof(p->gl_reason_text),
                 "fdk_window_create (GPU mode) refused: %d — the GL "
                 "stack could not initialize (see the FDK log lines "
                 "above)",
                 (int)r);
        fprintf(stderr, "halcyon-gpu: %s\n", p->gl_reason_text);
        fdk_shutdown(p->fdk);
        free(p);
        return HAL_ERR_PLATFORM_INIT;
    }
    p->probe = probe;
    (void)fdk_window_gl_engage(probe);
    fdk_gl_status_data st;
    memset(&st, 0, sizeof(st));
    (void)fdk_gl_status(probe, &st);
    p->gl_reason = (int)st.reason;
    {
        fdk_gl_info gi;
        memset(&gi, 0, sizeof(gi));
        (void)fdk_window_gl_info(probe, &gi);
        p->gl_info = gi; /* struct copy; strings are lane-owned */
    }
    snprintf(p->gl_reason_text, sizeof(p->gl_reason_text),
             "fdk_gl_status: %s (pixel_area %lld, size_gate %lld, "
             "force %d, cpu_mode %d)",
             gl_reason_name(p->gl_reason),
             (long long)st.pixel_area, (long long)st.size_gate,
             st.force_override, st.cpu_mode);
    if (p->gl_reason != FDK_GL_REASON_OK) {
        fprintf(stderr,
                "halcyon-gpu: the GL viewport cannot engage — %s. "
                "Halcyon is GPU-only (no CPU fallback); exiting.\n",
                p->gl_reason_text);
        fdk_shutdown(p->fdk);
        free(p);
        return HAL_ERR_PLATFORM_INIT;
    }

    *out_platform = p;
    return HAL_OK;
}

void hal_platform_shutdown(hal_platform *platform) {
    if (!platform)
        return;
    if (platform->kind) {
        fdk_ext_kind_destroy(platform->kind);
        platform->kind = NULL;
    }
    /* The kind's raw-event queue dies with the context's protocol
     * machinery: AFTER the kind (its surfaces are gone — the destroy
     * callbacks ran), BEFORE fdk_shutdown tears down the connection.
     * The raw-bound globals are ADD-ON owned (fdk_wl_raw_bind's
     * contract) — destroy them first or they leak (LSan under the
     * weston rig caught exactly 2 × 96-byte wl_proxy objects). */
    if (platform->wl_queue != NULL) {
        if (platform->wl_compositor != NULL) {
            wl_proxy_destroy((struct wl_proxy *)platform->wl_compositor);
            platform->wl_compositor = NULL;
        }
        if (platform->wl_wm_base != NULL) {
            wl_proxy_destroy((struct wl_proxy *)platform->wl_wm_base);
            platform->wl_wm_base = NULL;
        }
        fdk_wl_queue_destroy(platform->wl_queue);
        platform->wl_queue = NULL;
        platform->wl_registry = NULL;
        platform->wl_ext = NULL;
        platform->wl_win = NULL;
    }
    if (platform->probe)
        fdk_window_destroy(platform->probe);
    fdk_shutdown(platform->fdk);
    free(platform);
}

int hal_platform_pump(hal_platform *platform, int timeout_ms) {
    if (!platform)
        return HAL_ERR_INVALID_ARGUMENT;
    int n = fdk_pump_events(platform->fdk, timeout_ms);
    if (n < 0)
        return hal_result_from_fdk((fdk_result)n);
    /* xdg_toplevel::close (the Wayland kind's own queue, dispatched
     * by the pump above) → the same HAL_EVENT_WINDOW_CLOSE the X11
     * path delivers through FDK's translated event stream. */
    if (platform->wl_close_requested && !platform->wl_close_delivered &&
        platform->wl_win != NULL) {
        platform->wl_close_delivered = true;
        hal_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = HAL_EVENT_WINDOW_CLOSE_REQUEST;
        hal_dispatch(platform->wl_win, &ev);
    }
    return n;
}

/* ---- Windows (ext surfaces) ---- */

/* (Re)create the offscreen UI layer at the window's size: an
 * ARGB8888 surface — the viewport hole stays transparent (alpha 0)
 * so the GL scene shows through the composite; panels and overlays
 * paint alpha 255. */
void hal_window_reflow_ui(hal_window *window) {
    if (!window || window->size.width <= 0 || window->size.height <= 0)
        return;
    hal_i32 w = window->size.width;
    hal_i32 h = window->size.height;
    if (window->ui && window->ui->fdk) {
        fdk_surface_info info;
        memset(&info, 0, sizeof(info));
        if (fdk_ok(fdk_surface_get_info(window->ui->fdk, &info)) &&
            (hal_i32)info.width == w && (hal_i32)info.height == h)
            return; /* unchanged */
        hal_surface_destroy(window->ui);
        window->ui = NULL;
    }
    hal_surface *ui = NULL;
    if (!hal_ok(hal_surface_offscreen_create_argb(w, h, &ui)))
        return;
    ui->ui_owner = window;
    window->ui = ui;
}

hal_result hal_window_create(hal_platform *platform,
                             const hal_window_options *options,
                             hal_window **out_window) {
    if (!platform || !out_window)
        return HAL_ERR_INVALID_ARGUMENT;

    if (!hal_gl_load()) {
        fprintf(stderr,
                "halcyon-gpu: the GL function loader failed — the "
                "GPU-only viewport cannot run. Exiting.\n");
        return HAL_ERR_UNSUPPORTED;
    }

    if (!platform->kind) {
        /* On a Wayland session the kind needs its raw-event queue +
         * globals BEFORE the first create() (hal_wl_kind_setup; on X11
         * everything resolves at create time). */
        if (fdk_ext_active_backend(platform->fdk) ==
                FDK_EXT_BACKEND_WAYLAND &&
            !hal_wl_kind_setup(platform))
            return HAL_ERR_UNSUPPORTED;
        fdk_ext_surface_ops ops;
        memset(&ops, 0, sizeof(ops));
        ops.struct_size = sizeof(ops);
        ops.create = hal_kind_create;
        ops.destroy = hal_kind_destroy; /* Wayland role teardown */
        if (!fdk_ok(fdk_ext_register_kind(platform->fdk, "halcyon-editor",
                                          &ops, platform, &platform->kind))) {
            fprintf(stderr,
                    "halcyon-gpu: fdk_ext_register_kind failed\n");
            return HAL_ERR_WINDOW_CREATE;
        }
    }

    hal_window *win = calloc(1, sizeof(*win));
    if (!win)
        return HAL_ERR_OUT_OF_MEMORY;
    win->platform = platform;
    win->size.width = options && options->width > 0 ? options->width : 1280;
    win->size.height = options && options->height > 0 ? options->height : 800;
    snprintf(win->title, sizeof(win->title), "%s",
             options && options->title ? options->title : "Halcyon");
    win->scale = 1.0f; /* X11: no scale concept (fdk docs); Wayland
                        * reads the live buffer scale — R-004. */
    platform->wl_req_w = win->size.width;  /* the 0-size configure */
    platform->wl_req_h = win->size.height; /* fallback size */
    platform->wl_win = win;

    fdk_result r = fdk_ext_surface_create(platform->kind, &win->ext);
    if (!fdk_ok(r)) {
        free(win);
        return hal_result_from_fdk(r);
    }
    fdk_ext_surface_set_event_callback(win->ext, hal_translate_event, win);
    if (platform->wl_toplevel != NULL) /* the real title (not the
                                        * create-time default) */
        xdg_toplevel_set_title(platform->wl_toplevel, win->title);

    if (!fdk_ok(fdk_ext_surface_get_surface(win->ext, &win->target)) ||
        !win->target) {
        fdk_ext_surface_destroy(win->ext);
        free(win);
        return HAL_ERR_WINDOW_CREATE;
    }

    /* The engagement dance (examples/22_ext_complete.c): one ordinary
     * fill+present makes the lane serve THIS window, after which the
     * GL frame callback can be installed. */
    (void)fdk_surface_fill(win->target,
                           (fdk_color){ 0.063f, 0.078f, 0.18f, 1.0f });
    r = fdk_surface_present(win->target);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "halcyon-gpu: the first present failed (%d) — the GL "
                "lane refused this window: %s. Halcyon is GPU-only; "
                "exiting.\n",
                (int)r, platform->gl_reason_text);
        fdk_ext_surface_destroy(win->ext);
        free(win);
        return hal_result_from_fdk(r);
    }

    r = fdk_ext_gl_set_frame_callback(
        win->ext, (fdk_ext_gl_frame_fn)hal_gpu_frame_trampoline, win);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "halcyon-gpu: fdk_ext_gl_set_frame_callback refused "
                "(%d) — %s. Halcyon is GPU-only; exiting.\n",
                (int)r, platform->gl_reason_text);
        fdk_ext_surface_destroy(win->ext);
        free(win);
        return hal_result_from_fdk(r);
    }

    /* Wayland: the startup verdict + the --gpu-info identity live HERE
     * (the probe path's comment in hal_platform_init). The lane just
     * engaged for this surface — read the identity through the live
     * context; a refusal above already exited honestly. */
    if (platform->wl_toplevel != NULL) {
        r = fdk_ext_gl_make_current(win->ext);
        if (!fdk_ok(r)) {
            fprintf(stderr,
                    "halcyon-gpu: fdk_ext_gl_make_current refused (%d) "
                    "after engagement — Halcyon is GPU-only; "
                    "exiting.\n",
                    (int)r);
            fdk_ext_surface_destroy(win->ext);
            free(win);
            return hal_result_from_fdk(r);
        }
        const char *vendor = hal_gl.get_string(0x1F00 /* GL_VENDOR */);
        const char *renderer = hal_gl.get_string(0x1F01 /* GL_RENDERER */);
        const char *version = hal_gl.get_string(0x1F02 /* GL_VERSION */);
        snprintf(platform->gl_vendor, sizeof(platform->gl_vendor), "%s",
                 vendor ? vendor : "?");
        snprintf(platform->gl_renderer, sizeof(platform->gl_renderer),
                 "%s", renderer ? renderer : "?");
        int vmaj = 0, vmin = 0;
        if (version != NULL)
            (void)sscanf(version, "%d.%d", &vmaj, &vmin);
        /* flavor vocabulary per fdk_gl.h: core / compat / gles2 */
        const char *flavor = "core";
        if (version != NULL) {
            if (strstr(version, "Compatibility") != NULL)
                flavor = "compat";
            else if (strstr(version, "ES") != NULL)
                flavor = "gles2";
        }
        snprintf(platform->gl_info.flavor,
                 sizeof(platform->gl_info.flavor), "%s", flavor);
        platform->gl_info.version_major = vmaj;
        platform->gl_info.version_minor = vmin;
        platform->gl_info.vendor = platform->gl_vendor;
        platform->gl_info.renderer = platform->gl_renderer;
        platform->gl_info.version = version; /* context-owned static */
        platform->gl_reason = FDK_GL_REASON_OK;
        snprintf(platform->gl_reason_text,
                 sizeof(platform->gl_reason_text),
                 "engaged at the first window (Wayland)");
    }

    hal_window_reflow_ui(win);
    if (!win->ui) {
        fdk_ext_surface_destroy(win->ext);
        free(win);
        return HAL_ERR_OUT_OF_MEMORY;
    }

    *out_window = win;
    return HAL_OK;
}

void hal_window_show(hal_window *window) {
    if (!window)
        return;
    fdk_ext_surface_show(window->ext);
    window->shown = true;
}

void hal_window_destroy(hal_window *window) {
    if (!window)
        return;
    if (window->ui)
        hal_surface_destroy(window->ui);
    /* Detach the kind's close-delivery target BEFORE the free (the
     * pump reads it; hal_kind_destroy — via fdk_ext_surface_destroy —
     * already dropped wl_ext/wl_toplevel). */
    if (window->platform->wl_win == window) {
        window->platform->wl_win = NULL;
        window->platform->wl_close_requested = false;
        window->platform->wl_close_delivered = false;
    }
    fdk_ext_surface_destroy(window->ext);
    free(window);
}

void hal_window_set_title(hal_window *window, const char *title) {
    if (!window || !title)
        return;
    snprintf(window->title, sizeof(window->title), "%s", title);
    if (window->platform->wl_toplevel != NULL) {
        /* Wayland: the role owns the title (takes effect on the next
         * commit — FDK's presents commit). */
        xdg_toplevel_set_title(window->platform->wl_toplevel,
                               window->title);
        return;
    }
    /* X11: the live native handle (window->xid is 0 until probed —
     * fdk_ext_surface_native is the authoritative query). */
    fdk_ext_native_handle h;
    memset(&h, 0, sizeof(h));
    fdk_ext_native_display disp;
    if (fdk_ok(fdk_ext_surface_native(window->ext, &h)) &&
        h.backend == FDK_EXT_BACKEND_X11 &&
        fdk_ok(fdk_ext_get_native_display(window->platform->fdk, &disp)))
        XStoreName((Display *)disp.display,
                   (unsigned long)h.xid, window->title);
}

void hal_window_resize(hal_window *window, hal_i32 width, hal_i32 height) {
    if (!window || width <= 0 || height <= 0)
        return;
    if (window->platform->wl_toplevel != NULL) {
        /* Wayland sizes are compositor-driven (xdg_toplevel has no
         * client resize request): ask via a windowed set of bounds
         * is not part of v1 — the honest no-op, the size follows the
         * next configure. */
        window->size.width = width;
        window->size.height = height;
        hal_window_reflow_ui(window);
        return;
    }
    fdk_ext_native_handle h;
    memset(&h, 0, sizeof(h));
    fdk_ext_native_display disp;
    if (fdk_ok(fdk_ext_surface_native(window->ext, &h)) &&
        h.backend == FDK_EXT_BACKEND_X11 &&
        fdk_ok(fdk_ext_get_native_display(window->platform->fdk, &disp))) {
        Display *dpy = (Display *)disp.display;
        XResizeWindow(dpy, (unsigned long)h.xid,
                      (unsigned)width, (unsigned)height);
        XFlush(dpy);
    }
    window->size.width = width;
    window->size.height = height;
    hal_window_reflow_ui(window);
}

hal_result hal_window_get_size(const hal_window *window, hal_size *out_size) {
    if (!window || !out_size)
        return HAL_ERR_INVALID_ARGUMENT;
    *out_size = window->size;
    return HAL_OK;
}

hal_result hal_window_get_scale(const hal_window *window, hal_f32 *out_scale) {
    if (!window || !out_scale)
        return HAL_ERR_INVALID_ARGUMENT;
    *out_scale = window->scale;
    return HAL_OK;
}

hal_result hal_window_get_render_backend(const hal_window *window,
                                         hal_render_backend *out) {
    if (!window || !out)
        return HAL_ERR_INVALID_ARGUMENT;
    /* The GPU-only window presents through the GL frame callback on
     * the engaged lane — OpenGL by construction (the platform
     * refused to init otherwise). */
    *out = HAL_RENDER_BACKEND_OPENGL;
    return HAL_OK;
}

const char *hal_window_render_device(const hal_window *window) {
    if (!window || !window->platform)
        return "<none>";
    return window->platform->gl_info.renderer[0]
               ? window->platform->gl_info.renderer
               : "<unknown>";
}

hal_result hal_window_get_surface(hal_window *window,
                                  hal_surface **out_surface) {
    if (!window || !out_surface)
        return HAL_ERR_INVALID_ARGUMENT;
    /* The shell paints the UI into the offscreen surface; presenting
     * it triggers the GPU frame (see hal_surface_present). */
    *out_surface = window->ui;
    return window->ui ? HAL_OK : HAL_ERR_UNSUPPORTED;
}

void hal_window_set_gpu_frame_callback(hal_window *window,
                                       hal_gpu_frame_fn callback,
                                       void *user_data) {
    if (!window)
        return;
    window->gpu_frame = callback;
    window->gpu_frame_user = user_data;
}

/* The window's native X11 id (0 on Wayland / not yet created) —
 * for tests that verify SERVER-side pixels with XGetImage. */
unsigned long hal_window_native_xid(const hal_window *window) {
    if (!window || !window->ext)
        return 0;
    fdk_ext_native_handle h;
    memset(&h, 0, sizeof(h));
    if (!fdk_ok(fdk_ext_surface_native(window->ext, &h)) ||
        h.backend != FDK_EXT_BACKEND_X11)
        return 0;
    return (unsigned long)h.xid;
}

bool hal_window_close_requested(const hal_window *window) {
    return window ? window->close_requested : false;
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
    fdk_ext_surface_set_cursor(window->ext, shape);
}

void hal_window_set_event_callback(hal_window *window,
                                   hal_event_fn callback,
                                   void *user_data) {
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
    if (surface->ui_owner) {
        /* The UI layer of a GPU window: presenting drives the GPU
         * frame — the ext render target's present runs the GL frame
         * callback (which composites this surface's pixels as the
         * LAST pass) and swaps. */
        return hal_result_from_fdk(
            fdk_surface_present(surface->ui_owner->target));
    }
    return hal_result_from_fdk(fdk_surface_present(surface->fdk));
}

bool hal_surface_frame_ready(hal_surface *surface) {
    if (!surface)
        return false;
    if (surface->ui_owner)
        return fdk_surface_frame_ready(surface->ui_owner->target);
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

hal_result hal_surface_offscreen_create_argb(hal_i32 width,
                                             hal_i32 height,
                                             hal_surface **out_surface) {
    if (!out_surface)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_surface *fdk_surf = NULL;
    fdk_result r = fdk_surface_create_format(
        width, height, FDK_SURFACE_FORMAT_ARGB8888, &fdk_surf);
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

hal_result hal_surface_from_image(const char *path,
                                  hal_surface **out_surface) {
    if (!path || !out_surface)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_surface *fdk_surf = NULL;
    fdk_result r = fdk_surface_create_from_image(path, &fdk_surf);
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

hal_result hal_surface_save_png(const hal_surface *surface,
                                const char *path) {
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

/* ---- File dialogs ---- */

static void hal_dialog_bridge(const fdk_file_dialog_result *result,
                              void *user_data);

typedef struct hal_dialog_req {
    hal_dialog_done_fn on_done;
    void *user_data;
} hal_dialog_req;

static void hal_dialog_bridge(const fdk_file_dialog_result *result,
                              void *user_data) {
    hal_dialog_req *req = user_data;
    char *path = NULL;
    if (result->outcome == FDK_FILE_DIALOG_ACCEPTED && result->count > 0 &&
        result->paths[0]) {
        size_t n = strlen(result->paths[0]) + 1;
        path = malloc(n);
        if (path)
            memcpy(path, result->paths[0], n);
    }
    req->on_done(path, req->user_data);
    free(req);
}

static hal_result dialog_show(hal_platform *platform, fdk_window *parent,
                              const char *title, const char *filters,
                              hal_dialog_done_fn on_done, void *user_data,
                              bool save, const char *start_name) {
    if (!platform || !on_done)
        return HAL_ERR_INVALID_ARGUMENT;

    hal_dialog_req *req = malloc(sizeof(*req));
    if (!req)
        return HAL_ERR_OUT_OF_MEMORY;
    req->on_done = on_done;
    req->user_data = user_data;

    fdk_file_dialog_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.title = title;
    opts.kind = save ? FDK_FILE_DIALOG_SAVE_FILE : FDK_FILE_DIALOG_OPEN_FILE;
    opts.filters = filters;
    opts.start_name = start_name;
    opts.parent = parent;

    fdk_result r = fdk_dialog_open_file(platform->fdk, &opts,
                                        hal_dialog_bridge, req, NULL);
    if (!fdk_ok(r)) {
        free(req);
        return hal_result_from_fdk(r);
    }
    return HAL_OK;
}

hal_result hal_dialog_open_file(hal_platform *platform, const char *title,
                                const char *filters,
                                hal_dialog_done_fn on_done, void *user_data,
                                hal_window *parent) {
    (void)parent; /* dialogs parent to the hidden probe window */
    return dialog_show(platform, platform ? platform->probe : NULL, title,
                       filters, on_done, user_data, false, NULL);
}

hal_result hal_dialog_save_file(hal_platform *platform, const char *title,
                                const char *filters,
                                const char *start_name,
                                hal_dialog_done_fn on_done, void *user_data,
                                hal_window *parent) {
    (void)parent;
    return dialog_show(platform, platform ? platform->probe : NULL, title,
                       filters, on_done, user_data, true, start_name);
}

hal_result hal_dialog_message(hal_platform *platform, hal_window *parent,
                              const char *title, const char *text) {
    (void)parent;
    if (!platform)
        return HAL_ERR_INVALID_ARGUMENT;
    fdk_dialog_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.title = title;
    opts.text = text;
    opts.parent = platform->probe;
    fdk_dialog_show_message(platform->fdk, &opts, NULL, NULL, NULL);
    return HAL_OK;
}
