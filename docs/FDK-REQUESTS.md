# Halcyon — FDK requests

What Halcyon needs from FDK that its public API does not expose today.
Each entry states what, why, and a proposed API. Until an entry is
answered by an FDK release, Halcyon builds a clean fallback and keeps
working — requests are never blockers, and this file is the only place
FDK's missing pieces are tracked. (Halcyon can only change itself; FDK
is not edited from here.)

Format: `R-### status — title`.

**2026-11-19 — EVERY REQUEST ANSWERED.** FDK's owner pulled the
requests file, fixed all six on FDK `main` (commit `3414d94`,
"Halcyon's FDK requests, session 7/8: all six answered"), and
withdrew the 1.0.0 re-release the defects shipped in (release and
tag deleted; FDK is a development version again — build from
`main`). Statuses below are updated accordingly; each entry names
what changed and what Halcyon should do about it.

A note on numbering: this file historically carried TWO different
"R-002" entries (the text-rasterization API and the frame-callback
argument order). The entries are renumbered into one clean
sequence below; the original numbers are kept in the titles as
aliases so old notes still resolve.

---

## R-001 ANSWERED (fixed on FDK main, 2026-11-19) — ext-surface GL usable on the X11 backend

The ask (revised 2026-11-18, session 6): fill the adopted window's
GL vtable slot on X11 — `fdk_ext_gl_make_current` /
`fdk_ext_gl_set_frame_callback` answered `FDK_ERR_UNSUPPORTED`
even with the lane loaded (`FDK_EXT_CAP_GL` true via
`FDK_GL_FORCE=1` and a GPU-mode probe window).

**Fixed.** Root cause: an adopted extension window never acquires a
framebuffer through the engine, so nothing ever attached the lane —
`pw->gl_win` stayed NULL and every ext GL op refused. Both
backends' `window_gl_make_current` and `window_gl_set_frame_cb`
now attach the lane on demand through the same engage machinery a
normal window gets (lane load, context, shared root, core-profile
app context; `fdk_x11_gl_window_try_init` is xid-agnostic, and the
Wayland twin got the identical fix). An engage failure still
returns the honest per-reason refusal.

**Halcyon action:** drop no workarounds — the ext GL calls now
engage on first use. `fdk_ext_gl_make_current(surface)` /
`fdk_ext_gl_set_frame_callback(...)` on the adopted X11 window
work as documented.

## R-002 ANSWERED (fixed on FDK main, 2026-11-19) — text rasterization into caller-owned memory (original R-002)

The ask: rasterize a glyph or line into an application-owned ARGB
buffer instead of onto an fdk_surface — cached label textures,
viewport overlay sprites, the curve editor's axis-label atlas.

**Shipped.** `fdk_font_rasterize_utf8` (include/fdk/fdk_text.h):
the same shaped placement walk `fdk_surface_draw_utf8` uses
(identical shaping, kerning, fallbacks, glyph cache — results agree
pixel-for-pixel with on-surface draws), blended into caller-owned
ARGB8888 memory. Signature as shipped (the sketch grew a `color`
and a `baseline_y` — tint and vertical placement were the two
things the sketch could not express):

```c
fdk_result fdk_font_rasterize_utf8(const fdk_font *font,
                                   const char *utf8, size_t byte_len,
                                   fdk_color color, fdk_u32 *dst,
                                   fdk_i32 w, fdk_i32 h,
                                   fdk_i32 stride_bytes,
                                   fdk_i32 baseline_y,
                                   fdk_rect *out_ink);
```

Straight source-over including the destination alpha; the buffer is
the clip; `out_ink` returns the painted region (empty box = no
ink). No surface, no context, no display. Twelve-leg test in FDK's
suite (`tests/test_font_rasterize.c`: NULL contracts, ink-in-box,
tint, tiny-buffer clipping, whitespace).

**Halcyon action:** the offscreen-surface + copy workaround can be
retired for new code when building against FDK main.

## R-003 ANSWERED (fixed on FDK main, 2026-11-19) — ext GL frame callback invoked with swapped arguments (original duplicate R-002, session 7)

The ask: `fdk_ext_gl_frame_fn` is declared `(surface, user)` but
the X11 present arbitration invoked it as `fn(user, surface)`;
Halcyon's trampoline matched the REAL order and needed the swap
undone at the source.

**Fixed.** All four invocation sites (X11 `cb_flush` +
`cb_readback`, Wayland `cb_flush` and its readback twin) now call
`fn(surface, user)` per the header. The swap-symmetric draws that
hid this are exactly why the counting-callback proof mattered.

**Halcyon action:** remove `hal_gpu_frame_trampoline`'s reversed
argument shuffle — the callback now receives the DOCUMENTED order
when building against FDK main.

## R-004 ANSWERED (fixed on FDK main, 2026-11-19) — the fdk_gl_viewport widget composites UNDER the CPU frame on X11 (original R-003, session 7)

The ask: the widget's GL content was never visible — the lane's
present drew a fullscreen CPU quad over the back buffer after the
tree walk ran the render callback; pixel-proven twice
(server-side XGetImage); tests only pinned the reason enum.

**Fixed, with the pixel proof the widget never had.** The widget
now reads the region back right after the application's render
callback (a new `window_gl_read_region` platform op, implemented on
both backends through the lane's own `glReadPixels`) and composites
it into the CPU surface as an ordinary opaque sprite IN PAINT
ORDER: later siblings (menus, popups, tooltips) paint after the
viewport in the walk and therefore draw OVER it — the documented
promise, now actually true. FDK's test suite grew the pixel leg:
under Xvfb + llvmpipe + `FDK_GL_FORCE=1`, a callback that clears to
a known color must show that color in the window's own CPU surface
(`tests/test_gl_viewport.c`, "R-003: GL region composites into the
frame (paint order)").

**Halcyon action:** none required — Halcyon stayed on the
ext-surface frame callback (present arbitration), which remains the
right shape for a whole-window GPU editor. The widget is now a
legitimate option for embedded regions.

## R-005 ANSWERED (documented on FDK main, 2026-11-19) — GL_UNSIGNED_BYTE index draws silently rasterize nothing on llvmpipe core (original R-004, session 7)

The ask: `glDrawElements(GL_UNSIGNED_BYTE, ...)` on the llvmpipe
core 4.5 context draws NOTHING and reports NO error; SHORT/INT
work. A spec-legal byte-index path fails silently for other
add-ons too.

**Answered honestly: this is driver-side and unfixable in FDK**
(FDK's own drawing never uses indices — the present path is an
unindexed fullscreen quad). The limitation is now documented in
`fdk_gl.h`'s app-context contract ("Known driver quirks in the app
context"): applications drawing through the app context should
index with `GL_UNSIGNED_SHORT` or `GL_UNSIGNED_INT`; the byte-index
path is unreliable on at least Mesa's software rasterizer.

**Halcyon action:** none — Halcyon's renderer already uses
UNSIGNED_INT everywhere (this request was filed for other add-ons'
sake).

## R-006 ANSWERED (fixed on FDK main, 2026-11-19) — a Wayland-backend FDK build + the GPU window's Wayland kind (original R-005, session 7)

The ask: a Wayland-enabled FDK build (the installed build was
X11-only; `FDK_PLATFORM_WAYLAND` -> `FDK_ERR_NO_DISPLAY`), plus
weston/sway in the test rig and xdg-shell bindings to finish
Halcyon's Wayland ext-surface kind.

**Answered in two parts.**

1. *The channel guarantee:* FDK's release workflow now GATES on
   the Wayland backend — the release fails loudly if
   `libfdk.so.1` does not NEEDED-link `libwayland-client` (the
   build job already installs `libwayland-dev` +
   `libxkbcommon-dev`, and the Makefile's auto-detection builds
   Wayland whenever those are visible). An X11-only bundle can no
   longer ship silently.
2. *The loader crash that made the Wayland path untestable:* see
   R-007 — with it fixed, `fdk_init` + the first
   `fdk_window_create` on a GPU-mode Wayland session no longer
   SEGV, and Halcyon's `HALCYON_BACKEND=wayland` opt-in can be
   exercised against the staged weston rig (`scripts/wl-env.sh`).

**Halcyon action:** build FDK from `main` (or the next release,
which will carry the backend gate); finish the Wayland ext-surface
kind against the weston rig when ready.

## R-007 ANSWERED (fixed on FDK main, 2026-11-19) — Wayland EGL lane loader crashes: eglGetProcAddress CALLED before it is resolved (original R-006, session 8)

The ask: `fdk_wayland_egl_try_load`'s app-context block called
`egl->fn.eglGetProcAddress("eglBindAPI")` ~20 lines BEFORE
`EGL_RESOLVE(eglGetProcAddress, ...)` filled the field — NULL
deref, SEGV with pc=0, deterministic for EVERY GPU-mode window on
Wayland; reproduced under headless weston and matching a real
desktop report frame-for-frame. The fix was a two-line move.

**Fixed exactly as asked.** `eglGetProcAddress` is now resolved
FIRST (before the app-context block), and `eglBindAPI` gained a
`dlsym` fallback alongside the loader lookup (it is a direct
export of every EGL 1.2+ loader, so the fallback is not merely
theoretical).

**Halcyon action:** remove the forced `FDK_PLATFORM_X11` (XWayland)
workaround in `src/platform/platform.c` when building against FDK
main — `$WAYLAND_DISPLAY` sessions can use the native backend
again (`HALCYON_BACKEND` keeps working as the explicit override).

---

## The GPU viewport path note (requested 2026-11-18, Halcyon session 3)

The original preference-ordered request (a window-system-backed
GPU surface for `hal_surface`, or at least a CPU-framebuffer blit
path) was satisfied by the 1.0.0 extension API itself — the
ext-surface GL lane runs the whole editor window on the GPU with
the UI composited via `fdk_texture`, and Halcyon's renderer keeps
its CPU rasterizer for content that needs it. With R-001/R-003/R-007
fixed, that path works on both backends. No further ask.

## Withdrawal context (2026-11-19)

The 1.0.0 re-release that shipped the six defects above has been
withdrawn by FDK's owner: the release and its tag are deleted, the
CHANGELOG carries a 2026-11-19 withdrawal record naming these
requests, and FDK is a development version again (build from
`main`; the packaging channel stays wired and will re-cut when the
owner says so). Halcyon should treat FDK `main` post-`3414d94` as
the fixed baseline and expect the next numbered release to carry
all of the above.
