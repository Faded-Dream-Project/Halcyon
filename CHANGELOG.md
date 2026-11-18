# Halcyon — CHANGELOG

The format follows keep-a-changelog (Added / Changed / Fixed /
Removed); dates are ISO. Everything listed here is built and tested —
PROGRESS.md carries the honest ledger of what is NOT done yet.

## [Unreleased]

### Added — M1: core (2026-11-18)

- Math library: f32 vec2/3/4, column-major mat4, unit quaternions —
  dot/cross/normalize, lerp, axis-angle, Euler<->quat, TRS
  compose/decompose, general inverse, perspective/ortho (NDC z
  [0,1]), look_at, slerp. Right-handed, Y-up, glTF-compatible
  conventions.
- Scene graph: stable ids, TRS transforms, parenting with cycle
  rejection, cached world matrices with dirty propagation,
  selection/visibility.
- Undo/redo: command stack (push-after-mutation contract), bounded
  depth, labels, leak-free by construction and by test.
- `.halcyon` project format v1 (docs/project-format.md): strict
  fail-closed parser, exact float round-trips, atomic saves.
- Autosave: interval schedule, dirty tracking, mtime-based crash
  recovery detection.
- Tests: test_math (85 checks), test_scene (43), test_undo (52),
  test_project (57, includes autosave).

### Added — M0: repo foundation (2026-11-18)

- Repository skeleton: `src/platform`, `src/ui`, `src/core`, `src/render`,
  `src/io`, `tests/`, `docs/`, `examples/`, CI workflow.
- Makefile: C17 + GCC strict warning set (`-Werror`), debug
  (ASan+UBSan) and release configurations with a config stamp, FDK
  discovery through pkg-config, headless and display test tiers.
- `src/platform`: Halcyon's platform layer — the only module that
  touches FDK. Window lifecycle, the frame loop (pump / draw /
  present), XRGB8888 surface access with damage declaration,
  offscreen surfaces (no display needed), text rendering and
  measurement (FDK's exposed TrueType rasterizer, wrapped), text
  clipboard, PNG encoding. All FDK types are value-translated to
  Halcyon types; the FDK include path is only given to
  `src/platform`, so a stray FDK include anywhere else is a compile
  error.
- `src/main.c`: application entry with the frame loop and the M0
  placeholder scene (dusk gradient + wordmark). `--smoke` runs the
  automated window test; `--screenshot <path>` saves the presented
  frame as PNG.
- Tests: `tests/test_platform.c` (41 headless checks — surface
  lifecycle, raw pixel readback, damage declaration, font metrics and
  measurement, text drawing with and without clip, color/rect
  helpers) and the display-tier smoke test (`make test-display`,
  starts its own Xvfb; a real window opens, draws and presents —
  verified pixel-wise on the saved frame).
- Docs: architecture, decisions, dependencies, FDK-REQUESTS.
- CI: builds FDK 1.0.0, builds Halcyon, runs both test tiers on every
  push.
