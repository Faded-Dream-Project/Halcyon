# Halcyon shortcuts

Every binding in the editor, grouped by mode. Menus list the same
bindings next to their items.

## Global

| Binding | Action |
| --- | --- |
| Ctrl+N | New project |
| Ctrl+O | Open project |
| Ctrl+S | Save project |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z | Redo |
| Ctrl+Q | Quit |

## Viewport (all modes)

| Binding | Action |
| --- | --- |
| Middle-drag (or Alt+Left-drag) | Orbit |
| Shift+Middle-drag | Pan |
| Wheel | Zoom |
| F | Frame the selected object |

Snapping (View menu sets the mode, Ctrl-hold applies during gizmo
drags): grid 0.25, increment 0.1, or snap to the nearest vertex of
another object.

## Object mode

| Binding | Action |
| --- | --- |
| Left-click | Select object (empty space deselects) |
| Left-drag on a gizmo axis | Move along that axis |
| R | Rotate (modal: move the mouse, click to accept, Esc cancels) |
| S | Scale (modal, same interaction) |
| Tab | Enter edit mode on the selected mesh (pose mode on an armature) |

## Edit mode (mesh)

| Binding | Action |
| --- | --- |
| Tab | Back to object mode |
| 1 / 2 / 3 | Vertex / edge / face selection |
| Left-click | Select (Shift extends) |
| G | Grab selected verts (drag on the camera plane) |
| O | Toggle proportional editing (falloff during grabs) |
| K | Knife: drag across the mesh, release to cut |
| E | Extrude selected faces (region-aware) |
| I | Inset selected faces |
| B | Bevel selected edges |
| Ctrl+R | Loop cut (hover an edge, or select one first) |
| M | Merge selected verts |
| X | Delete (verts in vertex mode, faces otherwise) |
| A / Alt+A | Select all / deselect all |

## Pose mode (armature)

| Binding | Action |
| --- | --- |
| Tab | Back to object mode |
| Left-click | Select a bone (also sets the paint brush's bone) |
| R | Rotate the bone (modal) |
| I | Insert a rotation key for the bone |

## Animation

| Binding | Action |
| --- | --- |
| Space | Play / pause the active clip |
| I (object mode) | Insert TRS keys for the selected object |
| Left-drag in the timeline | Scrub the playhead |

## Weight paint mode

| Binding | Action |
| --- | --- |
| Paint toggle (tool rail) | Enable/disable |
| Left-drag over a bound mesh | Add weight to the active bone |
| Shift+Left-drag | Subtract weight |
