/* gl_renderer_overlays.h — the GL renderer's overlay batch.
 *
 * Included BY gl_renderer.c (it reaches into the renderer struct and
 * the hal_gl table). Overlays (armature sticks, edit-mode selection,
 * gizmo, axis widget) arrive as SCREEN-SPACE lines/points computed
 * by the shell's existing projection code — one projection path,
 * two consumers (the CPU canvas in tests, the GL batch at runtime).
 * Depth is disabled for them, exactly like the CPU overlay pass.
 *
 * The grid is world-space (it must depth-test against the meshes),
 * so it lives here with its own emitter.
 */

#ifndef HAL_GL_RENDERER_OVERLAYS_H
#define HAL_GL_RENDERER_OVERLAYS_H

#include "camera.h"
#include "../core/scene.h"
#include <stdlib.h>
#include "gl_renderer.h"

static void ov_reset(hal_gl_renderer *r) {
    if (!r->ov) {
        r->ov_cap = 4096;
        r->ov = malloc(r->ov_cap * sizeof(hal_gl_ov_vert));
    }
    r->ov_len = 0;
}

static void ov_push(hal_gl_renderer *r, hal_gl_ov_vert v) {
    if (!r->ov && !r->ov_cap)
        ov_reset(r);
    if (!r->ov)
        return;
    if (r->ov_len == r->ov_cap) {
        size_t nc = r->ov_cap * 2;
        hal_gl_ov_vert *nb = realloc(r->ov, nc * sizeof(*nb));
        if (!nb)
            return;
        r->ov = nb;
        r->ov_cap = nc;
    }
    r->ov[r->ov_len++] = v;
}

static hal_gl_ov_vert ov_from_screen(hal_vec2f p, hal_rectf vp,
                                     hal_color c) {
    hal_gl_ov_vert v;
    float x = (p.x - vp.x) / (vp.width > 0 ? vp.width : 1);
    float y = (p.y - vp.y) / (vp.height > 0 ? vp.height : 1);
    v.x = x * 2.0f - 1.0f;
    v.y = 1.0f - y * 2.0f;
    v.r = c.r;
    v.g = c.g;
    v.b = c.b;
    v.a = c.a;
    return v;
}

static void ov_draw(hal_gl_renderer *r, hal_gl_enum mode) {
    if (!r->ov_len)
        return;
    hal_gl.bind_vertex_array(r->line_vao);
    hal_gl.bind_buffer(HAL_GL_ARRAY_BUFFER, r->line_vbo);
    hal_gl.buffer_sub_data(HAL_GL_ARRAY_BUFFER, 0,
                           (ptrdiff_t)(r->ov_len * sizeof(hal_gl_ov_vert)),
                           r->ov);
    hal_gl.draw_arrays(mode, 0, (hal_gl_int)r->ov_len);
    r->ov_len = 0;
}

/* The world-space XZ grid, depth-tested against the meshes. Runs
 * BEFORE the mesh pass (the CPU grid renders under the objects). */
static void hal_gl_grid_world(hal_gl_renderer *r, hal_mat4f vpm,
                              hal_color grid) {
    ov_reset(r);
    const float E = 10.0f;
    for (int i = -10; i <= 10; i++) {
        float f = (float)i;
        hal_vec3f p[4] = {
            { f, 0.0f, -E }, { f, 0.0f, E },
            { -E, 0.0f, f }, { E, 0.0f, f },
        };
        for (int pair = 0; pair < 2; pair++) {
            hal_vec4f c0 = hal_mat4f_mul_vec4(
                vpm, (hal_vec4f){ p[pair * 2].x, p[pair * 2].y,
                                  p[pair * 2].z, 1.0f });
            hal_vec4f c1 = hal_mat4f_mul_vec4(
                vpm, (hal_vec4f){ p[pair * 2 + 1].x, p[pair * 2 + 1].y,
                                  p[pair * 2 + 1].z, 1.0f });
            if (c0.w > 0.001f && c1.w > 0.001f) {
                hal_gl_ov_vert a = { c0.x / c0.w, c0.y / c0.w,
                                     grid.r, grid.g, grid.b, 0.55f };
                hal_gl_ov_vert b = { c1.x / c1.w, c1.y / c1.w,
                                     grid.r, grid.g, grid.b, 0.55f };
                ov_push(r, a);
                ov_push(r, b);
            }
        }
    }
    hal_gl.use_program(r->line_prog);
    hal_gl.uniform_1f(r->u_line_psize, 1.0f);
    hal_gl.enable(HAL_GL_BLEND);
    hal_gl.blend_func(HAL_GL_SRC_ALPHA, HAL_GL_ONE_MINUS_SRC_ALPHA);
    ov_draw(r, HAL_GL_LINES);
    hal_gl.disable(HAL_GL_BLEND);
}

#endif /* HAL_GL_RENDERER_OVERLAYS_H */
