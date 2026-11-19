# Halcyon — architecture

## The one rule

**Halcyon draws its own user interface.** The platform (FDK) provides
the window, the pixel surface, input events, text rasterization,
clipboard and the event loop — nothing more. Everything the user sees
and touches — widgets, layout, theming, panels, timeline, curve
editor, the 3D viewport — is Halcyon's own code painting pixels.

```
                 src/main.c  (application wiring, the frame loop)
                      |
        +-------------+------------------+------------------+
        |             |                  |                  |
     src/ui/       src/render/        src/core/          src/io/
   own widgets,   CPU 3D rasterizer,  math, scene graph, glTF/OBJ/STL/
   layout, theme  viewport, camera    mesh, undo, project  import/export
        |             |                  |                  |
        +-------------+--------+---------+------------------+
                               |
                        src/platform/            <- THE ONLY FDK CALLER
                        window, surface, text,
                        clipboard, events
                               |
                        FDK (installed, pkg-config fdk)
                               |
                     X11 / Wayland (system)
```

## Modules and their contracts

### src/platform/ — the platform layer

`platform.h` is the public API; `platform.c` is the only translation
unit in the entire repository that includes FDK headers. The Makefile
enforces this mechanically: the FDK include path (`$(FDK_CFLAGS)`) is
given only to `src/platform/*.o` — anywhere else, `#include
"fdk/..."` is a compile error.

The API is a **value-translated mirror**, never a pass-through:

- FDK events arrive as `fdk_event_data` and leave as `hal_event`
  (Halcyon's own tagged union, its own scancode/modifier constants —
  evdev numbering, documented in the header).
- FDK results are mapped through an explicit table
  (`hal_result_from_fdk`); unknown codes degrade to `HAL_ERR_UNKNOWN`.
- Geometry and colors are Halcyon types; FDK structs never escape.

Swapping the platform means rewriting `src/platform/platform.c`
against the new backend and keeping `platform.h` — that is the whole
point of the seam.

**What FDK provides that Halcyon uses:** window lifecycle, the XRGB8888
drawing surface with damage-tracked presentation, frame pacing, the
poll-based event loop, offscreen surfaces (no display connection — the
basis of Halcyon's headless test tier), TrueType text rasterization and
measurement (FDK's public text API exposes it, so Halcyon uses it
rather than growing a second font stack), the text clipboard, and PNG
encoding. Anything Halcyon needs that FDK does not expose is recorded
in docs/FDK-REQUESTS.md with a local fallback.

**What FDK provides that Halcyon does NOT use:** FDK's widget, layout
and theme engines. Halcyon's own UI (src/ui/) exists precisely because
using those would make Halcyon "a themed FDK app", not a from-scratch
suite. The build does not even see FDK's widget headers.

### src/core/ — foundations (M1+)

Math (vec2/3/4, mat4, quaternions — written in-house), the scene
graph, the mesh data structure and editing operations, undo/redo, the
`.halcyon` project format, autosave. No platform or UI dependency
upward: core is pure data + algorithms, fully testable headless.

### src/render/ — the 3D renderer (M3+)

A CPU rasterizer first: depth buffer, shaded (lambert + simple
specular) and wireframe modes, view frustum culling, the grid and axis
gizmo. It renders into a Halcyon surface region and composites under
the UI. The `hal_renderer` interface is written so a GPU
implementation can sit behind it later (FDK's extension API exposes
native handles for GL/Vulkan; see docs/FDK-REQUESTS.md).

### src/ui/ — Halcyon's own toolkit (M2+)

Own painter (rects, AA lines/circles, rounded boxes, text via the
platform wrapper), own widget model (retained tree, damage on
invalidate), own layout engine, own theme files (the dreamy palette),
own focus and input routing, own panels. Public API: `src/ui/ui.h`;
widgets live in separate small sources.

### src/io/ — files (M8+)

Importers/exporters as pure functions over scene data: glTF 2.0/GLB
first-class, then OBJ, STL, FBX if feasible. Round-trip tests are part
of the definition of done.

## The frame loop

Halcyon owns its loop (it is an animation tool — `fdk_run()`-style
blocking is wrong for it):

```
while (running) {
    hal_platform_pump(platform, 15);       /* input, resizes, timers */
    if (!needs_redraw || !hal_surface_frame_ready(surface))
        continue;                          /* compositor-paced       */
    ui_paint(surface);                     /* full-frame repaint     */
    viewport_render(surface, viewport_rect);
    hal_surface_invalidate_all(surface);
    hal_surface_present(surface);          /* only damage travels    */
}
```

M0 redraws on demand (configure/expose/focus). With animation playback
(M7) the loop gains a clock and redraws every paced frame. Full-frame
repaint plus damage-tracked present is the correctness-first choice;
per-region partial redraw is a measured optimization for later — the
surface API already supports it (`hal_surface_invalidate(rect)`).

## HiDPI

`hal_window_get_scale` reports buffer pixels per logical unit. The UI
works in logical coordinates; the painter multiplies by the scale and
fonts are loaded at `px * scale`. The framebuffer itself is physical
(`hal_surface_info`); conversion happens at the platform/painter
boundary, once.

## Testing tiers

1. **Headless** (`make test`): offscreen surfaces and fonts need no
   display, so the painter, layout, input routing (synthetic events),
   math, mesh ops, file round-trips and exporters all run on a bare CI
   box, under ASan+UBSan.
2. **Display** (`make test-display`): starts a private Xvfb and runs
   the window smoke test — a real window opens, draws, presents, and
   its size/geometry are verified. UI interaction tests that need a
   display join this tier as the UI grows.

## Style

C17, GCC 13+. Halcyon follows FDK's conventions: lowercase snake_case,
`hal_` prefix, opaque pointer types, result codes from an enum with
documented failure modes, output-pointer parameters, doc comments on
every public function, `-Wall -Wextra -Wpedantic -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Wconversion
-Wsign-conversion -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
-Werror`. Headers use `#ifndef HAL_*_H` guards. Every module keeps a
public header (`<module>.h`) and, when needed, `*_internal.h` for
implementation-private declarations — the application's equivalent of
FDK's `include/` vs `src/` split.
