# Halcyon performance record

## What was measured, where, and how

The only GL environment this project's sandbox can run is Mesa
llvmpipe (software rasterization) under Xvfb with FDK_GL_FORCE=1.
llvmpipe executes the full GL 3.3/4.5 core pipeline in software:
it verifies CORRECTNESS (pixels, compositing, shaders, readback)
and NOTHING about hardware performance. No frame-time number from
this environment is a performance claim, and none is quoted as one.

## The tooling (all landed)

- `--gpu-info`: the engaged context's flavor/version/vendor/
  renderer (fdk_window_gl_info), the fdk_gl_status verdict, the
  loader path (glX/egl), MSAA samples, timer-query availability.
  Sandbox output: "GL core 4.5 — Mesa / llvmpipe (LLVM 19.1.7, 256
  bits)", status OK, loader glX, msaa 0 (llvmpipe reports no
  samples), timer yes.
- `F9` in-app: the frame-time overlay — CPU frame time (the frame
  callback, CLOCK_MONOTONIC) + the GPU timer query (GL_TIME_ELAPSED,
  read one frame late) + triangles/frame + MSAA.
- `make bench-run` / `--bench N`: an N-triangle grid scene, 40
  presented frames, CPU ms + GPU ms averages. HONEST STATUS: the
  100k run currently crashes in its own scene teardown (ASan
  use-after-free) — the bench harness itself is next-session work;
  no numbers exist from any machine yet.

## The target (unmeasured)

"60+ fps at 1M triangles on a GTX 1050 Ti (i5-8400)" is the
requirement. The renderer is built for it (buffers uploaded once,
per-frame work is one MVP + one indexed draw per object, MSAA via
one blit), but NO hardware measurement has happened. Run
`make bench-run` on the target machine and record the numbers here.

## CPU rasterizer

The CPU raster path left the runtime entirely (it is the test
oracle). Its historical numbers (editor-size scenes, headless) are
in the git history's PROGRESS entries; it was never a 1M-triangle
path and is not the performance surface anymore.
