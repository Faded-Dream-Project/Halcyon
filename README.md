<div align="center">

# Halcyon

**A calm, from-scratch 3D modeling and animation suite for Linux. Part of the [Faded Dream Project](https://github.com/Faded-Dream-Project).**

</div>

---

Halcyon is being built as a long-term replacement for Blender, with a
different philosophy: **simple to learn, calm to use, able to export to
anything.** It is aimed at game assets — items, tools, props, characters —
and at pure animation. One clean window, a small set of obvious tools,
sensible defaults: a beginner should make and export a model in minutes,
and an expert should never fight the interface.

- **Own UI, drawn from scratch.** Halcyon draws its entire user interface
  itself — widgets, layout, theming, timeline, curve editor, 3D viewport —
  onto a plain pixel surface. No GTK, no Qt, no SDL, no ImGui, no wrapped
  toolkit.
- **FDK as a minimal platform layer only.** The [Faded Dream
  ToolKit](https://github.com/Faded-Dream-Project/FDK) provides the window,
  the drawing surface, input, clipboard, dialogs and the event loop —
  nothing more — and every FDK call lives inside one thin module
  (`src/platform/`) so the platform can be swapped later.
- **Dreamy look.** Soft purples and deep blues, the Faded Dream palette,
  driven by Halcyon's own theme files.
- **Fast on modest hardware.** Target machine: Intel i5-8400, GTX 1050 Ti,
  16 GB. The viewport and UI must stay smooth on it.
- **Honest engineering.** `PROGRESS.md` is a truthful ledger: nothing is
  claimed to work unless it builds and is tested. Every milestone ships
  with tests (a headless tier that needs no display, plus a display tier
  under Xvfb).

## Status

Early development — see [PROGRESS.md](PROGRESS.md) for the honest
milestone ledger and [CHANGELOG.md](CHANGELOG.md) for the history. The
roadmap (M0…M11) lives at the top of PROGRESS.md.

## Building

Requirements:

- GCC 13+ (C17), GNU make
- [FDK](https://github.com/Faded-Dream-Project/FDK) installed
  (`pkg-config fdk` must resolve) — Halcyon links against an installed
  FDK and never bundles it
- `Xvfb`, optionally, for `make test-display`

```sh
make              # debug build (ASan+UBSan): build/halcyon
make release      # optimized build
make test         # headless test suite (no display needed)
make test-display # window smoke test under a throwaway Xvfb
./build/halcyon   # run
```

## License

To be decided — see [LICENSE](LICENSE) (placeholder). Halcyon will use a
permissive license; no copyleft anywhere in its dependency graph
(docs/dependencies.md tracks every exception before it can happen).

## Repository layout

```
src/platform/  the ONLY module that touches FDK (windows, surface, text)
src/core/      math, scene graph, undo/redo, project files, mesh data
src/render/    the 3D rasterizer and camera
src/ui/        Halcyon's own widget toolkit and theming
src/io/        importers and exporters (glTF, OBJ, STL, ...)
tests/         headless test tier + the display tier
docs/          architecture, decisions, formats, FDK requests
```
