# Halcyon — dependencies

Goal: **as close to zero as possible.** Halcyon writes its own math,
mesh structures, rasterizers, UI toolkit, file parsers and exporters.
Anything third-party must be permissively licensed (MIT/BSD/zlib/
public domain), justified here in writing, and listed in
THIRD-PARTY-NOTICES.md. No copyleft anywhere.

## Direct dependencies

| Dependency | Role | License | Justification |
|---|---|---|---|
| FDK 1.0.0 | platform layer: window, drawing surface, input events, clipboard, event loop, text rasterization, PNG encode | FDK Proprietary v1.0 (applications governed separately) | The project's stated architecture: FDK is used as a MINIMAL platform layer only. Halcyon links against an installed FDK (`pkg-config fdk`); it never bundles, embeds, copies or redistributes FDK source or runtime. FDK's license expressly leaves applications and their licensing independent. |

That is the whole direct list. Notably NOT dependencies (deliberately):
GTK, Qt, SDL, ImGui or any other UI toolkit; OpenGL/Vulkan headers;
zlib (Halcyon writes its own deflate-free file formats and reuses
FDK's PNG encoder through the wrapper); any JSON/XML library (the
project format is a bespoke documented format; glTF's JSON is written
by Halcyon's own small serializer).

## Transitive / system (via FDK, not Halcyon's tree)

libX11/libXext/libXrandr/libXrender (X11 backend), fontconfig (font
discovery), optional dlopen'd HarfBuzz/FriBidi (text shaping), glibc.
Listed in THIRD-PARTY-NOTICES.md; Halcyon neither links them directly
nor vendors them.

## Policy

1. Before any third-party source enters the tree: license check
   (permissive only), a paragraph here saying why writing it in-house
   is the worse choice, and a THIRD-PARTY-NOTICES.md entry with
   license text and provenance.
2. Copyleft (GPL/LGPL/AGPL/SSPL/…) anywhere in the graph is a hard
   no.
3. Vendored code keeps upstream file boundaries and license headers
   intact; no mixing into Halcyon-authored files.
