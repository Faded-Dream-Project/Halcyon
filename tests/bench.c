/*
 * bench.c — viewport render benchmark: frame time at 10k / 50k /
 * 100k triangles (1280x720, solid shading, grid on)
 *
 * Build: make bench. Run: ./build/bench. Reports wall-clock ms per
 * hal_render_view call (min / median / max over RUNS iterations).
 */

#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/scene.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "render/raster.h"
#include "render/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 1280
#define H 720
#define RUNS 24

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* A grid mesh with exactly `tris` triangles (grid quads = 2 tris). */
static hal_mesh *grid_mesh(size_t tris) {
    /* cells = tris / 2; side x side >= cells with side = ceil(sqrt). */
    size_t cells = tris / 2;
    size_t side = 1;
    while (side * side < cells)
        side++;
    hal_mesh *m = hal_mesh_create();
    if (!m)
        return NULL;
    hal_f32 step = 2.0f / (hal_f32)side;
    for (size_t j = 0; j <= side; j++)
        for (size_t i = 0; i <= side; i++)
            hal_mesh_vert_add(
                m, hal_vec3f_make(-1.0f + (hal_f32)i * step, 0.0f,
                                  -1.0f + (hal_f32)j * step));
    for (size_t j = 0; j < side; j++)
        for (size_t i = 0; i < side; i++) {
            hal_u32 a = (hal_u32)(j * (side + 1) + i);
            hal_u32 b = (hal_u32)(j * (side + 1) + i + 1);
            hal_u32 c = (hal_u32)((j + 1) * (side + 1) + i + 1);
            hal_u32 d = (hal_u32)((j + 1) * (side + 1) + i);
            hal_u32 t1[3] = { a, d, c };
            hal_u32 t2[3] = { a, c, b };
            hal_mesh_face_add(m, t1, 3);
            hal_mesh_face_add(m, t2, 3);
        }
    hal_mesh_rebuild_edges(m);
    return m;
}

static void bench(size_t tris) {
    hal_scene *s = hal_scene_create("Bench");
    hal_object_id id = hal_scene_object_add(s, "Grid", HAL_OBJECT_MESH);
    hal_mesh *m = grid_mesh(tris);
    hal_object_set_mesh(s, id, m);
    /* Tilt the grid so the camera sees it at an angle (the common
     * viewport situation; flat-shaded by default). */
    hal_object_set_rotation_euler(s, id,
                                  hal_vec3f_make(0.5f, 0.3f, 0.0f));
    hal_object_set_scale(s, id, hal_vec3f_make(3.0f, 3.0f, 3.0f));

    hal_surface *surf = NULL;
    if (!hal_ok(hal_surface_offscreen_create(W, H, &surf))) {
        printf("%6zu tris: surface creation failed\n", tris);
        hal_scene_destroy(s);
        return;
    }
    hal_canvas canvas;
    hal_canvas_init(&canvas, surf, 1.0f);
    hal_camera cam = hal_camera_default();
    cam.distance = 6.0f;
    hal_render_opts opts = hal_render_opts_default();
    opts.draw_gizmo = false;

    hal_raster raster = { 0 };
    hal_rectf vp = { 0, 0, (hal_f32)W, (hal_f32)H };
    /* Warm-up (first frame allocates caches). */
    hal_render_view(&raster, s, &cam, &canvas, vp, &opts);

    double times[RUNS];
    for (int i = 0; i < RUNS; i++) {
        double t0 = now_s();
        hal_render_view(&raster, s, &cam, &canvas, vp, &opts);
        times[i] = (now_s() - t0) * 1000.0;
    }
    qsort(times, RUNS, sizeof(double), cmp_double);
    printf("%6zu tris: min %6.2f ms  median %6.2f ms  max %6.2f ms"
           "  (~%.0f fps at median)\n",
           tris, times[0], times[RUNS / 2], times[RUNS - 1],
           1000.0 / times[RUNS / 2]);

    hal_raster_end(&raster);
    hal_surface_destroy(surf);
    hal_scene_destroy(s);
}

int main(void) {
    printf("Halcyon viewport benchmark — %dx%d, solid shading, grid"
           " on, %d runs each\n\n",
           W, H, RUNS);
    bench(10000);
    bench(50000);
    bench(100000);
    return 0;
}
