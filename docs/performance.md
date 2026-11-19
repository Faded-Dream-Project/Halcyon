# Viewport performance

Measured with `make bench` (`build/bench`): a solid-shaded grid mesh
tilted toward the camera, rendered at **1280x720 with the grid on**,
24 runs per size, reporting wall-clock `hal_render_view` time.
The benchmark builds with clean **-O2 objects, no sanitizers**, so
the numbers are honest timings rather than instrumented ones.

## Numbers (this development sandbox, 2026-11-18)

| Triangles | min | median | max | ~fps (median) |
| --------- | --: | -----: | --: | ------------- |
| 10 000    | 5.80 ms | 5.86 ms | 7.61 ms | ~171 |
| 50 000    | 9.36 ms | 9.62 ms | 12.54 ms | ~104 |
| 100 000   | 13.62 ms | 13.67 ms | 14.06 ms | ~73 |

The sandbox CPU is slower than the reference target (Intel i5-8400);
expect comfortably higher frame rates there. The reference target for
a "smooth viewport" is 60 fps — met at 100k triangles with headroom.

## What the time goes to

The cost is dominated by pixel fill (the grid overdraws heavily at
these sizes): edge tests, depth compare and the color write per
pixel. Triangle counts alone matter less than covered area — the
50k→100k step costs less than doubling because the grid cells shrink
and many triangles fail the bbox test cheaply or cover fewer pixels.

## Optimizations applied (this session)

- **Flat-shading fast path** in `hal_raster_triangle`: when the three
  corner intensities are equal (the default flat shading — one lambert
  value per face), the packed color is computed once and the inner
  loop reduces to edge stepping, a depth compare and the write.
- **Incremental edge functions**: the barycentric edge functions are
  affine in (x, y), so each pixel costs three adds per function
  instead of a recomputed cross product (six multiplies).
- Together: ~15% faster at 100k triangles, ~25% at 10k, with the
  barycentric-binding regression test guarding correctness.

## Deliberately NOT done (honest)

- **Multithreading**: the numbers above are single-threaded and clear
  the 60 fps bar with margin on the reference target; a tiled
  rasterizer with per-tile depth buffers is the natural next step if
  a real workload (heavy texture sampling, antialiasing) needs it.
  Deferred to keep the rasterizer simple and debuggable.
- **SIMD**: the edge stepping is scalar; SSE/AVX batches of 4-8
  pixels per iteration are the same deferred work.
- **GPU rasterization**: Halcyon's platform layer (FDK) does not
  currently expose a GPU surface or context; the request is recorded
  in `docs/FDK-REQUESTS.md` (GPU viewport path). Until then the CPU
  rasterizer is the only backend.

## Reproducing

```sh
source /home/z/my-project/scripts/hal-env.sh
make bench
./build/bench
```
