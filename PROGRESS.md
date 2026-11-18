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

**STEP 0 (2026-11-18, session 6) — FDK HAS a GPU API; the old
"blocked" note was wrong.** FDK 1.0.0 exposes a real GPU draw path
(not merely a present lane): extension surfaces get a per-frame GL
callback with FDK's lane context CURRENT (fdk_extend.h:992-1014 —
fdk_ext_gl_make_current / fdk_ext_gl_set_frame_callback /
fdk_ext_gl_swap / fences; gated by FDK_EXT_CAP_GL, fdk_extend.h:162),
the add-on creates the native window in a kind's create callback
(fdk_extend.h:285-353), CPU-painted content rides fdk_texture
(fdk_surface.h:661-681), and FDK_RENDER_GPU (fdk_core.h:53-79) makes
the lane REQUIRED with no CPU fallback (creation fails honestly).
docs/FDK-REQUESTS.md R-001 is rewritten accordingly (deleted as a
request). Verified the symbols ship in libfdk.so (nm: FDK_1.0 tag).
Halcyon's GPU-only viewport therefore does NOT need the sub-rect API
the old note asked for: the editor window becomes an extension
surface, 3D renders with raw GL in the frame callback, and Halcyon's
own UI painter renders to an fdk_texture composited as the last pass
(exactly the intended architecture).

**PRE-ALPHA — NOT READY FOR USE.** Both former tags (v0.1.0, v0.2.0)
were DELETED on 2026-11-18 after a real-desktop screenshot showed
the editor UI is visibly broken at 1920x1080 (vertically clipped
text, truncated menu popups, overflowing property rows, a wrongly
projected ground grid). Nothing is a release; no tag or release
will be created unless explicitly requested.

**UI REPAIR (2026-11-18, session 5) — the reported defects are fixed
and now GUARDED BY TESTS.** Root causes found and fixed:
(1) every text site used a vertical-centering formula that drew text
a full ASCENT too high (8 sites) — replaced by a shared
metrics-based hal__baseline; (2) row/panel/tab/tree heights were
sized from the nominal font px — now from REAL font metrics
(hal__text_row_h), scale-aware (physical metrics / ui->scale);
(3) the tool rail and hint bar never received layout rects — the
Wire/Paint toggles were invisible AND unclickable; (4) the
properties Location/Color rows overflowed the sidebar — fields now
expand to share the row, rows restructured (label line + fields);
(5) the sidebar now SCROLLS when panels overflow (small logical
surfaces / HiDPI); (6) the ground grid's near-plane SEGMENT clip
treated 2-point input as a closed polygon and dropped every line
crossing the near plane (the "wrongly projected grid": only
far-side lines survived) — plus unbounded Bresenham hung on the
huge near-crossing coordinates (now Liang-Barsky clipped to the
raster first); (7) hal_canvas_measure_text inverted the
hal_font_measure success check — every measured width was 0
(shortcut columns, button centering, entry carets); (8) menu
popups now clip to their own rect; (9) layout runs in LOGICAL units
from the canvas (surface/scale), never the raw window size;
(10) the status/hint labels were zero-width on the first frame
(measured sizes read before the first measure pass) — full-width
strips; the status bar paints a panel strip + text after the UI
pass. Camera defaults: a classic 3/4 view, closer (cube reads at a
sane size); small font 13 -> 14.

**VERIFICATION (how this was checked — and its limits).** A new
STEP-2 audit hooks EVERY draw_text: the text's full ink rect (real
ascent..descent, real advance) must fit the clip stack, and every
menu popup must be at least as wide as its widest item. It runs
headless in `make test` (test_uilayout: a shell-mirroring UI at
1920x1080/1366x768/1280x720 x scale 1.0/2.0 — 28 checks) and on
the REAL app in `make test-display` (--ui-audit at the same six
configs plus an alternate font face via HAL_UI_FONT). All 0
violations. The grid has a geometric regression test (projected
grid intersections must have ink at their projected pixels). Pixels
were also verified programmatically in the saved screenshots
(scripts/verify-screens.py: no ink in the window's top rows, menubar
text present, cube + grid ink in the viewport, sidebar text, status
strip + text). Screenshots: download/editor-{1920x1080,1366x768,
1280x720}.png. **HONEST LIMITS:** this agent CANNOT view images (no
image input in its environment) — all visual verification here is
programmatic (metrics audits + pixel-color invariants), and the
sandbox's font is fontconfig's DejaVu Sans (what a typical Linux
desktop resolves; a user's desktop may resolve another face — hence
the alternate-face audit runs). The reported grid distortion could
not be reproduced geometrically BEFORE the fixes (the old camera
survived the older checks by accident); the near-plane clip bug it
exposed is real, fixed, and now guarded.

Session 3 (2026-11-18): items 1-3 of the release plan DONE (all
pushed). Item 1: glTF skins. Item 2: texture sampling + the
barycentric-misbinding fix. Item 3 (M9 remainder): asset presets
(Crate/Barrel/Sword/Table on the Add menu), vertex-clustering LOD
generation (Edit > Generate LOD; cluster welding with degenerate
face removal), box-collider glTF export (File menu, from the selected
object's world AABB), and glTF export options — Z-up axis (rotX+90
baked into root nodes; animation channels on roots convert too) and
centimeter units (File menu toggles, also applied by batch and
collider exports). NEXT: item 4 (curve editor widget + weight
painting), item 5 (benchmark + rasterizer optimization), then 6/7.
Convex-hull colliders remain future work (box covers the common
case; noted below).

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

### Implemented and tested — M7: rigging (2026-11-18)

- Armatures (src/core/armature.h/c): bones as rest head/tail
  segments with parents in armature space; a local pose rotation per
  bone; chain posing where a child's rest positions ride the parent's
  posed matrix (rotating a parent swings descendants, children stay
  attached to posed tails).
- Skinning: 4 influences per vertex (bone index + weight), automatic
  weights by nearest-bone-segment inverse-square (2 strongest,
  normalized — not heat diffusion, documented); the standard
  inverse-bind formulation p' = Σ w·P·B⁻¹·p; the viewport renders
  skinned meshes posed.
- Analytic 2-bone IK (law of cosines, bend plane from the chain and
  target direction, reach clamped to the annulus) — the lower bone's
  pose is correctly LOCAL (cancels the parent's rotation); the
  classic trap the tests caught.
- Editor: Add > Armature; pose mode (Tab on an armature) with bone
  picking (projected segment distance), R bone-rotate modal (world
  Y), Add Bone (child), Bind Mesh to Armature (auto weights), Solve
  IK (tip toward the mouse ray at the tip's height plane). Armatures
  draw as stick figures with selected bones highlighted.
- Project format: bones (rest + pose), skin references, and weight
  blocks round trip; the format version constant was bumped to 3
  (files written before this commit say version 1 but parse fine —
  the loader accepts 1..3).
- Tests: test_rig (42 checks — chain posing, weight sums, unposed
  identity, posed deformation, IK reach + clamped reach, project
  round trip). Bugs caught: the degenerate IK bend axis, the
  local-vs-world lower-bone rotation, and an unsigned parse of the
  signed bone parent.

### Implemented and tested — M8: animation (2026-11-18)

- Clips (src/core/anim.h/c): channels per property (location XYZ,
  rotation quat, scale XYZ, bone pose), sorted keys with per-key
  easing (linear, smoothstep ease-in-out, step), quaternion slerp,
  clamped or looping sampling, per-object evaluation for exporters.
- Editor: Animation menu (New Clip, Insert Key, Play/Pause, Loop);
  I inserts object TRS keys (or the posed bone's rotation); Space
  plays; the timeline strip draws the ruler, one row of key dots per
  channel (color-coded by path kind) and the playhead, click-scrubs,
  playback advances in the frame loop with loop wrap. Clips persist
  in the project format (channels, keys, range, loop).
- glTF export: animations as baked 30 fps LINEAR sampler pairs
  (in-app easing bakes into the samples) with correct accessor/
  bufferView numbering after the mesh and image slots — the exported
  JSON validated with an independent parser.
- Tests: test_anim (50 checks — key insert/replace, linear/eased/step
  sampling, quaternion midpoints, loop wrap semantics where end ==
  start, bone channels, project round trip, glTF export contents and
  re-import). Caught: two non-NUL-terminated buffer reads (ASan) and
  the 180-degree quaternion path ambiguity (the test now pins an
  unambiguous target; both arcs are legal halves).

### Implemented and tested — M10: preview rendering (2026-11-18)

- src/render/preview.h/c: scene framing (bounds-sphere fit with
  margin), lit PNG renders at any size through the viewport
  rasterizer (gizmo off, grid optional), turntable frame sequences
  (360-degree orbit, prefix_NNN.png), thumbnails (framed square).
- Render menu: Preview PNG, Turntable Frames (24 frames), Thumbnail.
- The platform wrapper gained image DECODING (hal_surface_from_image)
  so tests (and future texture work) read PNGs back.
- Tests: test_preview (14 checks — PNG content verification by
  reloading and counting distinct pixels, turntable frame existence
  and content, bad-argument refusal).

### Implemented and tested — M9 (partial, 2026-11-18)

- Origin tools: Origin to BBox Center (verts shift, the object
  location compensates so world geometry stays put; snapshot-undo
  pushed), Origin to World Origin (location reset).
- Batch export: File > Batch Export glTF — one .glb per mesh object
  (clone-scene per object), prefix_NNN.glb naming.
- NOT implemented from M9 (honest): asset presets for tools/props,
  LOD generation (decimation), collision-shape export, units and
  Y-up/Z-up export settings. These are the next session's first
  tasks.

### Implemented and tested — glTF skins, export + import (session 3)

- Export: bones as joint NODES under the armature object's node
  (local = T(armature-space head) * R(pose); children chains), skins
  with joints + skeleton, inverseBindMatrices = rest^-1 * A^-1 * Wm
  (A = armature world, Wm = mesh world — at rest the glTF sum
  collapses to Wm * v exactly), JOINTS_0 (UBYTE VEC4, bone index into
  the joints array) and renormalized WEIGHTS_0 (FLOAT VEC4) on
  skinned primitives (per-corner duplication forced for skins), and
  bone-rotation animation channels targeting joint nodes (baked 30
  fps LINEAR like all Halcyon animation export).
- Import: skins rebuild armatures (bones with armature-LOCAL heads
  from joint local translations, +Y stub tails — glTF joints carry
  no tails; the armature object's TRS recovered from the root joint's
  parent-chain world), JOINTS_0/WEIGHTS_0 reattach to the mesh
  objects, and joint rotation animations import as HAL_CH_BONE
  channels (raw LINEAR keys). The file's IBMs are not consulted —
  Halcyon derives binds from the rebuilt rest pose (exact for
  Halcyon exports, approximate for foreign files whose joints carry
  rest rotations — documented).
- Space-aware skinning: hal_mesh_apply_pose_ex(mesh, arm,
  A^-1*Wm, out) — the renderer uses it, fixing the silent assumption
  that a bound mesh shares its armature's transform.
- Round-trip test (test_rig): structural JSON checks (skins, joints,
  IBMs, JOINTS_0/WEIGHTS_0, skeleton, rotation path), imported
  armature/mesh/weights/clip verification, and the numeric
  world-space pose equivalence with a 60-degree posed bone.
- Bugs found and fixed (each with a regression or strengthened
  test): the M7-era REST POSE leaked posed parent rotations into
  bind matrices (skin results were wrong whenever a parent bone was
  posed; the old test only checked "moved" — now exact values);
  sampler bufferViews missed the IBM/image view offsets (clip keys
  read garbage); read_accessor rejected VEC4; "skin" sat on the
  primitive instead of the node; a missing comma after the object
  nodes; JOINTS_0/WEIGHTS_0 missing from the primitive attributes;
  the root-joint search required parent < 0.

### Implemented and tested — viewport texture sampling (session 3)

- raster.h/c: hal_raster_triangle_uv — TRUE perspective correction
  (uv/w and 1/w interpolate affinely in screen space, divide per
  pixel; intensity too on this path; the plain triangle stays affine
  and is now documented as such); hal_texture (borrowed packed rows)
  + hal_texture_sample with REPEAT wrapping, exact nearest and
  bilinear.
- render.h/c: hal_texture_cache (paths -> decoded 0x00RRGGBB rows,
  owned by the app, loads through the platform's image loader);
  hal_render_opts.textures; per-corner UVs ride clip_vertex through
  near-clipping (lerped with the intensity); materials with textures
  render modulated (base color x texel x lambert).
- material: per-material `filter` (nearest/bilinear, default
  bilinear), persisted in the project format. Shell: Edit > Load
  Texture... (image picker -> scene image + the object's material, a
  material created when none), Toggle Texture Filter.
- Tests (test_material, 846 checks total): textured render against
  known geometry (2x2 texel checker on a planar-unwrapped plane at an
  angle; the pixel at each projected world point shows that point's
  own texel — affine interpolation would fail this), bilinear center
  blend, and the isolated raster regression (screen-space triangle,
  exact texels, plus a mixed-clip-w case with the perspective-correct
  uv computed by hand — affine picks the wrong texel).

### Implemented and tested — M9 game-asset workflow complete (session 3)

- Asset presets (Add menu): Crate (wood material), Barrel (cylinder +
  steel material), Sword (tapered blade + guard + handle, three
  objects), Table (top + 4 legs) — hand-built compositions exercising
  primitives, transforms and materials.
- LOD generation: hal_mesh_decimate_cluster — vertex-clustering
  decimation (lattice weld with centroid representatives; faces
  survive only when their deduped corner count equals their run
  length, no wrap collapse; the vert array is replaced directly —
  compact_verts is a DELETE helper and would wrongly drop faces that
  reference merged verts). Edit > Generate LOD clones the selected
  mesh at diag/8 as <name>_LOD. Honest scope: clustering, not
  quadrics; meshes straddling a cell boundary never merge (a real
  property, pinned by the test).
- Collision-shape export: File > Export Box Collider — the selected
  object's world AABB as a one-box glTF scene (the pipeline verified
  by re-import and AABB comparison). CONVEX hulls are future work.
- Units/axis: hal_io_gltf_export_ex with {unit_scale, z_up} — the
  conversion (S(unit) * rotX(+90)) bakes into ROOT node transforms;
  children ride the hierarchy; animation channels targeting roots
  convert (translation rotated+scaled, rotation premultiplied, scale
  scaled); skin IBMs stay consistent (the conversion is uniform).
  File menu toggles (Z-Up, Centimeters) feed the single, batch and
  collider exports. Y-up meters remains the default.
- Tests: test_gameflow (26 checks — decimation reduction/validity/
  total-collapse/tiny-cell, z-up+cm root and child world positions
  through a round trip, defaults unchanged, collider AABB
  equivalence). Bugs caught: the weld originally used compact_verts
  (wrong helper — heap overflow, then no reduction) and the huge-cell
  test straddled a lattice boundary.

### Implemented and tested — curve editor + weight painting (session 3)

- Curve editor widget (src/ui/curves.c): bound to the active clip;
  per-channel value-over-time graphs (vec: x/y/z lines; rotation:
  x/y/z/w), each channel normalized to its own value bounds, key
  dots, a synced playhead, and the clips' own easing baked into the
  sampled polylines. READ-ONLY (honest scope: key insertion and
  scrubbing live in the timeline; per-key easing is edited in data).
- Weight painting: hal_mesh_paint_weight (core, testable) — brush at
  a world point with smoothstep falloff, world-to-local through the
  object matrix, per-vert renormalization, free influence-slot
  acquisition, add/subtract by sign. Shell: Paint tool-rail toggle,
  left-drag strokes (Shift subtracts), the brush rides the mouse
  ray's mesh hit (or the closest approach to the object center on
  misses), one snapshot-undo command per stroke, the active bone
  tracks the pose-mode bone selection.
- Tests: weight paint (falloff reach, sums stay 1, subtract reduces,
  far touch is a no-op) in test_rig; curves widget pixel-content
  paint check in test_anim.

### Implemented and tested — M11 polish (session 3)

- Theme switching: File > Load Theme... applies a .haltheme (the
  format and parser existed since M2); the snapshot render options
  refresh with the theme. examples/themes/daylight.haltheme ships as
  the second look.
- Example projects (examples/, reload-verified by the generator):
  tool-sword.halcyon (a simple tool), prop-crate.halcyon (a crate on
  a barrel with materials), character-wave.halcyon (a rigged, skinned
  and keyframed limb with a looping wave clip). Generator:
  scripts/gen_examples.c via scripts/gen-examples.sh.
- Documentation: docs/shortcuts.md (every binding by mode) and
  docs/user-guide.md (a start-to-export walkthrough). FBX assessed
  and declined for v0.1 (docs/decisions.md D-015: ASCII FBX is
  poorly imported, binary FBX is a multi-week format effort; glTF
  covers the need).
- Bugs the examples found and fixed (with regressions): the material
  'texture -1' line failed an unsigned parse; the weights block was
  saved before the mesh block so weighted projects could not reload
  (test_project_armature_round_trip now binds weights before saving
  and asserts they survive the round trip).

### Implemented and tested — robust glTF import (session 4)

- FOREIGN RIGS, EXACT: hal_bone gains rest_offset (a local rest
  rotation; identity = native). pose_build's REST matrix becomes
  T(parent_rest * head) * R(rest_offset) — for foreign files the
  offset accumulates the file's joint rest rotations, so the in-app
  bind equals the file's node-chain rest and the file's
  inverseBindMatrices are reproduced exactly (foreign skinned meshes
  import with identity object TRS, matching glTF's "node transform
  ignored for skinned meshes" rule; Halcyon's own exports keep their
  mesh transform — the importer branches on the asset generator
  string). The root-recovery now un-rotates the root joint's local
  rotation. Rest offsets persist (additive "restoff" lines).
- Joint orderings: child-before-parent joints (legal glTF) defer
  parent links to a two-phase create + a topological offset fix-up.
  BUG FIXED (found by the fixture): hal_armature_bone_add rejected
  bones whose parent index did not exist yet, leaving arm->bones
  NULL and the importer writing through it — a real crash.
- Materials/textures: pbrMetallicRoughness (baseColorFactor,
  metallic, roughness, baseColorTexture) maps onto Halcyon
  materials. Images resolve three ways: embedded bufferView bytes
  and data: URIs (base64) are sniffed (PNG/JPEG magic) and written
  to <glb>-textures/ next to the source; plain URIs resolve
  relative to it. BUG FIXED: byteOffset/byteLength were read with
  json_num (an ARRAY-ELEMENT helper) and silently defaulted to 0 —
  embedded images never extracted.
- Multi-primitive meshes: ALL primitives merge into one Halcyon
  mesh (per-primitive vert offsets; per-corner TEXCOORD_0 rides the
  loops; JOINTS_0/WEIGHTS_0 merge with offsets; primitive 0's
  material represents the merged mesh — multi-material meshes
  merge geometry, documented).
- Tests (test_gltf_foreign, 45 checks): a minimal GLB WRITER builds
  fixtures in three conventions — parent-relative joints with rest
  rotations (import + pose + numeric comparison against the
  analytic sum(w * G(t) * IBM * v)), child-before-parent ordering
  (offset topology), and material + embedded/data-URI textures +
  two-primitive merge (factors, UVs, extraction on disk, decode
  through the texture cache).

### Implemented and tested — interactive curve editor (session 4)

- Core ops (anim.c): hal_clip_key_move (sets time+value, re-sorts
  the channel, the clip range follows the keys), hal_clip_key_delete
  (compacts; a channel dies with its last key), hal_clip_clone +
  hal_clip_destroy (undo snapshots).
- Widget event hook: hal_widget_set_event_hook — input.c dispatches
  presses (left AND right, before menus), motion and release to the
  hit widget (or its ancestor chain); claimed events grab the
  pointer. The curve editor installs its handler through
  hal_curves_interactive(clip, on_edit, user).
- Interactions (ui/curves.c, the normalization math shared with the
  painter through curves_channel_bounds): drag a key dot moves it in
  time and value (value bounds frozen at drag start; keys re-sorted;
  the dragged key re-found through the sort); double-click a key
  cycles STEP -> LINEAR -> SMOOTH (the easing "handles" in Halcyon's
  per-key model); double-click empty graph inserts a key into the
  channel whose curve passes nearest the click; right-click deletes.
  on_edit(BEGIN/END) brackets each change — the shell snapshots the
  clip and pushes ONE anim undo command per edit (contents swap).
- Tests (test_anim, 85 checks): the core ops, plus drag-to-middle
  (time 0.5 / value 0.5 through two-point bounds), easing cycle,
  insert-then-delete, all driven by synthesized pointer events
  through hal_ui_handle_event.

### Implemented and tested — convex-hull colliders (session 4)

- hal_mesh_convex_hull (mesh_ops.c, from-scratch quickhull): unique
  points, an extreme-point tetrahedron (coplanar/collinear sets bail
  with NULL), then eye/horizon wrapping — visible faces die, each
  horizon edge (traversed OPPOSITE to the non-visible face's winding
  — the consistent-manifold orientation; the same-direction bug made
  every wrap face inward and self-intersected the "hull", caught by
  the containment property test) spawns a face to the eye. A guard
  bounds iterations; degenerate results NULL.
- Export: File > Export Convex Collider — the quickhull of the
  selected object's WORLD-space verts as <object>_UCX (the Unreal-
  readable convex prefix; D-016 records why a naming convention
  beats the draft KHR_collision extensions), identity transform,
  honoring the Z-up/centimeter options. Degenerate meshes report and
  point at the box collider.
- Tests (test_gameflow, 42 checks): cube hull exact (8/12), sphere
  hull on-surface + volume within 5% + containment, coplanar/thin
  sets NULL, and the world-space export round trip (the imported
  collider still contains the source verts).

### Implemented and tested — modeling quality (session 4)

- Region-aware extrude (hal_mesh_extrude_region): connected selected
  faces extrude as ONE region — verts duplicated once (offset along
  the averaged face normal), walls only on boundary edges (interior
  shared edges stay seamless). E now uses it (identical to the
  per-face op for single faces; the old op stays for compat).
- Angle-based unwrap (hal_mesh_unwrap_angle): edges whose faces'
  dihedral exceeds the threshold become seams; charts grow across
  non-seam shared EDGES (true adjacency, unlike smart-lite's vert
  adjacency); per-chart dominant-plane projection; charts scale to a
  ceil(sqrt(n)) grid and shelf-pack INSIDE the UV square (the first
  packer overflowed [0,1] with six charts — the in-range test caught
  it).
- Knife (hal_mesh_cut_segment + the K interaction): the drag's
  screen endpoints unproject at the edit object's depth into a
  world-space cut segment; every face whose EDGES the segment
  crosses twice splits into two rings (crossings interpolated in 3D,
  coplanarity-checked Cramer solve — the u sign was flipped at
  first, and the ring construction put all corners in one ring;
  both caught by tests). A knife is a SURFACE stroke: lines through
  a solid's interior cross no face edges (documented in the test).
  One undo per stroke; a preview line draws during the drag.
- Vertex grab + proportional editing: G drags selected verts on the
  camera plane (screen delta -> world via the view basis); with O
  on, unselected verts within the radius move with a smoothstep
  falloff from the nearest selected vert. One undo per grab.
- Snapping: View menu cycles Off/Grid(0.25)/Increment(0.1)/Vertex;
  Ctrl-hold applies during gizmo drags (grid/increment quantize the
  moved component; vertex snaps the origin to the nearest other
  object's vert within 15px). Frame Selected (F) targets the camera.
- EDGE SLIDE deferred (honest): the trickiest of the six for the
  least modeling value right now; recorded here.
- Tests: test_modeling (17 checks) — region face/vert counts, seam
  chart counts + UV ranges, knife splits/misses/on-surface semantics.

### Implemented and tested — stability + usability (session 4)

- FUZZ (test_fuzz, deterministic splitmix64 seed): 240 random-byte
  files, every truncation (1/64 steps) of valid .halcyon/.obj/.glb
  files, 300 mutated valid files, and 120 GLB structural-garbage
  cases (valid magic, absurd lengths, junk chunk types) — every one
  through hal_project_load, hal_io_obj_import and hal_io_gltf_import
  under ASan+UBSan. The suite asserts both directions: clean
  rejections happened AND the valid files still load. FOUND A REAL
  BUG: nine hal_project_load error paths leaked the parsed key (key
  now hoisted, NULLed at every free, freed at done).
- Hostile inputs: absurd declared counts ("mesh 4000000000") are
  rejected without allocating; huge OBJ face indices reject; a
  1000-deep parenting chain loads and its world matrices resolve
  (recursion depth is tree depth, cycle-free by rule).
- Large-scene stress: 200 twice-subdivided cubes (~24k verts) —
  save/load round trip, one rasterizer pass, and a quickhull — all
  under the sanitizers.
- Hint bar: a second status line showing the current mode's key map
  (object/edit+prop/pose/paint), refreshed on every status change.
- First-run quick start: a seven-line card over the viewport on
  EMPTY first scenes (add, edit, material, animate, export); any
  viewport click dismisses it for the session.
- Tooltips: hal_widget_set_tooltip + a hover overlay after the
  pointer rests 500ms (the Wire/Paint tool-rail toggles carry the
  first two). docs/shortcuts.md gained G/O/K/F and the snapping
  modes.

### Known issues (honest, carried forward)

- Extrude/inset operate per face (adjacent selected faces extrude
  with seams between the duplicates); region-aware extrude is a later
  refinement. Bevel handles single manifold edges only (vertex
  bevels out of scope). glTF import flattens node hierarchies into
  world transforms (parenting is baked; geometry exact).
- M7/M8 leftovers (first tasks of the next session): manual weight
  PAINTING (auto weights exist; the brush UI does not), glTF SKIN
  export (bones/joints/weights), and the CURVE EDITOR widget (the
  timeline shows keys; curves are not yet drawn — easing is edited
  per key in data only). IK assumes the chain root's parent is
  unposed; the bone-rotate modal is world-Y only; animation export
  bakes at 30 fps rather than exporting raw keys.
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

- **2026-11-18 — session 4, items 5-6: stability + usability.** A
  deterministic fuzz suite over all three loaders (found and fixed
  nine leaked parser keys), hostile-count and deep-chain and
  large-scene stress; the hint bar, quick-start overlay and
  tooltips. The v0.2 plan is complete. 2771 headless checks total.
- **2026-11-18 — session 4, item 4: modeling quality.** Region
  extrude, angle-based unwrap, the knife, proportional grab, and
  snapping (edge slide deferred). Two knife bugs and one packer
  overflow caught by the new tests. 2375 headless checks total.
- **2026-11-18 — session 4, item 3: convex-hull colliders; v0.2.0.**
  A from-scratch quickhull with horizon-walk wrapping (orientation
  bug caught by the containment property test), exported as
  _UCX-named glTF meshes in world space (D-016). 2358 headless
  checks total. Tagged v0.2.0 (items 1-3 of the v0.2 plan done and
  tested).
- **2026-11-18 — session 4, item 2: interactive curve editor.** Key
  drag (time+value, re-sorted), easing cycle, insert/delete, one undo
  per edit; a generic widget event hook in the UI input dispatch.
  2316 headless checks total.
- **2026-11-18 — session 4, item 1: robust glTF import.** Foreign
  rigs exact via bone rest offsets (analytically verified against
  hand-built fixtures); materials + textures (embedded, data-URI,
  external) import; multi-primitive meshes merge. Two real bugs
  fixed with regressions: child-before-parent joint crash, and
  bufferView fields read with the wrong JSON helper. 2231 headless
  checks total.
- **2026-11-18 — session 3, item 7: v0.1.0.** README with
  screenshots (the editor + a posed character render through the
  example project), the tested feature list, known limitations;
  v0.1.0 tagged.
- **2026-11-18 — session 3, item 6: M11 polish.** Theme switching
  with a shipped Daylight theme; reload-verified example projects
  (tool/prop/animated character); shortcuts + user-guide docs; FBX
  honestly declined (D-015). Two format bugs fixed with regressions
  (texture -1 parse, weights-before-mesh save order). 2186 headless
  checks total.
- **2026-11-18 — session 3, item 4: curve editor + weight painting.**
  Eased channel graphs widget (read-only) and the weight-paint brush
  (falloff, renormalization, stroke undo, Shift subtract). 2145
  headless checks total.
- **2026-11-18 — session 3, item 3: M9 complete.** Asset presets,
  vertex-clustering LODs, box-collider glTF export, and Z-up /
  centimeter export options (baked into roots, animation-consistent).
  2112 headless checks total (26 new in test_gameflow).
- **2026-11-18 — session 3, item 2: viewport texture sampling.**
  Perspective-correct textured raster path (uv/w + 1/w), nearest +
  bilinear repeat sampling, app-owned texture cache, material filter
  setting, Load Texture UI. Fixed (with regressions): the M3-era
  CYCLIC BARYCENTRIC MISBINDING in the rasterizer (wrong smooth
  shading, depth and UV interpolation); the plain triangle's
  "perspective-correct" comment was aspirational — it is affine and
  now documented. 2086 headless checks total (846 in test_material).
- **2026-11-18 — session 3, item 1: glTF skins.** Skin export (joint
  nodes, IBMs, JOINTS_0/WEIGHTS_0, bone animation) + import with a
  numeric pose-equivalence round trip; space-aware skinning fixed a
  pre-existing bind bug; the M7 rest-pose leak fixed with an exact
  regression. 2015 headless checks total.
- **2026-11-18 — M10 complete, M9 partial (second session).**
  Preview renders (PNG/turntable/thumbnail, framing, image decode in
  the platform wrapper); origin tools and batch glTF export. M9's
  presets, LODs, collision shapes and axis settings remain.
- **2026-11-18 — M8 complete.** Clips, channels, easing, slerp,
  timeline + scrubbing + playback, Insert Key, clip persistence, glTF
  animation export (baked, independently validated). 1925 headless
  checks total (50 new).
- **2026-11-18 — M7 complete.** Armatures with chain posing,
  automatic weights + skinning in the viewport, analytic 2-bone IK,
  pose mode with bone picking and rotation, armature persistence.
  1836 headless checks total (42 new). Weight painting and glTF skin
  export recorded as next-session tasks.
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
