# Halcyon — FDK requests

What Halcyon needs from FDK that its public API does not expose today.
Each entry states what, why, and a proposed API. Until an entry is
answered by an FDK release, Halcyon builds a clean fallback and keeps
working — requests are never blockers, and this file is the only place
FDK's missing pieces are tracked. (Halcyon can only change itself; FDK
is not edited from here.)

Format: `R-### status — title`.

---

## R-001 open — GPU drawing surface (GL/Vulkan) for the 3D viewport

**What.** A way for an application to render with OpenGL/Vulkan into
a region of an FDK window, presented by FDK alongside the CPU-painted
UI.

**Why.** Halcyon's M3 viewport ships as a CPU rasterizer (decision
D-007) — deterministic, headless-testable, and smooth on the target
hardware at editor sizes. The roadmap leaves a GPU renderer open for
later. FDK today rasterizes every pixel on the CPU itself: its GL/EGL
lanes are *present* lanes (they upload finished CPU frames), and the
extension API's `fdk_ext_gl_make_current` exists but is documented for
extension *surfaces* (own native windows), not for a sub-rect of a
normal application window.

**Status of the seam today.** FDK's extension API does expose native
display/surface handles (`fdk_ext_get_native_display`,
`fdk_ext_surface_native`), so a Halcyon-owned GL surface is reachable
in principle — but composing it *inside* the main window means a
separate native child window (a subsurface on Wayland), which fights
the single-window architecture (D-001) and HiDPI compositing.

**Proposed API (sketch).**
```c
/* Render into a rect of an existing fdk_surface-backed window with a
 * caller-owned GL context; FDK composites the result under the CPU
 * frame at present time. */
fdk_result fdk_window_gl_region(fdk_window *win, fdk_rect region,
                                fdk_gl_region **out);
/* Make the region's context current on the UI thread; draw; FDK's
 * next fdk_surface_present() composites the region. */
fdk_result fdk_gl_region_make_current(fdk_gl_region *r);
fdk_result fdk_gl_region_swap(fdk_gl_region *r);
```
**Fallback in the meantime.** CPU rasterizer only; the viewport
already meets the performance target without GL.

---

## R-002 open — text rasterization into caller-owned memory

**What.** Rasterize a glyph (or a line) into an application-owned
ARGB buffer instead of directly onto an fdk_surface.

**Why.** Halcyon's UI composites text in places where there is no
surface yet (cached label textures, viewport overlay sprites, the
future curve editor's axis labels in an offscreen atlas). Today
Halcyon works around it by drawing text onto a temporary offscreen
surface through the wrapper and copying — correct but double-handled.

**Status.** Not blocking: the offscreen-surface workaround is clean
and tested. Recorded because it is the natural next text API
(FDK's own "unstable tier" queue mentions per-glyph shaping and raw
framebuffer access as queued candidates).

**Proposed API (sketch).**
```c
typedef struct { int x0,y0,x1,y1; } fdk_glyph_box;
fdk_result fdk_font_rasterize_utf8(const fdk_font *font,
                                   const char *utf8, size_t len,
                                   fdk_u32 *dst, int w, int h,
                                   int stride, fdk_glyph_box *out);
```
**Fallback.** Offscreen surface + copy (in use today).

## GPU viewport path (requested 2026-11-18, Halcyon session 3)

Halcyon's viewport rasterizes on the CPU (see docs/performance.md:
~73 fps at 100k triangles single-threaded at 1280x720 on a slow
sandbox — fine for v0.1, but texture sampling and antialiasing will
raise the per-pixel cost). Requested, in preference order:

1. A window-system-backed GPU surface (Vulkan preferred, OpenGL ES
   acceptable) that `hal_surface` can wrap, so Halcyon can hand the
   rasterizer's output — or ideally render — on the GPU.
2. Even without a full GPU API: a blit path that uploads a CPU
   framebuffer as a texture (zero-copy when possible) — this alone
   removes the final CPU copy from the present path.
3. Shader-adjacent primitives are NOT needed; Halcyon's rasterizer
   already computes shading, UVs, skinning and texturing and would
   only need the surface/blit layer.

Use case: viewport interactivity at 200k+ triangles with bilinear
texturing on mid-range hardware (Intel i5-8400 class).
