# Halcyon user guide (v0.1)

Halcyon is a small, self-contained 3D modeler for game assets. This
guide takes you from an empty scene to an exported, animated glTF.

## Starting up

Build and run per the README (`make && ./build/halcyon`). You start
in a default scene with a cube. The layout: menu bar and tool rail on
top, the outliner and properties on the right, the 3D viewport in the
middle, the animation timeline along the bottom (once a clip exists).

**Navigation**: middle-drag (or Alt+left-drag) orbits, Shift+middle
pans, the wheel zooms.

## Modeling

Press **Tab** on the selected cube to enter edit mode. Pick a
selection mode with **1/2/3** (vertex/edge/face) and click geometry
(Shift extends). The Edit menu (or the shortcuts in
docs/shortcuts.md) has the operators: extrude, inset, bevel, loop
cut, subdivide, merge, delete, mirror, and normal tools. Everything
is undoable (Ctrl+Z). The move gizmo in object mode drags along its
colored axis handles; **R** and **S** start modal rotate/scale.

New geometry comes from the **Add** menu: the six primitives, the
asset presets (crate, barrel, sword, table), armatures, and lights.

## Materials and UVs

Select a mesh and use **Edit > New Material**, then edit the color,
metallic and roughness in the Material panel. **Edit > Load
Texture...** assigns an image; the viewport shows it with
perspective-correct sampling (Edit > Toggle Texture Filter switches
nearest/bilinear). UVs: unwrap with the Edit menu's Unwrap entries
(planar/box/smart-lite) and inspect the result in the UV Editor
panel.

## Rigging and animation

**Add > Armature**, then Tab into pose mode, click a bone, and use
**Edit > Add Bone** to extend the chain. **Edit > Bind Mesh to
Armature** computes automatic weights. Animate: **Animation > New
Clip**, pose bones or move objects, press **I** to insert keys, and
**Space** to play. The timeline scrubs; the Curves panel graphs the
channels. Fine-tune weights with the **Paint** tool-rail toggle
(Shift-drag subtracts).

## Exporting

**File > Export glTF (.glb)** writes meshes, normals, materials,
textures, skins and animations — open the result in any glTF viewer
or engine. The File menu also has OBJ and binary STL export, batch
per-object glTF, box colliders, and the **Z-Up / Centimeters**
toggles for engines that expect those conventions. **Render >
Preview PNG / Turntable / Thumbnail** renders stills and turntables
through the same rasterizer.

## Where to look next

- docs/shortcuts.md — every binding
- docs/project-format.md — the .halcyon file format
- docs/themes.md — writing .haltheme files
- examples/ — a tool, a prop and an animated character project
  (File > Open)
