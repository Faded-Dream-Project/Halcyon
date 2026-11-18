# The .halcyon project format (version 1)

A `.halcyon` file is a strict, line-oriented, human-readable text file.
Halcyon's parser is fail-closed: anything that does not match this
grammar is a load error naming the line number — a project file never
loads half-way.

## Grammar

```
file        := header scene? object*
header      := "halcyon" WS version LF
version     := integer          (1; newer versions refuse to load)
scene       := "scene" WS quoted-name LF
object      := "object" WS id WS quoted-name WS type LF
               property*
               "end" LF
property    := "parent" WS parent-id LF
             | "loc" WS float WS float WS float LF
             | "rot" WS float WS float WS float WS float LF
             | "scale" WS float WS float WS float LF
             | "flags" WS flag* LF
flag        := "visible" | "selected" | "-"
type        := "mesh" | "armature" | "camera" | "light"
```

- `WS` is one or more spaces or tabs. Blank lines are allowed anywhere.
- `quoted-name` is a `"double-quoted"` UTF-8 string with no embedded
  quotes or newlines (v1 has no escape sequences).
- `id` is an unsigned integer, unique in the file, never 0. `parent-id`
  is 0 (no parent) or the id of an object **defined earlier** in the
  file — forward references and self-references are refused, which
  makes parenting cycles unrepresentable.
- `loc` is the location (x y z). `rot` is the rotation quaternion
  (x y z w; loaded values are normalized). `scale` is the scale
  (x y z). Transforms are the glTF TRS triple; see docs/decisions.md
  D-005 for the coordinate conventions (right-handed, Y-up, 1 unit =
  1 meter).
- `flags` is a space-separated list; `-` alone means "no flags".
  Unknown flags are errors.
- Floats are written with 9 significant digits (`%.9g`), which
  round-trips binary32 exactly.

## Example

```
halcyon 1
scene "Sword concept"
object 1 "Blade" mesh
  parent 0
  loc 0 0.5 0
  rot 0 0 0 1
  scale 1 1 1
  flags visible
end
object 2 "Guard" mesh
  parent 1
  loc 0 0.2 0
  rot 0.0851135 0.0200637 0.413175 0.906741
  scale 1 1 1
  flags visible selected
end
```

## Semantics and guarantees

- **Saving is atomic.** Halcyon writes `<path>.tmp~`, fsyncs, then
  renames over the destination — a crash mid-save can never destroy an
  existing project, and a `.tmp~` file left behind means the save was
  interrupted, never that the original is damaged.
- **Autosave files are ordinary project files.** The autosave path is
  `<project>.autosave`; rename it and it opens like any project.
- **Versioning is additive.** A new format version only ever adds keys;
  Halcyon refuses to load a version newer than it understands (with a
  clear message) rather than guessing.

## What is not in version 1 (honest list)

Mesh geometry, materials, UVs, armatures, animation, and scene
settings (units, axis, render hints) are **not** representable in
version 1 — they land with milestones M4–M7 and bump the format
version additively. A v1 file saved by this Halcyon contains objects,
names, types, parenting, transforms, visibility and selection only.
