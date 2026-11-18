# Halcyon — CHANGELOG

The format follows keep-a-changelog (Added / Changed / Fixed /
Removed); dates are ISO. Everything listed here is built and tested —
PROGRESS.md carries the honest ledger of what is NOT done yet.

## [Unreleased]

### Added — stability + usability (2026-11-18, session 4)

- A deterministic fuzz suite: random bytes, truncations, mutations
  and GLB structural garbage through every loader under ASan/UBSan —
  crashes are never acceptable. It found and fixed nine leaked
  parser keys in project-load error paths.
- Hostile-count rejection, 1000-deep hierarchy and large-scene
  stress tests.
- A context hint bar (the current mode's key map), a first-run
  quick-start card, hover tooltips on the tool rail, and Frame
  Selected (F).

### Added — modeling quality (2026-11-18, session 4)

- Region-aware extrude (E): connected faces move as one seamless
  region; walls only on boundary edges.
- Angle-based unwrap: dihedral seams, edge-adjacency charts,
  grid-fitted packing inside the UV square.
- Knife (K): drag across a mesh to split faces along the stroke;
  undoable, with a preview line.
- Vertex grab (G) with proportional editing (O, smoothstep falloff).
- Snapping on gizmo drags (Ctrl-hold): grid 0.25, increment 0.1,
  vertex; View menu + Frame Selected (F). Edge slide deferred.

## [v0.2.0] — 2026-11-18

Robust glTF import (foreign rigs exact, materials, textures in all
three flavors, multi-primitive meshes), the fully interactive curve
editor (drag/easing/insert/delete, undoable), and convex-hull
collider export (_UCX naming convention, D-016).

## [Unreleased]

### Added — interactive curve editor (2026-11-18, session 4)

- Drag keys in time and value (re-sorted, clip range follows);
  double-click a key to cycle its easing (step/linear/smooth);
  double-click empty graph to insert into the nearest channel;
  right-click to delete. Every edit is a single undo command.
- New anim ops: hal_clip_key_move / _delete / _clone.

### Added — robust glTF import (2026-11-18, session 4)

- Foreign rigs import EXACTLY: bones carry rest offsets (the file's
  joint rest rotations) so bind matrices equal the file's node-chain
  rest — verified analytically against hand-built foreign fixtures,
  including child-before-parent joint orderings (which fixed a real
  import crash).
- Materials import (pbrMetallicRoughness); textures import embedded
  (bufferView), as data: URIs, and as external files (extracted to
  <glb>-textures/); multi-primitive meshes merge with UVs and skin
  influences.

## [v0.1.0] — 2026-11-18

The first usable release: modeling, materials/UVs with textures,
rigging/skinning, animation, glTF (with skins) + OBJ + STL
interchange with Z-up/unit options, batch export, box colliders,
LODs, presets, preview renders, theming, example projects, and the
docs set (user guide, shortcuts, performance). See README.md for the
feature list and known limitations.

## [Unreleased]

### Added — M11 polish (2026-11-18, session 3)

- Theme switching (File > Load Theme; a Daylight theme ships in
  examples/themes/).
- Example projects in examples/: a tool, a prop, and an animated
  character — generated and reload-verified by scripts/gen_examples.
- docs/shortcuts.md (every binding) and docs/user-guide.md (a
  start-to-export walkthrough).
- Fixed with regressions: material texture -1 parsing and the
  weights-before-mesh save order (weighted projects could not
  reload). FBX assessed and declined for v0.1 (docs/decisions.md).

### Added — curve editor + weight painting (2026-11-18, session 3)

- Curve editor panel: per-channel value-over-time graphs with the
  clips' easing baked in, key dots, a synced playhead (read-only —
  scrubbing and keying stay in the timeline).
- Weight painting: a Paint tool-rail mode; ray-hit brush with
  smoothstep falloff, per-vertex renormalization, Shift subtracts,
  one undo per stroke; the active bone follows pose-mode selection.

### Added — M9 game-asset workflow (2026-11-18, session 3)

- Asset presets on the Add menu: Crate, Barrel, Sword, Table.
- Vertex-clustering LOD generation (Edit > Generate LOD).
- Box-collider glTF export from the selected object's world AABB
  (convex hulls remain future work).
- glTF export options: Z-up axis and centimeter units (File menu
  toggles; applied by single, batch and collider exports; animation
  channels on root nodes convert consistently).
- Tests: test_gameflow — 26 checks.

### Added — viewport texture sampling (2026-11-18, session 3)

- Perspective-correct textured triangles in the CPU rasterizer
  (uv/w + 1/w with a per-pixel divide; intensities too on this path).
- Nearest and bilinear repeat-wrap sampling; per-material filter;
  a path-keyed texture cache (platform image loader); per-corner UVs
  survive near-clipping. Edit > Load Texture / Toggle Filter; the
  filter persists in the project format.
- Fixed (with regressions): the rasterizer's barycentric edge
  functions had bound to the wrong vertices since M3 (a cyclic
  shift — invisible for flat shading and constant-z triangles, wrong
  for smooth shading, depth, and UVs).

### Added — glTF skins: export and import (2026-11-18, session 3)

- Skinned meshes export as full glTF skins: joint node hierarchies,
  inverse bind matrices (rest^-1 * A^-1 * Wm), JOINTS_0/WEIGHTS_0
  with per-corner duplication, skeleton references, and baked
  bone-rotation animation on the joint nodes.
- Skin import rebuilds armatures and reattaches weights; joint
  rotation animations import as bone channels. Halcyon-exported
  rigged+animated characters round trip with world-space skinned
  vertices matching within 1e-4 (numeric pose-equivalence test).
- Fixed (with regressions): the M7 rest pose leaked posed parent
  rotations into bind matrices; skinning now handles meshes at a
  different transform than their armature (hal_mesh_apply_pose_ex).

### Added — M10 + M9 partial (2026-11-18)

- Preview rendering: framed lit PNG renders, turntable frame
  sequences, thumbnails (Render menu); scene framing; image decoding
  exposed through the platform wrapper (14 new checks).
- Origin tools (Origin to BBox Center — world-compensating and
  undoable; Origin to World Origin) and batch glTF export (one file
  per mesh object). Not yet from M9: presets, LODs, collision
  shapes, axis/units export options (recorded in PROGRESS.md).

### Added — M8: animation (2026-11-18)

- Clips with per-property channels (loc/rot/scale/bone), sorted keys,
  per-key easing (linear, smooth, step), quaternion slerp, looping.
- Timeline strip: ruler, per-channel key rows, playhead, scrubbing;
  playback with loop wrap in the frame loop; Space, I (insert key),
  clip management on the Animation menu; clips persist in the
  project format.
- glTF 2.0 animation export: baked 30 fps LINEAR samplers, accessor/
  bufferView numbering after mesh + image slots (validated with an
  independent JSON parser).
- Tests: test_anim — 50 checks. Pending (recorded in PROGRESS.md):
  curve editor widget, manual weight painting, glTF skin export.

### Added — M7: rigging (2026-11-18)

- Armatures: bones with parents, local pose rotations, chain posing.
- Skinning: 4 influences/vertex, automatic nearest-segment weights,
  inverse-bind deformation rendered in the viewport.
- Analytic 2-bone IK with reach clamping (tip-to-mouse in the UI).
- Pose mode: bone picking, bone-rotate modal, add child bone, bind
  mesh to armature; armature stick-figure drawing.
- Project format: bones, poses, skin refs, weight blocks (version 3).
- Tests: test_rig — 42 checks (chain posing, weight sums, IK reach
  and clamp, round trip). Known gaps, recorded in PROGRESS.md:
  manual weight painting and glTF skin export.

### Added — M6: materials & UVs (2026-11-18)

- PBR-style materials (base color, metallic, roughness, emissive,
  texture slot) with a scene material list and per-object slots;
  scene image list.
- Per-loop UV storage, compaction-safe; unwrap operators: planar,
  box, smart-lite — all undoable, on the Edit menu.
- UV editor widget in the sidebar; material panel (color entries,
  metallic/roughness sliders, New Material).
- Viewport shades by material base color + emissive.
- glTF export with materials, TEXCOORD_0, embedded textures; project
  format v3 (materials + images; v1/v2 still load). 818 new checks.

### Added — M5: export/import (2026-11-18)

- glTF 2.0 GLB export (meshes, normals, TRS + hierarchy) and import
  (own JSON DOM parser; common accessor subset; flattened
  transforms). OBJ export/import (global numbering, n-gons,
  relative indices). Binary STL export. Round-trip tests (51
  checks). File menu: Export glTF/OBJ/STL, Import glTF/OBJ.

### Added — M4: mesh modeling (2026-11-18)

- Mesh selection + picking; edit mode (Tab, 1/2/3, shift-extend);
  operators: extrude, inset, bevel, loop cut, subdivide, merge,
  delete, mirror, flip/recalc normals, smooth shading — all
  undoable through snapshot-swap commands; move gizmo with axis
  handles; modal rotate/scale; edit overlays; all six primitives in
  the Add menu; project format v2 with meshes (90 new checks).
  Fixed: the cube primitive was inward-wound since M3 (invisible
  under backface culling), loop-cut midpoint placement, a subdivide
  heap overflow.

### Added — M3: 3D viewport (2026-11-18)

- Mesh structure (verts/edges/polygon faces) + cube, sphere, plane,
  cylinder, cone, torus primitives; Newell + smooth normals; bounds.
- Orbit/pan/zoom camera with projection, ray unprojection and
  frustum-sphere culling.
- CPU rasterizer: depth-tested triangles (perspective-correct),
  depth-interpolated lines, near-plane clipping, scissoring.
- Viewport renderer: horizon grid, headlight solid shading, wireframe
  mode, axis gizmo; wired into the editor with MMB/Alt+LMB orbit,
  Shift+MMB pan, wheel zoom, and a Wire toggle.
- Tests: test_mesh (88 checks), test_viewport (62 checks).

### Added — M2: own UI system (2026-11-18)

- Own 2D painter (canvas): AA rounded rects, lines, circles,
  triangles, clip stack, HiDPI-aware logical coordinates.
- Theme system: `.haltheme` strict parser, the Faded Dream default
  palette, save/load round trips (docs/themes.md).
- Widget toolkit: labels, buttons, toggles, sliders, entries,
  panels, rows/columns, tabs, tree views, menu bars with overlay
  menus; measure/arrange layout engine; headless input routing
  (hover, press, grab, focus, menu semantics).
- Editor shell: menus, outliner (scene-bound), properties panel
  (undoable edits), status bar, viewport rect reserved for M3;
  project new/open/save/save-as + shortcuts; autosave driven from
  the frame loop.
- Platform: file/message dialog wrappers.
- Tests: test_ui — 152 checks, all headless.

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
