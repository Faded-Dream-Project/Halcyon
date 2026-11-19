# Halcyon — THIRD-PARTY-NOTICES

Halcyon is written from scratch; the goal is a dependency set as close
to zero as possible. This file lists every third-party component that
ends up in Halcyon's source tree or runtime, with its license and
provenance. Each entry requires a matching justification in
docs/dependencies.md. No copyleft component may ever appear here.

## Current entries

None. Halcyon's tree contains only original code.

## Linked (not bundled) components

The following are not part of Halcyon's source tree and are never
redistributed by it; they are system dependencies resolved at build or
run time:

| Component | Role | License | Notes |
|---|---|---|---|
| [FDK](https://github.com/Faded-Dream-Project/FDK) | platform layer: window, surface, input, text rasterization, clipboard | FDK Proprietary v1.0 | Halcyon links against an installed FDK via pkg-config. FDK's license explicitly leaves applications built with it to their own terms. |
| libX11 / Xlib, libXext, libXrandr, libXrender | X11 backend (pulled in transitively by FDK) | MIT / X11 | System libraries. |
| fontconfig, fontconfig-discovered system fonts | system UI font discovery (inside FDK) | MIT | System service. |
| HarfBuzz / FriBidi (runtime, dlopen'd by FDK) | text shaping (inside FDK) | MIT / ISC | Optional at runtime. |
| libc, libm, libpthread | the C runtime | LGPL-2.1-or-later (glibc) | System runtime, dynamically linked; the standard exception for every C program on Linux. |

If Halcyon ever vendors third-party source (permissive only), it is
appended here with its license text and provenance before the commit
lands.
