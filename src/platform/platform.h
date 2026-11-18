/*
 * platform.h — Halcyon's platform layer (the ONLY module that touches FDK)
 *
 * Halcyon draws its own user interface. This header is the thin, swappable
 * seam between Halcyon and the platform (the Faded Dream ToolKit, "FDK"):
 * windows, the drawing surface, the event loop, text rasterization,
 * clipboard. Everything above src/platform/ — widgets, layout, theming,
 * the 3D viewport, the scene graph — includes Halcyon headers only.
 *
 * Design rules enforced by review (see docs/architecture.md):
 *   - No FDK type appears here or in any other Halcyon header.
 *   - The platform sources are the only files that include FDK
 *     headers (the Makefile gives the FDK include path to
 *     src/platform alone; everywhere else it is a compile error).
 *   - The API is a value-translated mirror: Halcyon events, Halcyon
 *     results, Halcyon geometry — never a pass-through of platform
 *     structs.
 *
 * Text: FDK's public API exposes TrueType rasterization
 * (fdk_text.h), so Halcyon uses it through this wrapper rather than
 * growing its own font stack. Image decode (PNG and friends) is
 * likewise exposed and wrapped here. Anything the platform cannot do
 * is recorded in docs/FDK-REQUESTS.md with a local fallback.
 *
 * Threading: like the underlying platform, everything here is
 * UI-thread-only.
 */

#ifndef HAL_PLATFORM_H
#define HAL_PLATFORM_H

#include "types.h"

/* Opaque handles, declared early so the header orders by concept
 * (lifecycle, windows, events, surfaces, text) instead of by C's
 * declaration-before-use discipline. */
typedef struct hal_surface hal_surface;
typedef struct hal_font hal_font;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ---- */

typedef struct hal_platform hal_platform;

typedef struct hal_platform_options {
    /* Reverse-DNS application identifier (app-id / WM_CLASS). May be
     * NULL for the default "org.faded-dream.halcyon". */
    const char *app_id;
} hal_platform_options;

/* Connects to the desktop (X11 or Wayland — the platform's choice) and
 * creates the Halcyon platform instance. Writes it to *out_platform.
 *
 * Can fail with:
 *   HAL_ERR_INVALID_ARGUMENT - out_platform is NULL
 *   HAL_ERR_OUT_OF_MEMORY
 *   HAL_ERR_PLATFORM_INIT    - the platform backend refused
 *   HAL_ERR_NO_DISPLAY       - no display server reachable
 */
hal_result hal_platform_init(const hal_platform_options *options,
                             hal_platform **out_platform);

/* Tears the platform down: destroys remaining windows and releases the
 * desktop connection. NULL is a safe no-op. */
void hal_platform_shutdown(hal_platform *platform);

/* Runs one event-pump slice: waits up to timeout_ms (0 = poll pending
 * only, negative = block until something arrives), dispatches what came,
 * and returns the number of events dispatched (>= 0) or a negative
 * hal_result when the connection is dead. Halcyon owns its frame loop:
 * pump, decide whether to redraw, draw, present. */
int hal_platform_pump(hal_platform *platform, int timeout_ms);

/* ---- Windows ---- */

typedef struct hal_window hal_window;

typedef struct hal_window_options {
    const char *title;   /* copied; NULL = "Halcyon" */
    hal_i32 width;       /* <= 0 = default 1280 */
    hal_i32 height;      /* <= 0 = default 800  */
} hal_window_options;

/* Creates (unshown) a top-level window. Show it with hal_window_show. */
hal_result hal_window_create(hal_platform *platform,
                             const hal_window_options *options,
                             hal_window **out_window);

void hal_window_show(hal_window *window);
void hal_window_destroy(hal_window *window);
void hal_window_set_title(hal_window *window, const char *title);
void hal_window_resize(hal_window *window, hal_i32 width, hal_i32 height);

/* The window's last-known size (logical units; see hal_window_scale). */
hal_result hal_window_get_size(const hal_window *window, hal_size *out_size);

/* Buffer pixels per logical unit (HiDPI). 1.0 = logical == physical. */
hal_result hal_window_get_scale(const hal_window *window, hal_f32 *out_scale);

/* The window's drawing surface (one per window, owned by the window). */
hal_result hal_window_get_surface(hal_window *window, hal_surface **out_surface);

/* Pointer cursor hint while hovering this window. */
typedef enum hal_cursor {
    HAL_CURSOR_DEFAULT = 0,
    HAL_CURSOR_POINTER = 1,   /* pointing hand over clickable things */
    HAL_CURSOR_TEXT    = 2,   /* I-beam over selectable text */
    HAL_CURSOR_CROSSHAIR = 3, /* precise selection (the 3D viewport) */
    HAL_CURSOR_GRAB    = 4,   /* an orbitable thing can be grabbed */
    HAL_CURSOR_GRABBING = 5,  /* it is grabbed */
} hal_cursor;

void hal_window_set_cursor(hal_window *window, hal_cursor cursor);

/* ---- Events ----
 * Backend-neutral, Halcyon-owned. Coordinates are LOGICAL window-local
 * pixels; the platform layer converts physical input to logical. */

typedef enum hal_event_type {
    HAL_EVENT_WINDOW_CONFIGURE   = 1, /* size assigned; .configure     */
    HAL_EVENT_WINDOW_CLOSE_REQUEST = 2,
    HAL_EVENT_WINDOW_FOCUS       = 3, /* .focus                       */
    HAL_EVENT_WINDOW_EXPOSE      = 4, /* repaint obligation; .expose   */
    HAL_EVENT_WINDOW_STATE       = 5, /* .state                       */
    HAL_EVENT_KEY_DOWN           = 10,
    HAL_EVENT_KEY_UP             = 11,
    HAL_EVENT_POINTER_MOTION     = 20,
    HAL_EVENT_POINTER_BUTTON_DOWN = 21,
    HAL_EVENT_POINTER_BUTTON_UP  = 22,
    HAL_EVENT_POINTER_ENTER      = 23,
    HAL_EVENT_POINTER_LEAVE      = 24,
    HAL_EVENT_POINTER_SCROLL     = 25,
    HAL_EVENT_IME_COMMIT         = 50, /* .ime (input methods)         */
    HAL_EVENT_IME_PREEDIT        = 51,
} hal_event_type;

typedef struct hal_configure_event {
    hal_size size;
} hal_configure_event;

typedef struct hal_focus_event {
    int focused; /* nonzero = gained */
} hal_focus_event;

typedef struct hal_expose_event {
    hal_rect area; /* repaint the whole surface is always safe */
} hal_expose_event;

typedef struct hal_state_event {
    int maximized;
    int minimized;
    int fullscreen;
} hal_state_event;

/* Physical key identity, Linux evdev numbering — layout-stable ("the W
 * position" is the W position on every layout). Use for shortcuts. */
typedef hal_u32 hal_scancode;

#define HAL_KEY_ESC       ((hal_scancode)1)
#define HAL_KEY_BACKSPACE ((hal_scancode)14)
#define HAL_KEY_TAB       ((hal_scancode)15)
#define HAL_KEY_ENTER     ((hal_scancode)28)
#define HAL_KEY_SPACE     ((hal_scancode)57)
#define HAL_KEY_F1        ((hal_scancode)59)
#define HAL_KEY_F2        ((hal_scancode)60)
#define HAL_KEY_F3        ((hal_scancode)61)
#define HAL_KEY_F4        ((hal_scancode)62)
#define HAL_KEY_F5        ((hal_scancode)63)
#define HAL_KEY_F6        ((hal_scancode)64)
#define HAL_KEY_F7        ((hal_scancode)65)
#define HAL_KEY_F8        ((hal_scancode)66)
#define HAL_KEY_F9        ((hal_scancode)67)
#define HAL_KEY_F10       ((hal_scancode)68)
#define HAL_KEY_F11       ((hal_scancode)87)
#define HAL_KEY_F12       ((hal_scancode)88)
#define HAL_KEY_HOME      ((hal_scancode)102)
#define HAL_KEY_END       ((hal_scancode)107)
#define HAL_KEY_PAGE_UP   ((hal_scancode)104)
#define HAL_KEY_PAGE_DOWN ((hal_scancode)109)
#define HAL_KEY_LEFT      ((hal_scancode)105)
#define HAL_KEY_RIGHT     ((hal_scancode)106)
#define HAL_KEY_UP        ((hal_scancode)103)
#define HAL_KEY_DOWN      ((hal_scancode)108)
#define HAL_KEY_DELETE    ((hal_scancode)111)

typedef enum hal_key_modifier {
    HAL_MOD_SHIFT = 1 << 0,
    HAL_MOD_CTRL  = 1 << 1,
    HAL_MOD_ALT   = 1 << 2,
    HAL_MOD_SUPER = 1 << 3,
} hal_key_modifier;

typedef struct hal_key_event {
    hal_scancode scancode;  /* physical key, evdev numbering      */
    hal_u32 codepoint;      /* layout-resolved Unicode, 0 = none */
    hal_u32 modifiers;      /* OR of hal_key_modifier            */
    int is_repeat;          /* KEY_DOWN auto-repeat flag         */
} hal_key_event;

/* Mouse buttons. Values 1..3 are the canonical trio; the platform may
 * report higher indices for side buttons. */
#define HAL_BUTTON_LEFT   1u
#define HAL_BUTTON_MIDDLE 2u
#define HAL_BUTTON_RIGHT  3u

typedef struct hal_pointer_event {
    hal_pointf position; /* logical window-local */
} hal_pointer_event;

typedef struct hal_pointer_button_event {
    hal_pointf position;
    hal_u32 button;     /* HAL_BUTTON_* or a side-button index */
    hal_u32 modifiers;  /* held at press/release              */
} hal_pointer_button_event;

typedef struct hal_scroll_event {
    hal_pointf position;
    hal_f32 delta_x; /* positive = content moves right/down  */
    hal_f32 delta_y;
} hal_scroll_event;

typedef struct hal_ime_event {
    /* NUL-terminated UTF-8, owned by the platform for the callback
     * duration only — copy what must survive. COMMIT never empty;
     * PREEDIT empty clears the composition. */
    const char *text;
    int cursor; /* PREEDIT: byte offset into text */
} hal_ime_event;

typedef struct hal_event {
    hal_event_type type;
    union {
        hal_configure_event configure;
        hal_focus_event focus;
        hal_expose_event expose;
        hal_state_event state;
        hal_key_event key;
        hal_pointer_event pointer;
        hal_pointer_button_event pointer_button;
        hal_scroll_event scroll;
        hal_ime_event ime;
    };
} hal_event;

typedef void (*hal_event_fn)(hal_window *window, const hal_event *event,
                             void *user_data);

/* Registers (or replaces) the window's event callback; NULL drops it. */
void hal_window_set_event_callback(hal_window *window,
                                   hal_event_fn callback, void *user_data);

/* ---- Surfaces ----
 * A surface is a CPU drawing target: XRGB8888 pixels (R<<16 | G<<8 | B,
 * top byte ignored), top-left origin. Window surfaces present to the
 * screen; offscreen surfaces are memory-only and need NO display
 * connection — the entire painter, rasterizer, and UI stack are testable
 * headless on them. */


typedef struct hal_surface_info {
    hal_u32 *pixels;   /* valid until the next info_get/present */
    hal_i32 width;     /* PHYSICAL pixels                       */
    hal_i32 height;
    hal_i32 stride;    /* in hal_u32 units, >= width            */
} hal_surface_info;

/* (Re)acquires the framebuffer. Call once per frame; the pointer is
 * valid until the next acquisition or present. */
hal_result hal_surface_info_get(hal_surface *surface,
                                hal_surface_info *out_info);

/* Sends what changed to the screen. Halcyon paints whole frames and
 * declares full damage (hal_surface_invalidate_all) — the platform
 * transports only what actually changed. */
hal_result hal_surface_present(hal_surface *surface);

/* Frame pacing: false right after a present under a compositor that
 * paces frames; the frame loop skips drawing until it returns true. */
bool hal_surface_frame_ready(hal_surface *surface);

/* Damage declaration for raw pixel writes — REQUIRED after painting
 * through hal_surface_info.pixels, or present will not send the frame. */
void hal_surface_invalidate_all(hal_surface *surface);
void hal_surface_invalidate(hal_surface *surface, hal_rect rect);

/* Offscreen (memory) surface. No display connection needed. Destroy
 * with hal_surface_destroy; window surfaces refuse it (they belong to
 * their window). */
hal_result hal_surface_offscreen_create(hal_i32 width, hal_i32 height,
                                        hal_surface **out_surface);
void hal_surface_destroy(hal_surface *surface);

/* Writes the surface to `path` as an 8-bit RGBA PNG (the platform's
 * exposed encoder, wrapped — preview renders and the test rigs ride
 * this). */
hal_result hal_surface_save_png(const hal_surface *surface, const char *path);

/* ---- Text (FDK's exposed rasterizer, wrapped) ---- */

/* Loads a TrueType/OpenType face at `pixel_size` px of em height.
 * NULL on failure (missing file, not a font, bad size). */
hal_font *hal_font_load(const char *path, hal_i32 pixel_size);

/* Discovers and loads a system UI font (fontconfig, then known paths,
 * then a font scan — the platform's documented chain). NULL when the
 * machine has no usable font. */
hal_font *hal_font_system_default(hal_i32 pixel_size);

void hal_font_destroy(hal_font *font);

typedef struct hal_font_metrics {
    hal_i32 ascent;     /* px above the baseline (positive) */
    hal_i32 descent;    /* px below the baseline (positive) */
    hal_i32 line_height; /* ascent + descent + line gap    */
} hal_font_metrics;

void hal_font_get_metrics(const hal_font *font, hal_font_metrics *out);

typedef struct hal_text_metrics {
    hal_i32 advance_width; /* rounded pen advance          */
    hal_i32 ink_top;       /* <= 0: topmost ink above baseline */
    hal_i32 ink_bottom;    /* >= 0: lowest ink below baseline  */
} hal_text_metrics;

hal_result hal_font_measure(const hal_font *font, const char *utf8,
                            size_t byte_len, hal_text_metrics *out);

/* Draws one line of UTF-8 at (pen_x, baseline_y), clipped to `clip`
 * (NULL = whole surface). Glyphs blend source-over and damage the
 * surface automatically. */
hal_result hal_surface_draw_text(hal_surface *surface, hal_font *font,
                                 const char *utf8, size_t byte_len,
                                 hal_i32 pen_x, hal_i32 baseline_y,
                                 hal_color color, const hal_rect *clip);

/* ---- File dialogs (asynchronous, toolkit-owned windows) ---- */

/* Called once when the dialog closes: `path` is the chosen path (a
 * malloc'd copy the callback OWNS and must free) or NULL when the
 * dialog was cancelled. */
typedef void (*hal_dialog_done_fn)(char *path, void *user_data);

/* Shows an open-file dialog. `filters` is a "; "-free glob list like
 * "*.halcyon" (may be NULL for all files). */
hal_result hal_dialog_open_file(hal_platform *platform, const char *title,
                                const char *filters,
                                hal_dialog_done_fn on_done, void *user_data,
                                hal_window *parent);

/* Shows a save-file dialog (with overwrite confirmation where the
 * platform provides one). */
hal_result hal_dialog_save_file(hal_platform *platform, const char *title,
                                const char *filters,
                                const char *start_name,
                                hal_dialog_done_fn on_done, void *user_data,
                                hal_window *parent);

/* A modal information dialog. */
hal_result hal_dialog_message(hal_platform *platform, hal_window *parent,
                              const char *title, const char *text);

/* ---- Clipboard (text) ---- */

hal_result hal_clipboard_set_text(hal_platform *platform, const char *text);

/* Returns freshly malloc'd UTF-8 (free() it), or NULL when the
 * clipboard holds no text. */
char *hal_clipboard_get_text(hal_platform *platform);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PLATFORM_H */
