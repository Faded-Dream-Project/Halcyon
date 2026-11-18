# Halcyon — PROGRESS (the honest ledger)

> The rule this file lives by: **nothing is listed as working unless it
> builds and has been tested.** Everything not yet implemented is listed
> as not implemented. This file is the fast, authoritative answer to
> "what does Halcyon actually do today?"

## Roadmap

- **M0 — repo foundation**: Makefile, layout, README, LICENSE
  placeholder, CHANGELOG, PROGRESS, docs/, tests/, CI; a window opens
  through the platform module.
- **M1 — core**: math library, scene graph, undo/redo, `.halcyon`
  project format (documented in docs/), autosave.
- **M2 — own UI system**: draw primitives and text, widget set,
  layout engine, panels and docking-lite, theme files, focus and input
  routing, HiDPI; headless layout and input-routing tests.
- **M3 — 3D viewport**: CPU rasterizer (depth buffer, shaded and
  wireframe modes, grid, axis gizmo), orbit/pan/zoom camera; GPU
  renderer later behind the same interface if FDK exposes a usable
  GL/Vulkan surface.
- **M4 — mesh modeling**: primitives, object transforms, edit mode
  with vertex/edge/face selection, move/rotate/scale, extrude, inset,
  bevel, loop cut, subdivide, merge, delete, mirror, smooth/flat
  shading, normals recalculation.
- **M5 — materials and UVs**: PBR-style materials (color, metallic,
  roughness, emissive, texture slots), UV unwrap (box/planar/smart),
  UV editor, image textures.
- **M6 — rigging**: armatures, bones, parenting, automatic weights,
  manual weight painting, simple IK.
- **M7 — animation**: timeline, keyframes (transform, bone,
  material), dope sheet, curve editor with easing curves, playback,
  loop ranges, simple clips and clip management.
- **M8 — export/import**: glTF 2.0 / GLB (meshes, materials, textures,
  skeleton, animation) first; then OBJ, STL, FBX export if feasible;
  import for glTF and OBJ; round-trip tests.
- **M9 — game-asset workflow**: asset presets for tools/props, pivot
  and origin tools, simple LOD generation, collision-shape export,
  batch export, units and axis settings (Y-up / Z-up).
- **M10 — preview rendering**: simple lit render to PNG, turntable
  frames, thumbnails.
- **M11 — polish**: keyboard shortcuts, custom themes, preferences,
  documentation, example projects.

## Current state

**M0 complete** (2026-11-18). Milestones M1–M11: not started.

### Implemented and tested

- **Build system.** `make` (debug, ASan+UBSan), `make release`, strict
  GCC 13+ warning set with `-Werror`; FDK discovered via pkg-config;
  the FDK include path is deliberately given ONLY to `src/platform/`
  (an FDK include anywhere else is a compile error — the platform
  boundary is enforced by the build, not just by review).
- **Platform layer** (`src/platform/`, wraps FDK 1.0.0):
  - `hal_platform_init/shutdown/pump` — lifecycle and the
    application-owned frame loop.
  - `hal_window_*` — create/show/destroy, title, resize, size, HiDPI
    scale, cursor shapes, event callback registration.
  - Events translated value-by-value from FDK to `hal_event`
    (configure, close, focus, expose, state, key, pointer, scroll,
    IME). Touch/tablet/gesture/drag-drop FDK events exist but are not
    yet forwarded — they will be added when the features that need
    them land.
  - `hal_surface_*` — XRGB8888 framebuffer access, damage
    declaration, present, frame pacing, offscreen surfaces (work with
    no display connection), PNG encode.
  - `hal_font_*` / text drawing — FDK's exposed TrueType
    rasterizer, wrapped (load, system default, metrics, measure,
    draw with clip).
  - `hal_clipboard_*` — text clipboard.
- **Application entry** (`src/main.c`): the frame loop, the M0
  placeholder scene (dusk gradient + wordmark — the real editor UI
  arrives with M2), `--smoke` automated window test,
  `--screenshot <path>`.
- **Tests.**
  - Headless tier (`make test`, no display): 41 checks — offscreen
    surface lifecycle, raw pixel write/readback, damage semantics,
    font discovery/metrics/measurement, text ink with and without
    clip, color/rect helper math.
  - Display tier (`make test-display`): starts a private Xvfb, opens a
    real 1280x800 window through the platform module, draws a frame,
    presents, verifies window/surface size agreement. The saved frame
    was additionally verified pixel-wise (gradient endpoints exact,
    wordmark ink present, empty regions clean).

### Not implemented (honest list)

- Everything M1–M11: math library, scene graph, undo/redo, project
  files, autosave, the widget/UI system, the 3D viewport, modeling,
  materials/UVs, rigging, animation, import/export, game-asset
  workflow, preview rendering, shortcuts/preferences/example
  projects.
- Touch, tablet, gesture and drag-and-drop events are not yet
  forwarded from the platform (the FDK side delivers them; Halcyon
  will map them when a feature needs them).
- File dialogs (open/save) are exposed by FDK but not yet wrapped —
  they arrive with project save/load in M1.
- Wayland is exercised only through FDK's own test suite; Halcyon's
  display tier runs under Xvfb/X11 today.

## Log

- **2026-11-18 — M0 complete.** Foundation, platform layer, both test
  tiers, docs, CI. First push.
