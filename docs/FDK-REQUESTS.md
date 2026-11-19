# Halcyon — FDK requests

What Halcyon needs from FDK that its public API does not expose today.
Each entry states what, why, and a proposed API. Until an entry is
answered by an FDK release, Halcyon builds a clean fallback and keeps
working — requests are never blockers, and this file is the only place
FDK's missing pieces are tracked. (Halcyon can only change itself; FDK
is not edited from here.)

Format: `R-### status — title`.

---

## R-001 open (revised 2026-11-18, session 6) — ext-surface GL usable on the X11 backend (or a Wayland-enabled build)

The 1.0.0 extension API IS the GPU draw path (fdk_extend.h:992-1014:
fdk_ext_gl_make_current / set_frame_callback / swap / fences, plus
fdk_texture for CPU-pixel compositing and FDK_RENDER_GPU). The
sub-rect API originally sketched here is not needed and is withdrawn.
What is still needed to USE it from Halcyon:

1. On X11, fdk_ext_gl_make_current and fdk_ext_gl_set_frame_callback
   answer FDK_ERR_UNSUPPORTED even with the lane loaded
   (FDK_EXT_CAP_GL true via FDK_GL_FORCE=1 and a GPU-mode probe
   window). The adopted window's GL vtable slot is never filled by
   the X11 backend. Ask: fill it (GLXWindow on the adopted xid +
   the shared lane context), or document ext GL as Wayland-only.
2. The installed build has no Wayland backend at all (ldd: only
   libX11; FDK_PLATFORM_WAYLAND -> FDK_ERR_NO_DISPLAY). Ask: a
   build with Wayland enabled, if the Wayland path is the answer.

Below is the original API survey (kept for reference).

**Deleted as a request.** FDK 1.0.0 already ships a real GPU draw
path; the earlier "blocked on FDK" reading was wrong. What FDK does
NOT have is a GL *sub-rect* of a normal fdk_window — but it does not
need one for Halcyon: the extension-surface GL lane runs the whole
editor window on the GPU, with the UI composited via fdk_texture.
The API actually used (paths + line numbers from the installed
headers, /home/z/my-project/fdk-install/include/fdk/):

- fdk_extend.h:992-1014 — "GL for add-ons (P2: a real API, not a
  recipe)": fdk_ext_gl_make_current(surface),
  fdk_ext_gl_set_frame_callback(surface, fn, user),
  fdk_ext_gl_swap, fdk_ext_gl_fence_insert/wait. The callback runs
  from FDK's frame scheduling with the OWNING window's lane GL
  context CURRENT on the surface's native drawable: draw, return,
  FDK swaps. "The callback replaces the paint engine for that
  window's buffer — software content rides fdk_texture if mixed."
- fdk_extend.h:162-171 — FDK_EXT_CAP_GL: the EGL/GLX presentation
  lane actually initialized (hardware GL, not a refused software
  renderer).
- fdk_extend.h:285-353 — fdk_ext_register_kind +
  fdk_ext_surface_create: the add-on's create callback builds the
  native surface (X11: XCreateWindow; Wayland: wl_compositor +
  role), FDK adopts it and routes the full fdk_event_data stream
  (configure, expose, focus, pointer, keys, close).
- fdk_surface.h:661-681 — fdk_texture_create/update/draw/destroy:
  app-owned RGBA pixels composited into any fdk_surface in EVERY
  render mode; in GPU mode "uploads into the window's lane context
  at draw time (cached per texture) and draws as a batched-machinery
  quad".
- fdk_core.h:53-79 — FDK_RENDER_GPU: the lane is REQUIRED; the
  classic framebuffer path is disabled; window creation FAILS
  (FDK_ERR_UNSUPPORTED) if the GL stack cannot initialize — "never a
  silent fallback". Selected via fdk_init_options.render_mode or
  FDK_RENDER=gpu.

Minimal shape (as used in src/platform/gpu_window.c):

    fdk_init(&ctx, &(fdk_init_options){ .app_id = ..., .render_mode =
        FDK_RENDER_GPU });
    fdk_ext_register_kind(ctx, "halcyon-editor", &ops, app, &kind);
    fdk_ext_surface_create(kind, &surf);        /* create cb: XCreateWindow */
    fdk_ext_gl_set_frame_callback(surf, frame, app);  /* lane ctx current */
    /* frame(): GL draws the 3D scene, then
       fdk_texture_draw(render_target, ui_texture, rect) composites
       the CPU-painted UI; FDK swaps. */

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


---

## R-002 open (2026-11-19, session 7) — ext GL frame callback invoked with swapped arguments

fdk_extend.h declares `fdk_ext_gl_frame_fn` as
`void (*)(fdk_ext_surface *surface, void *user)`, but the X11 present
arbitination (fdk_x11_window_gl_set_frame_cb stores fn+user; cb_flush
calls `fn(user, surface)`) invokes it as fn(user, surface) — the
documented parameter order is swapped at the call site. Halcyon's
trampoline (src/platform/platform.c, hal_gpu_frame_trampoline) matches
the REAL order; swap back when FDK fixes the invocation. Proof: a
callback compiled to the declared order saw its counter stay 0 across
200 invocations while the swap-symmetric draws still ran.

## R-003 open (2026-11-19, session 7) — the fdk_gl_viewport widget composites UNDER the CPU frame on X11

fdk_gl.h promises the GL viewport widget's region composites in the
tree's paint order with "menus, popups and tooltips that overlap the
region draw OVER it". On FDK 7fbba8a/X11 the widget's GL content is
never visible at all: the lane's present draws a fullscreen CPU quad
(draw_fullscreen_quad) over the back buffer AFTER the tree walk ran
the widget's render callback, and nothing excludes the widget's
region. Pixel-proven twice (server-side XGetImage): the widget
example's animated clear and a constant-red callback clear both
invisible — the region shows the widget's CPU fill box (15,18,23).
tests/test_gl_viewport.c only pins the reason enum/engage identity,
not pixels. Halcyon therefore uses the ext-surface GL frame callback
(present arbitration) instead, which pixel-verified end to end.

## R-004 open (2026-11-19, session 7) — GL_UNSIGNED_BYTE index draws silently rasterize nothing on llvmpipe core

glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_BYTE, ...) on the
llvmpipe core 4.5 context (via the engaged lane) draws NOTHING and
reports NO error; GL_UNSIGNED_SHORT and GL_UNSIGNED_INT work. Found
by the session's spike (half-window triangle invisible, fullscreen
quad fine). Halcyon's renderer uses UNSIGNED_INT everywhere; a
spec-legal byte-index path would fail silently for other add-ons too.

## R-005 open (2026-11-19, session 7) — a Wayland-backend FDK build + the GPU window's Wayland kind

Halcyon's ext-surface window kind implements the X11 create path
(XCreateWindow via the documented native display handle). The
Wayland path (wl_compositor surface + an xdg_toplevel role through
the add-on's own registry) is sketched in the code but REFUSED
honestly: this sandbox has no Wayland compositor and FDK was built
X11-only (FDK_DISABLE_WAYLAND auto). To finish it: a Wayland-enabled
FDK build, weston/sway in the test rig, and the generated
xdg-shell-client protocol bindings.
