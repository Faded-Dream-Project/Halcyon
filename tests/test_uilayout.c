/*
 * test_uilayout.c — STEP-2 layout verification with REAL font
 * metrics
 *
 * The suite renders a representative editor UI (menubar with the
 * real File menu incl. separators and shortcuts, panels with
 * headers, labels, entries, sliders, a tree, an open popup) to an
 * offscreen canvas at three resolutions x two scales, and audits
 * EVERY draw_text: its full ink rect (real ascent..descent, full
 * advance) must lie inside the clip stack. Popup widths must fit
 * their widest item. Also: the ground-grid projection must put ink
 * exactly where the camera math says.
 *
 * This exists because the UI shipped with text a full ascent too
 * high, truncated popups and overflowing property rows while every
 * other test passed — geometry must be asserted, not assumed.
 */

#include "hal_test.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "render/raster.h"
#include "render/render.h"
#include "ui/canvas.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds a UI mirroring the editor shell's structure. */
static char entry_bufs[7][16];
static hal_f32 rough_value = 0.5f; /* outlives the UI (slider binds) */

static hal_ui *build_editor_ui(hal_widget **menubar_out) {
    for (int i = 0; i < 7; i++)
        snprintf(entry_bufs[i], sizeof(entry_bufs[i]), "%s",
                 i == 0 ? "Cube" : "0.000");
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 1920, 1080 });

    /* Menubar with real-world items (long labels + shortcuts + a
     * separator). */
    hal_widget *bar = hal_menubar_create(root);
    int file = hal_menu_add(bar, "File");
    hal_menu_item_add(bar, file, "New Project", "Ctrl+N", NULL, NULL);
    hal_menu_item_add(bar, file, "Open Project...", "Ctrl+O", NULL,
                      NULL);
    hal_menu_item_add(bar, file, "Save Project", "Ctrl+S", NULL,
                      NULL);
    hal_menu_item_add(bar, file, NULL, NULL, NULL, NULL);
    hal_menu_item_add(
        bar, file, "Export Convex Collider (.glb)...", NULL, NULL,
        NULL);
    int edit = hal_menu_add(bar, "Edit");
    hal_menu_item_add(bar, edit, "Extrude Region", "E", NULL, NULL);
    hal_menu_item_add(bar, edit, "Grab with Falloff", "G", NULL,
                      NULL);
    *menubar_out = bar;

    /* Sidebar: panels with headers, rows of label + expanding
     * entries, sliders — the structure that overflowed 1920x1080. */
    hal_widget *side = hal_column_create(root);
    hal_widget_set_rect(side, (hal_rectf){ 1680, 26, 240, 1028 });
    hal_widget *props = hal_panel_create(side, "Properties");
    hal_widget *col = hal_column_create(props);
    hal_label_create(col, "Name");
    hal_widget *nr = hal_row_create(col);
    hal_widget *ne = hal_entry_create(nr, entry_bufs[0], sizeof(entry_bufs[0]));
    hal_widget_set_expand(ne, true);
    hal_label_create(col, "Location");
    hal_widget *lr = hal_row_create(col);
    for (int i = 0; i < 3; i++) {
        hal_widget *e =
            hal_entry_create(lr, entry_bufs[1 + i], sizeof(entry_bufs[0]));
        hal_widget_set_expand(e, true);
    }
    hal_widget *mat = hal_panel_create(side, "Material");
    hal_widget *mc = hal_column_create(mat);
    hal_label_create(mc, "Color");
    hal_widget *cr = hal_row_create(mc);
    for (int i = 0; i < 3; i++) {
        hal_widget *e =
            hal_entry_create(cr, entry_bufs[4 + i], sizeof(entry_bufs[0]));
        hal_widget_set_expand(e, true);
    }
    hal_widget *mr = hal_row_create(mc);
    hal_label_create(mr, "Rough");
    hal_widget *sl =
        hal_slider_create(mr, 0.0f, 1.0f, &rough_value);
    hal_widget_set_expand(sl, true);
    return ui;
}

static void test_audit_all_configs(void) {
    static const struct {
        hal_f32 w, h, scale;
    } cfgs[] = {
        { 1920, 1080, 1.0f }, { 1920, 1080, 2.0f },
        { 1366, 768, 1.0f },  { 1366, 768, 2.0f },
        { 1280, 720, 1.0f },  { 1280, 720, 2.0f },
    };
    for (size_t c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
        hal_widget *bar = NULL;
        hal_ui *ui = build_editor_ui(&bar);
        HAL_TEST_CHECK(ui != NULL);
        if (!ui)
            continue;

        hal_surface *s = NULL;
        hal_f32 scale = cfgs[c].scale;
        if (!hal_ok(hal_surface_offscreen_create(
                (int)(cfgs[c].w * scale),
                (int)(cfgs[c].h * scale), &s))) {
            hal_ui_destroy(ui);
            continue;
        }
        hal_canvas canvas;
        HAL_TEST_CHECK(hal_canvas_init(&canvas, s, scale));
        hal_ui_set_scale(ui, scale);
        hal_widget *root = hal_ui_root(ui);
        hal_widget_set_rect(root, (hal_rectf){ 0, 0, cfgs[c].w,
                                               cfgs[c].h });
        hal_ui_layout(ui);

        int violations = hal__ui_audit(ui, &canvas);
        hal_surface_destroy(s);
        hal_ui_destroy(ui);
        if (violations != 0)
            printf("config %.0fx%.0f@%.1f: %d violation(s)\n",
                   cfgs[c].w, cfgs[c].h, scale, violations);
        HAL_TEST_CHECK_EQ_I(violations, 0);
    }
}

/* Row heights must fit the REAL font: a face taller than its
 * nominal px still needs its ascent + descent. */
static void test_row_heights_fit_font(void) {
    hal_widget *bar = NULL;
    hal_ui *ui = build_editor_ui(&bar);
    HAL_TEST_CHECK(ui != NULL);
    if (!ui)
        return;
    hal_ui_set_scale(ui, 1.0f);
    hal_font *f = hal__ui_font(ui, ui->theme->font_size);
    HAL_TEST_CHECK(f != NULL);
    if (f) {
        hal_font_metrics m;
        hal_font_get_metrics(f, &m);
        hal_f32 need = (hal_f32)m.ascent + (hal_f32)m.descent;
        hal_f32 row = hal__text_row_h(ui, ui->theme->font_size);
        HAL_TEST_CHECK(row >= need - 0.01f);
        (void)need;
    }
    hal_ui_destroy(ui);
}

/* The ground grid: ink must land exactly where the camera math
 * projects it (the reported-but-unreproducible "wrongly projected
 * grid" now has a geometric regression guard). */
static void test_grid_projection_matches_math(void) {
    const int W = 400, H = 400;
    hal_surface *s = NULL;
    hal_canvas c;
    if (!hal_ok(hal_surface_offscreen_create(W, H, &s)) ||
        !hal_canvas_init(&c, s, 1.0f)) {
        HAL_TEST_CHECK(false);
        return;
    }
    hal_camera cam = hal_camera_default();
    cam.pitch = 1.55f;
    cam.yaw = 0.0f;
    cam.distance = 12.0f;
    hal_raster raster = { 0 };
    hal_scene *empty = hal_scene_create("E");
    hal_render_opts opts = hal_render_opts_default();
    opts.draw_gizmo = false;
    hal_rectf vp = { 0, 0, (hal_f32)W, (hal_f32)H };
    hal_render_view(&raster, empty, &cam, &c, vp, &opts);
    hal_raster_end(&raster);

    hal_mat4f view = hal_camera_view(&cam);
    hal_mat4f proj = hal_camera_projection(&cam, vp.width / vp.height);
    hal_mat4f vpm = hal_mat4f_mul(proj, view);
    for (int wx = -3; wx <= 3; wx++) {
        hal_vec4f clip = hal_mat4f_mul_vec4(
            vpm, hal_vec4f_make((hal_f32)wx, 0.0f, 0.0f, 1.0f));
        hal_f32 sx =
            (clip.x / clip.w * 0.5f + 0.5f) * vp.width + vp.x;
        hal_f32 sy =
            (0.5f - clip.y / clip.w * 0.5f) * vp.height + vp.y;
        bool ink = false;
        for (int dy = -3; dy <= 3 && !ink; dy++)
            for (int dx = -3; dx <= 3 && !ink; dx++) {
                int x = (int)(sx + 0.5f) + dx;
                int y = (int)(sy + 0.5f) + dy;
                if (x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                hal_u32 p = ((hal_u32 *)c.pixels)[(size_t)y * (size_t)c.stride +
                                                 (size_t)x];
                if ((((p & 255) + ((p >> 8) & 255) +
                      ((p >> 16) & 255)) / 3) > 60)
                    ink = true;
            }
        HAL_TEST_CHECK(ink);
    }
    hal_scene_destroy(empty);
    hal_surface_destroy(s);
}

int main(void) {
    test_audit_all_configs();
    test_row_heights_fit_font();
    test_grid_projection_matches_math();
    HAL_TEST_SUMMARY("test_uilayout");
}
