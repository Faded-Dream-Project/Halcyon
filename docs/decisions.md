# Halcyon — decisions

Every ambiguous point gets the simplest option that matches the product
goals (simple first, calm to use, export to anything). Decisions are
numbered, dated, and never silently reversed.

- **D-001 (2026-11-18) — One main window.** Halcyon is a single-window
  application; the 3D viewport, panels, timeline and editors all live
  in one window drawn by Halcyon. Menus and dropdowns open as platform
  popup windows when needed. Rationale: "no walls of panels or hidden
  modes" is easiest to honor when there is exactly one surface to
  compose.

- **D-002 (2026-11-18) — FDK is a platform layer, not a toolkit.**
  Halcyon uses FDK for window/surface/input/text/clipboard and nothing
  else. FDK's widget/layout/theme engines are deliberately unused —
  using them would make Halcyon a themed FDK app rather than a
  from-scratch suite. The Makefile gives the FDK include path to
  src/platform only, so this boundary is enforced by the build.

- **D-003 (2026-11-18) — Own 2D painter; FDK text through the wrapper.**
  Halcyon's UI paints its own primitives (rects, AA lines, circles,
  rounded boxes) directly into the XRGB8888 framebuffer, because the
  roadmap explicitly asks for own draw primitives and because the
  curve editor/gizmo work needs float-precision AA under Halcyon's
  control. Text rasterization is the one piece Halcyon reuses: FDK's
  public API exposes fonts/measurement/drawing, and shipping a second
  font stack would be waste, not virtue. Both compose on the same
  surface through src/platform.

- **D-004 (2026-11-18) — Full-frame repaint + damage-tracked present.**
  The frame loop repaints the whole window and declares full damage;
  FDK's present then transports only what changed. Simple and
  correct-first; per-region redraw is a measured optimization for
  later (the surface API already takes rects).

- **D-005 (2026-11-18) — Units: 1.0 = 1 meter, Y-up, Z-forward
  (right-handed), like glTF.** Halcyon's internal convention matches
  glTF 2.0 so the first-class exporter is lossless; Y-up/Z-up switch
  and unit scale are export-time settings (M9). This avoids Blender's
  constant internal conversion tax against its own primary export
  target.

- **D-006 (2026-11-18) — Scene data owns copies; undo/redo snapshots
  semantics, not files.** (Fleshed out in M1 when the systems land;
  recorded now as intent: undo operates on the scene graph's own
  structures with command-style apply/revert, not by re-parsing saved
  files.)

- **D-007 (2026-11-18) — CPU rasterizer first, GPU later behind the
  same interface.** The M3 viewport is a CPU rasterizer writing into
  the surface: deterministic, testable headless, identical on every
  machine, and fast enough for the target hardware at editor
  resolutions. A GPU renderer may come later behind the same
  `hal_renderer` interface; FDK's extension API exposes native display
  handles for that path (see docs/FDK-REQUESTS.md).
