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

**M0–M6 complete** (2026-11-18). M6 = PBR-style materials + texture
slots, UV unwrap (planar/box/smart-lite), the UV editor widget, glTF
export with materials/UVs/embedded textures, project format v3. Next:
M7 rigging (armatures, bones, parenting, auto weights, weight paint,
simple IK; skins in the glTF export), then M8 animation. Resume from
src/core/ (new bone/armature structures) and src/io/gltf.c (skins).

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

### Implemented and tested — M1: core (2026-11-18)

- **Math library** (`src/core/math.h`, `math.c`): vec2/3/4 (f32), vec2i,
  column-major mat4 (v' = M·v), unit quaternions (x y z w). Dot/cross/
  normalize (zero-safe), lerp, axis-angle, Euler XYZ <-> quaternion,
  TRS compose and decompose (with shear detection and negative-scale
  handling), general 4x4 inverse (degenerate input -> identity),
  perspective/ortho with NDC z in [0,1] (glTF-style), right-handed
  look_at, quaternion slerp (short-arc). Conventions documented in the
  header and docs/decisions.md D-005.
- **Scene graph** (`src/core/scene.h/c`): flat object array with stable
  unique ids, glTF TRS local transforms, parenting with cycle rejection
  (set_parent refuses self/descendant/unknown), removal re-parents
  children upward, cached world matrices with subtree dirty
  propagation, selection and visibility. Object types declared for
  mesh/armature/camera/light; payloads attach in later milestones.
- **Undo/redo** (`src/core/undo.h/c`): command pattern — push-after-
  mutation (documented contract), apply/revert over an owned state
  blob, bounded depth with oldest-first eviction, redo stack cleared on
  push, labels for UI menus, no-leak guarantees on every path (held to
  by ASan).
- **Project format** (`src/core/project.h/c` + docs/project-format.md):
  `.halcyon` version 1 — strict fail-closed parser with line-numbered
  errors (unknown keys, bad numbers, forward parent references, broken
  blocks all refuse), exact float round-trips (%.9g), atomic save
  (temp + fsync + rename). Round-trip tested.
- **Autosave** (`src/core/autosave.h/c`): dirty tracking, interval-
  scheduled writes through the atomic project save, retry-on-failure
  without busy-looping, crash-recovery detection by mtime comparison.

### Implemented and tested — M2: own UI system (2026-11-18)

- **2D painter** (`src/ui/canvas.h/c`): rect fills with outward
  rounding at HiDPI scale, antialiased rounded rects (rounded-box SDF,
  interior byte-identical to crisp fills, 1px fringe), rect outlines,
  Xiaolin-Wu AA lines, AA circles, AA triangles (barycentric coverage),
  text through the platform's exposed rasterizer, an intersecting clip
  stack (clamped in float space — no integer overflow), logical ->
  physical scaling so the UI never sees the HiDPI factor.
- **Theme system** (`src/ui/theme.h/c` + docs/themes.md): 17 color
  tokens + 5 metrics, the built-in "Faded Dream" face (deep blues,
  soft purples), strict fail-closed `.haltheme` parser with
  line-numbered errors, partial themes inherit the default, save/load
  round-trip tested.
- **Widget toolkit** (`src/ui/`): retained widget tree with labels,
  buttons, toggles, sliders, single-line entries (UTF-8 caret
  navigation, backspace/delete/insert), panels with title bands,
  rows/columns with expand, tabs, tree views (expander triangles,
  selection, callbacks) and menu bars with in-window overlay menus
  (decision D-008 — no OS popups). Public API in `src/ui/ui.h`;
  internals split across widget/layout/paint/input/menu/tree/controls
  sources.
- **Layout engine**: bottom-up measure, top-down arrange, absolute
  rect walk; text measured through a per-UI font cache keyed by
  (logical px, scale).
- **Input routing** (`src/ui/input.c`): hit testing (paint order ==
  hit order), hover tracking, press/release with release-position
  activation, the implicit grab (slider drags survive leaving the
  widget), keyboard focus cycling (Tab/Shift-Tab), entry editing,
  menu open/switch/activate/close semantics (outside click consumed,
  Escape closes). Fully headless-testable — the tests feed synthetic
  events.
- **Editor shell** (`src/shell.h/c`): the M2 application — menu bar
  (File/Edit/Add with working actions), outliner bound to the scene
  graph (selection sync), properties panel (name + location editing
  through the M1 undo stack), status bar, the viewport rect reserved
  for M3 with a placeholder grid. Project new/open/save/save-as
  through the platform's file dialogs; Ctrl+N/O/S/Z/Y and
  Ctrl+Shift+Z shortcuts; autosave driven from the frame loop.
- **Platform additions**: file dialogs (open/save, async callbacks,
  bridge-owned paths), message dialogs.
- Tests: test_ui (152 checks — canvas fills/clips/scaling/AA
  coverage, theme round trip + strict-parser failures, row/column
  layout with expand, panel nesting, button/toggle/slider/entry/tree/
  menu interaction through synthetic events). The display-tier smoke
  test now paints the full editor shell; the frame was verified
  pixel-wise (menubar/sidebar/viewport regions, grid lines, placeholder
  text ink).

### Implemented and tested — M3: 3D viewport (2026-11-18)

- **Mesh data structure** (`src/core/mesh.h/c`): vertices, edges,
  polygon faces as loop runs — the shape M4's modeling tools need.
  Newell face normals, area-weighted smooth normals, bounds + bounding
  radius, all cache-invalidated on mutation. Primitives: cube, UV
  sphere, plane, cylinder, cone, torus. Scene objects own an optional
  mesh payload (freed with the object).
- **Camera** (`src/render/camera.h/c`): orbit/pan/zoom with pitch
  clamping, view/projection (D-005 conventions), project + ray
  unprojection (picking-ready), Gribb-Hartmann frustum planes with
  sphere culling.
- **CPU rasterizer** (`src/render/raster.h/c`): depth-buffered
  triangles (perspective-correct attributes, NDC-z [0,1], back-face
  culling, scissored to the canvas region + clip stack),
  depth-interpolated Bresenham lines, points. The depth buffer is
  reused across frames.
- **Viewport renderer** (`src/render/render.h/c`): 1 m horizon grid
  (near-plane clipped lines), objects with flat headlight shading or
  wireframe (near-clipped edges), homogeneous near-plane polygon
  clipping, per-object frustum culling, and the axis gizmo overlay.
  Renders into the editor shell's viewport rect; camera controls wired
  (MMB/Alt+LMB orbit, Shift+MMB pan, wheel zoom); a Wire toggle rides
  the tool rail.
- Tests: test_mesh (88 checks) and test_viewport (62 checks — camera
  orbit/eye/project/ray round trips, culling, raster depth ordering,
  back-face culling, depth-range line rejection, scene render with
  cull-on/off). The display-tier smoke test paints the full editor
  with the live viewport; the frame verified pixel-wise (background,
  grid, lit geometry).
- **Bugs the tests caught** (all fixed): a face-offset array growth
  off-by-one (heap overflow, ASan), a clip buffer sized for its input
  rather than its output (stack overflow, ASan), a stale-binary smoke
  run, sphere face-count arithmetic in a test.

### Implemented and tested — M4: mesh modeling (2026-11-18)

- Selection state (per vert/edge/face bytes, grown with the arrays),
  hal_mesh_clone for undo snapshots, and the operators: extrude
  (per face along its normal), inset, edge bevel (manifold
  single-edge), loop cut (quad-ring walk, cut across the ring),
  subdivide (shared midpoints + centers, selection-aware), merge to
  centroid, delete verts/faces, mirror (winding-consistent), flip
  winding, rebuild edges, recalc normals. All operator results
  outward-normal-verified.
- Picking: Möller–Trumbore face rays, scene-depth object picking,
  projected vert/edge picking.
- Edit mode in the editor: Tab, 1/2/3 selection modes, click select
  with shift-extend, overlays (selected verts/edges/faces), every
  operator on a menu + shortcut through one undoable path (snapshot
  + swap command). Move gizmo with axis handles (screen-space hit
  test, depth-correct world-per-pixel dragging). Modal R/S rotate
  (world Y) and uniform scale with click-confirm/Esc-cancel.
- Per-object smooth shading (per-vertex lambert interpolation in the
  rasterizer); project format v2 saves meshes and the smooth flag
  (v1 files still load). Add menu: all six primitives.
- Tests: test_model (90 checks). Bugs the tests caught: the cube
  primitive had been INWARD-WOUND since M3 (invisible under correct
  backface culling; M3's screenshot checks had mistaken grid lines
  for the cube — all primitives now outward-verified), loop cut put
  midpoints on the wrong edges, subdivide read past its keep array
  (heap overflow, ASan).

### Implemented and tested — M5: export/import (2026-11-18)

- glTF 2.0 GLB export: nodes with TRS + hierarchy, per-mesh
  primitives with FLOAT positions, per-corner FLOAT normals (face
  normals when flat-shaded — position stream duplicated per corner;
  vertex normals when smooth), UNSIGNED_INT indices, one BIN chunk,
  POSITION min/max. Hand-rolled JSON writer with string escaping.
- glTF GLB import: a minimal recursive-descent JSON DOM parser
  (objects/arrays/strings with escapes incl. \uXXXX, numbers,
  bools), GLB chunk parsing, accessor/bufferView resolution over
  FLOAT/UNSIGNED_INT/SHORT/BYTE, node-chain walking with accumulated
  world transforms (hierarchy flattened on import), triangulated
  faces, non-indexed primitives supported.
- OBJ export (global vertex numbering, n-gon faces, one o per
  object) and import (global indices, negative relative indices,
  unknown-line skipping). STL binary export (all visible meshes
  baked to world space, true facet normals).
- File menu: Export glTF/OBJ/STL, Import glTF/OBJ.
- Tests: test_io (51 checks — GLB structure + round trips with
  transform/hierarchy verification, OBJ round trip with relative
  indices, STL structure/size/facet checks, corrupt-file refusal).
  The round trip caught a real bug: OBJ export used global vertex
  numbering while the importer reset per object (multi-object files
  failed to re-import).
- Materials and UVs are NOT in the exporters yet (they arrive with
  M6, per the revised roadmap).

### Implemented and tested — M6: materials & UVs (2026-11-18)

- Materials (src/core/material.h/c): glTF metallic-roughness model —
  base color, metallic, roughness, emissive + strength, one texture
  slot — owned by the scene, referenced per object; scene image list
  (file paths).
- Per-corner (per-loop) UV storage on the mesh, grown with the loops
  and carried through face compaction (merge/delete/subdivide keep
  UVs aligned); uvs_valid marks unwrapped meshes.
- Unwrap operators (src/core/uv.h/c): planar (X/Y/Z), box (dominant-
  normal axis groups in a 3x2 grid), smart-lite (normal-similarity
  islands via BFS, dominant-axis projection, row packing) — all as
  undoable mesh operations on the Edit menu.
- UV editor widget (src/ui/uvview.c): bound to the selected object's
  mesh, draws the UV grid, face loops and selected faces; lives in
  the sidebar.
- Material panel: base-color entries + metallic/roughness sliders
  bound to the object's material; New Material on the Edit menu. The
  viewport shades objects with their material base color + emissive
  lift.
- glTF export: materials (baseColorFactor, metallic/roughness
  factors, emissiveFactor, baseColorTexture), TEXCOORD_0 per-corner
  streams (per-corner duplication forced when UVs exist), embedded
  image textures (bytes + mimeType + bufferView + sampler). Project
  format v3 saves materials/images (v1/v2 files still load).
- Tests: test_material (818 checks — material/v3 round trip, unwrap
  ranges + box cell seams, UV editor paint, GLB JSON carrying the
  material and UVs). Fixed along the way: the material header parse
  order, and a heap overflow where a non-NUL-terminated build buffer
  reached %s (ASan).

### Known issues (honest, carried forward)

- Extrude/inset operate per face (adjacent selected faces extrude
  with seams between the duplicates); region-aware extrude is a later
  refinement. Bevel handles single manifold edges only (vertex
  bevels out of scope). glTF import flattens node hierarchies into
  world transforms (parenting is baked; geometry exact).
- Viewport texture SAMPLING is not implemented (materials shade by
  base color + emissive; textures are authored, previewed in the UV
  editor, and exported). glTF import ignores materials/textures.
  Smart unwrap is projection-based (documented in uv.h), not
  angle-based flattening. Geometry created after an unwrap carries
  (0,0) UVs until re-unwrapped.
- The move gizmo drags along world axes only; screen-plane drag and
  rotate/scale gizmos are future polish (R/S modal covers them).

### Not implemented (honest list)

- Everything M4–M11: mesh modeling (edit mode, extrude/inset/bevel/
  loop cut/subdivide/merge/mirror; meshes are not yet representable in
  the project file — format version 1 has objects and transforms
  only), materials/UVs, rigging, animation, import/export, game-asset
  workflow, preview rendering, preferences/example projects.
- A GPU renderer behind the hal_renderer seam (the CPU rasterizer is
  the only implementation; FDK-REQUESTS.md R-001 tracks the GL
  surface question).
- Widget gaps (honest): no scrolling containers (long outliners clip),
  no multi-line text editor, no tooltips, no DPI-change re-layout
  while running (re-layout happens on resize events), no IME
  forwarding into entries yet (the platform exposes the events; the
  entry consumes raw codepoints only), no side-of-window panel
  dragging (docking-lite is fixed panels with splitters planned for
  M3+).
- Menus cannot extend past the window (in-window overlays by design).
- Touch/tablet/gesture events not yet routed (platform delivers them;
  no consumer yet).
- Touch, tablet, gesture and drag-and-drop events are not yet
  forwarded from the platform (the FDK side delivers them; Halcyon
  will map them when a feature needs them).
- File dialogs (open/save) are exposed by FDK but not yet wrapped —
  they arrive with project save/load in M1.
- Wayland is exercised only through FDK's own test suite; Halcyon's
  display tier runs under Xvfb/X11 today.

## Log

- **2026-11-18 — M6 complete.** Materials + texture slots, per-corner
  UVs with compaction-safe storage, three unwraps, the UV editor,
  glTF materials/UVs/embedded textures, format v3. 1784 headless
  checks total (818 new).
- **2026-11-18 — M5 complete.** glTF 2.0/GLB + OBJ + STL export,
  glTF + OBJ import, round-trip tested (1024 headless checks total).
- **2026-11-18 — M4 complete.** Mesh modeling: selection, picking,
  edit mode, 12 operators, move gizmo, modal rotate/scale, smooth
  shading, project format v2.
- **2026-11-18 — M3 complete.** Mesh data structure + six primitives,
  orbit camera with ray/frustum math, depth-buffered CPU rasterizer,
  the viewport renderer (grid, solid/wireframe, gizmo) wired into the
  shell with camera controls. 735 headless checks total (150 new).
  Tests caught two memory-safety bugs before they could ship.
- **2026-11-18 — M2 complete.** Halcyon's own UI toolkit: painter,
  themes, widgets, layout, input routing, the editor shell wiring the
  M1 core (scene outliner, undoable property editing, project
  open/save, autosave). 582 headless checks total (152 new). Tests
  caught five real bugs: a clip-stack integer overflow (UBSan), layout
  order (absolute rects computed before arrange), release-position
  activation, menu-bar item geometry computed before absolute rects
  existed, and a tree-row leak (ASan).
- **2026-11-18 — M1 complete.** Math library, scene graph, undo/redo,
  .halcyon project format (documented, atomic, strict), autosave with
  crash-recovery detection. 278 headless checks across five suites
  (math 85, platform 41, project 57, scene 43, undo 52). Three real
  bugs found and fixed by the tests: a wrong Euler-extraction
  convention, a broken slerp basis formula, and autosave's first-save
  schedule contradicting its documentation.
- **2026-11-18 — M0 complete.** Foundation, platform layer, both test
  tiers, docs, CI. First push.
