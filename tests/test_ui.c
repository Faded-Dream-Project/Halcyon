/*
 * test_ui.c — headless UI tests: canvas primitives, theme, layout
 * engine, and input routing (synthetic events, no display)
 */

#include "hal_test.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Canvas ---- */

static void test_canvas_fill_and_clip(void) {
    hal_surface *surf = NULL;
    HAL_TEST_CHECK_EQ_I(hal_surface_offscreen_create(100, 80, &surf),
                        HAL_OK);
    hal_canvas canvas;
    HAL_TEST_CHECK(hal_canvas_init(&canvas, surf, 1.0f));

    HAL_TEST_CHECK_EQ_F(hal_canvas_size(&canvas).width, 100.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(hal_canvas_size(&canvas).height, 80.0f, 1e-5);

    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 100, 80 },
                         hal_color_hex(0x000000u));
    /* Fill a red square; verify the center pixel and outside. */
    hal_canvas_fill_rect(&canvas, (hal_rectf){ 10, 10, 20, 20 },
                         hal_color_hex(0xFF0000u));
    hal_surface_invalidate_all(surf);

    hal_surface_info info;
    hal_surface_info_get(surf, &info);
    HAL_TEST_CHECK_EQ_I(info.pixels[15 * info.stride + 15] & 0xFFFFFFu,
                        0xFF0000u);
    HAL_TEST_CHECK_EQ_I(info.pixels[5 * info.stride + 5] & 0xFFFFFFu, 0);

    /* Clip: nothing paints outside the pushed rect. */
    hal_canvas_push_clip(&canvas, (hal_rectf){ 0, 0, 10, 10 });
    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 100, 80 },
                         hal_color_hex(0x00FF00u));
    hal_canvas_pop_clip(&canvas);
    hal_surface_invalidate_all(surf);
    hal_surface_info_get(surf, &info);
    HAL_TEST_CHECK_EQ_I(info.pixels[50 * info.stride + 50] & 0xFFFFFFu,
                        0); /* outside clip: untouched */
    HAL_TEST_CHECK_EQ_I(info.pixels[5 * info.stride + 5] & 0xFFFFFFu,
                        0x00FF00u); /* inside clip: painted */

    /* Nested clips intersect (never expand). */
    hal_canvas_push_clip(&canvas, (hal_rectf){ 0, 0, 50, 50 });
    HAL_TEST_CHECK(hal_canvas_push_clip(&canvas, (hal_rectf){ 25, 25, 50, 50 }));
    hal_rectf eff = hal_canvas_clip(&canvas);
    HAL_TEST_CHECK_EQ_F(eff.x, 25.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(eff.y, 25.0f, 1e-5);
    HAL_TEST_CHECK_EQ_F(eff.width, 25.0f, 1e-5);
    hal_canvas_pop_clip(&canvas);
    hal_canvas_pop_clip(&canvas);

    /* Scale: a logical rect at scale 2 covers the doubled physical
     * area. */
    hal_canvas canvas2;
    hal_surface *surf2 = NULL;
    hal_surface_offscreen_create(200, 200, &surf2);
    hal_canvas_init(&canvas2, surf2, 2.0f);
    HAL_TEST_CHECK_EQ_F(hal_canvas_size(&canvas2).width, 100.0f, 1e-5);
    hal_canvas_fill_rect(&canvas2, (hal_rectf){ 10, 10, 10, 10 },
                         hal_color_hex(0x0000FFu));
    hal_surface_invalidate_all(surf2);
    hal_surface_info_get(surf2, &info);
    /* Logical 10..20 at scale 2 = physical 20..40. */
    HAL_TEST_CHECK_EQ_I(info.pixels[30 * info.stride + 30] & 0xFFFFFFu,
                        0x0000FFu);
    HAL_TEST_CHECK_EQ_I(info.pixels[10 * info.stride + 10] & 0xFFFFFFu, 0);
    HAL_TEST_CHECK_EQ_I(info.pixels[45 * info.stride + 45] & 0xFFFFFFu, 0);

    hal_surface_destroy(surf);
    hal_surface_destroy(surf2);
}

static void test_canvas_aa_shapes(void) {
    hal_surface *surf = NULL;
    hal_surface_offscreen_create(64, 64, &surf);
    hal_canvas canvas;
    hal_canvas_init(&canvas, surf, 1.0f);

    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 64, 64 },
                         hal_color_hex(0x000000u));
    /* A filled circle: center covered, far corner not, boundary
     * blended (not pure black, not pure white). */
    hal_canvas_fill_circle(&canvas, 32.5f, 32.0f, 20.3f,
                           hal_color_hex(0xFFFFFFu));
    hal_surface_invalidate_all(surf);

    hal_surface_info info;
    hal_surface_info_get(surf, &info);
    HAL_TEST_CHECK_EQ_I(info.pixels[32 * info.stride + 32] & 0xFFu, 0xFFu);
    HAL_TEST_CHECK_EQ_I(info.pixels[2 * info.stride + 2] & 0xFFu, 0x00u);
    /* Pixel 52's center (52.5) is 0.3 inside the boundary: partial. */
    hal_u32 edge = info.pixels[32 * info.stride + 52] & 0xFFu;
    HAL_TEST_CHECK(edge > 10 && edge < 245);

    /* A triangle covers its centroid. */
    hal_canvas_fill_rect(&canvas, (hal_rectf){ 0, 0, 64, 64 },
                         hal_color_hex(0x000000u));
    hal_canvas_fill_triangle(&canvas,
                             (hal_pointf){ 32.0f, 8.0f },
                             (hal_pointf){ 8.0f, 56.0f },
                             (hal_pointf){ 56.0f, 56.0f },
                             hal_color_hex(0x00FF00u));
    hal_surface_invalidate_all(surf);
    hal_surface_info_get(surf, &info);
    HAL_TEST_CHECK_EQ_I(info.pixels[40 * info.stride + 32] & 0xFF00u, 0xFF00u);
    HAL_TEST_CHECK_EQ_I(info.pixels[4 * info.stride + 4] & 0xFF00u, 0);

    hal_surface_destroy(surf);
}

/* ---- Theme ---- */

static const char *ui_tmp_dir(void) {
    static char dir[256];
    static bool made = false;
    if (!made) {
        snprintf(dir, sizeof(dir), "/tmp/halcyon-ui-test-%d", (int)getpid());
        mkdir(dir, 0755);
        made = true;
    }
    return dir;
}

static void test_theme(void) {
    hal_theme *t = hal_theme_default();
    HAL_TEST_CHECK(t != NULL);
    if (!t)
        return;
    HAL_TEST_CHECK(strcmp(t->name, "Faded Dream") == 0);
    /* The dreamy look: window is a deep blue, accent a soft violet. */
    HAL_TEST_CHECK_EQ_F(t->colors[HAL_THEME_WINDOW].r, 0x14 / 255.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(t->colors[HAL_THEME_WINDOW].b, 0x3c / 255.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(t->colors[HAL_THEME_ACCENT].r, 0x8b / 255.0f, 1e-4);

    /* Save + load round trip through a file. */
    char path[256];
    snprintf(path, sizeof(path), "%s/theme.haltheme", ui_tmp_dir());
    HAL_TEST_CHECK(hal_theme_save(t, path));
    char err[256] = { 0 };
    hal_theme *loaded = hal_theme_load(path, err, sizeof(err));
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK(strcmp(loaded->name, "Faded Dream") == 0);
        for (int i = 0; i < HAL_THEME__COLOR_COUNT; i++) {
            HAL_TEST_CHECK_EQ_F(loaded->colors[i].r, t->colors[i].r, 1e-3);
            HAL_TEST_CHECK_EQ_F(loaded->colors[i].g, t->colors[i].g, 1e-3);
            HAL_TEST_CHECK_EQ_F(loaded->colors[i].b, t->colors[i].b, 1e-3);
            HAL_TEST_CHECK_EQ_F(loaded->colors[i].a, t->colors[i].a, 1e-3);
        }
        HAL_TEST_CHECK_EQ_F(loaded->radius, t->radius, 1e-5);
        HAL_TEST_CHECK_EQ_I(loaded->font_size, t->font_size);
        hal_theme_destroy(loaded);
    }

    /* Strict parser: an unknown token refuses with a line number. */
    snprintf(path, sizeof(path), "%s/bad.haltheme", ui_tmp_dir());
    FILE *f = fopen(path, "wb");
    fputs("halcyon-theme 1\nname \"X\"\ncolor sparkle #ffffff\n", f);
    fclose(f);
    loaded = hal_theme_load(path, err, sizeof(err));
    HAL_TEST_CHECK(loaded == NULL);
    HAL_TEST_CHECK(strstr(err, "line 3") != NULL);

    /* A theme overriding one color inherits the rest from default. */
    snprintf(path, sizeof(path), "%s/partial.haltheme", ui_tmp_dir());
    f = fopen(path, "wb");
    fputs("halcyon-theme 1\nname \"Darker\"\ncolor window #000000\n",
          f);
    fclose(f);
    loaded = hal_theme_load(path, err, sizeof(err));
    HAL_TEST_CHECK(loaded != NULL);
    if (loaded) {
        HAL_TEST_CHECK_EQ_F(loaded->colors[HAL_THEME_WINDOW].r, 0.0f, 1e-4);
        HAL_TEST_CHECK_EQ_F(loaded->colors[HAL_THEME_ACCENT].r,
                            t->colors[HAL_THEME_ACCENT].r, 1e-4);
        hal_theme_destroy(loaded);
    }

    hal_theme_destroy(t);
}

/* ---- Layout ---- */

static void test_layout_row_column(void) {
    hal_ui *ui = hal_ui_create();
    HAL_TEST_CHECK(ui != NULL);
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });

    hal_widget *row = hal_row_create(root);
    hal_widget_set_rect(row, (hal_rectf){ 0, 0, 400, 40 });
    hal_widget *a = hal_label_create(row, "AAAA");
    hal_widget *b = hal_label_create(row, "BBBB");
    hal_widget *c = hal_label_create(row, "CCCC");

    hal_ui_layout(ui);

    /* Children sit side by side in measured order, no overlap. */
    hal_rectf ar = hal_widget_abs_rect(a);
    hal_rectf br = hal_widget_abs_rect(b);
    hal_rectf cr = hal_widget_abs_rect(c);
    HAL_TEST_CHECK(ar.x < br.x);
    HAL_TEST_CHECK(br.x < cr.x);
    HAL_TEST_CHECK(ar.x + ar.width <= br.x + 1.0f);

    /* Expand: one expanding child takes the leftover width. */
    hal_widget *row2 = hal_row_create(root);
    hal_widget_set_rect(row2, (hal_rectf){ 0, 50, 400, 40 });
    hal_widget *fixed = hal_label_create(row2, "X");
    hal_widget *stretch = hal_label_create(row2, "");
    hal_widget_set_expand(stretch, true);
    hal_ui_layout(ui);
    HAL_TEST_CHECK_EQ_F(hal_widget_abs_rect(stretch).width,
                        400.0f - hal_widget_measured(fixed).width -
                            hal_ui_theme(ui)->gap -
                            hal_ui_theme(ui)->padding,
                        2.0f);

    /* A column stacks vertically. */
    hal_widget *col = hal_column_create(root);
    hal_widget_set_rect(col, (hal_rectf){ 0, 100, 200, 200 });
    hal_widget *t1 = hal_label_create(col, "One");
    hal_widget *t2 = hal_label_create(col, "Two");
    hal_ui_layout(ui);
    HAL_TEST_CHECK(hal_widget_abs_rect(t1).y < hal_widget_abs_rect(t2).y);
    HAL_TEST_CHECK_EQ_F(hal_widget_abs_rect(t1).x,
                        hal_widget_abs_rect(t2).x, 1e-4f);

    hal_ui_destroy(ui);
}

static void test_layout_panel(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });

    hal_widget *panel = hal_panel_create(root, "Outliner");
    hal_widget_set_rect(panel, (hal_rectf){ 10, 10, 200, 280 });
    hal_widget *inner = hal_column_create(panel);

    hal_ui_layout(ui);

    /* The panel is bigger than its content (title + padding). */
    HAL_TEST_CHECK(hal_widget_measured(panel).width >
                   hal_widget_measured(inner).width);
    HAL_TEST_CHECK(hal_widget_measured(panel).height >
                   hal_widget_measured(inner).height);
    /* The client child sits inside the panel's padding. */
    hal_rectf pr = hal_widget_abs_rect(panel);
    hal_rectf ir = hal_widget_abs_rect(inner);
    HAL_TEST_CHECK(ir.x >= pr.x);
    HAL_TEST_CHECK(ir.y > pr.y); /* title band */
    HAL_TEST_CHECK(ir.x + ir.width <= pr.x + pr.width);

    hal_ui_destroy(ui);
}

/* ---- Input routing ---- */

static int g_clicks = 0;
static void on_click_count(hal_widget *w, void *user) {
    (void)w;
    (void)user;
    g_clicks++;
}

static int g_menu_activated_item = -1;
static void test_menu_activate(hal_widget *menubar, int menu, int item,
                               void *user) {
    (void)menubar;
    (void)menu;
    (void)user;
    g_menu_activated_item = item;
}

static bool fire_motion(hal_ui *ui, float x, float y) {
    hal_event e = { .type = HAL_EVENT_POINTER_MOTION };
    e.pointer.position = (hal_pointf){ x, y };
    return hal_ui_handle_event(ui, &e);
}

static bool fire_press(hal_ui *ui, float x, float y) {
    hal_event e = { .type = HAL_EVENT_POINTER_BUTTON_DOWN };
    e.pointer_button.position = (hal_pointf){ x, y };
    e.pointer_button.button = HAL_BUTTON_LEFT;
    return hal_ui_handle_event(ui, &e);
}

static bool fire_release(hal_ui *ui, float x, float y) {
    hal_event e = { .type = HAL_EVENT_POINTER_BUTTON_UP };
    e.pointer_button.position = (hal_pointf){ x, y };
    e.pointer_button.button = HAL_BUTTON_LEFT;
    return hal_ui_handle_event(ui, &e);
}

static void test_input_button(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });
    g_clicks = 0;

    hal_widget *btn = hal_button_create(root, "Save", on_click_count, NULL);
    hal_widget_set_rect(btn, (hal_rectf){ 10, 10, 80, 30 });
    hal_ui_layout(ui);

    /* The button's abs rect is where the shell placed it. */
    hal_rectf r = hal_widget_abs_rect(btn);
    HAL_TEST_CHECK_EQ_F(r.x, 10.0f, 1e-4);
    HAL_TEST_CHECK_EQ_F(r.y, 10.0f, 1e-4);

    float cx = r.x + r.width * 0.5f;
    float cy = r.y + r.height * 0.5f;

    /* Hover marks the widget. */
    HAL_TEST_CHECK(fire_motion(ui, cx, cy));
    HAL_TEST_CHECK(hal_widget_hovered(btn));

    /* Press + release inside fires the click. */
    HAL_TEST_CHECK(fire_press(ui, cx, cy));
    HAL_TEST_CHECK(fire_release(ui, cx, cy));
    HAL_TEST_CHECK_EQ_I(g_clicks, 1);

    /* Press inside, release outside: no click. */
    fire_press(ui, cx, cy);
    fire_release(ui, 390, 290);
    HAL_TEST_CHECK_EQ_I(g_clicks, 1);

    /* Events on empty space fall through to the application. */
    HAL_TEST_CHECK(!fire_press(ui, 390, 290));

    hal_ui_destroy(ui);
}

static void test_input_slider(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });

    hal_f32 value = 0.0f;
    hal_widget *slider = hal_slider_create(root, 0.0f, 1.0f, &value);
    hal_widget_set_rect(slider, (hal_rectf){ 0, 100, 200, 26 });
    hal_ui_layout(ui);

    /* Press at the middle sets ~0.5; the drag updates continuously
     * (the implicit grab: even motion outside the widget continues to
     * drive the slider). */
    HAL_TEST_CHECK(fire_press(ui, 100, 113));
    HAL_TEST_CHECK(value > 0.45f && value < 0.55f);

    HAL_TEST_CHECK(fire_motion(ui, 180, 113));
    HAL_TEST_CHECK(value > 0.85f);

    HAL_TEST_CHECK(fire_motion(ui, 5000, -20)); /* clamps, stays grabbed */
    HAL_TEST_CHECK_EQ_F(value, 1.0f, 1e-4);

    HAL_TEST_CHECK(fire_release(ui, 180, 113));
    /* After release, motion no longer drives the slider. */
    fire_motion(ui, 10, 113);
    HAL_TEST_CHECK_EQ_F(value, 1.0f, 1e-4);

    hal_ui_destroy(ui);
}

static void test_input_toggle_and_entry(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });

    bool checked = false;
    hal_widget *toggle = hal_toggle_create(root, "Smooth shading", &checked);
    hal_widget_set_rect(toggle, (hal_rectf){ 10, 10, 160, 24 });

    char buf[32] = "Cube";
    hal_widget *entry = hal_entry_create(root, buf, sizeof(buf));
    hal_widget_set_rect(entry, (hal_rectf){ 10, 44, 160, 26 });
    hal_ui_layout(ui);

    /* Toggle flips per click. */
    hal_rectf tr = hal_widget_abs_rect(toggle);
    float tcx = tr.x + 8, tcy = tr.y + tr.height * 0.5f;
    fire_press(ui, tcx, tcy);
    fire_release(ui, tcx, tcy);
    HAL_TEST_CHECK(checked);
    fire_press(ui, tcx, tcy);
    fire_release(ui, tcx, tcy);
    HAL_TEST_CHECK(!checked);

    /* Clicking the entry focuses it; typing appends. */
    hal_rectf er = hal_widget_abs_rect(entry);
    fire_press(ui, er.x + 5, er.y + 12);
    fire_release(ui, er.x + 5, er.y + 12);
    HAL_TEST_CHECK(hal_ui_focus(ui) == entry);

    hal_event k = { .type = HAL_EVENT_KEY_DOWN };
    k.key.codepoint = '!';
    HAL_TEST_CHECK(hal_ui_handle_event(ui, &k));
    HAL_TEST_CHECK(strcmp(buf, "Cube!") == 0);

    k.key.codepoint = 0;
    k.key.scancode = HAL_KEY_BACKSPACE;
    HAL_TEST_CHECK(hal_ui_handle_event(ui, &k));
    HAL_TEST_CHECK(strcmp(buf, "Cube") == 0);

    /* Tab moves focus; Escape releases it. */
    k.key.scancode = HAL_KEY_ESC;
    HAL_TEST_CHECK(hal_ui_handle_event(ui, &k));
    HAL_TEST_CHECK(hal_ui_focus(ui) == NULL);

    hal_ui_destroy(ui);
}

static void test_input_tree_and_menu(void) {
    hal_ui *ui = hal_ui_create();
    hal_widget *root = hal_ui_root(ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, 400, 300 });

    hal_widget *tree = hal_tree_create(root);
    hal_widget_set_rect(tree, (hal_rectf){ 0, 30, 200, 270 });
    hal_tree_row *parent = hal_tree_add(tree, NULL, "Sword", (void *)1);
    hal_tree_row *child = hal_tree_add(tree, parent, "Blade", (void *)2);
    hal_ui_layout(ui);
    hal_rectf parent_rect = hal_tree_row_rect(tree, parent);
    hal_rectf child_rect = hal_tree_row_rect(tree, child);
    HAL_TEST_CHECK(parent_rect.y < child_rect.y); /* visible: expanded */

    /* Click the parent row: selected. */
    fire_press(ui, 60, parent_rect.y + 5);
    fire_release(ui, 60, parent_rect.y + 5);
    HAL_TEST_CHECK(hal_tree_selected(tree) == parent);

    /* Click the expander triangle: collapse (children hide). */
    fire_press(ui, parent_rect.x + 10, parent_rect.y + 5);
    fire_release(ui, parent_rect.x + 10, parent_rect.y + 5);
    HAL_TEST_CHECK(!hal_tree_row_expanded(parent));

    /* Menu bar: open, hover, activate, close. */
    hal_widget *bar = hal_menubar_create(root);
    hal_widget_set_rect(bar, (hal_rectf){ 0, 0, 400, 28 });
    int file = hal_menu_add(bar, "File");
    HAL_TEST_CHECK(file >= 0);
    hal_menu_item_add(bar, file, "New", "Ctrl+N", test_menu_activate,
                      (void *)11);
    hal_menu_item_add(bar, file, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, file, "Quit", "Ctrl+Q", test_menu_activate,
                      (void *)99);
    g_menu_activated_item = -1;
    hal_ui_layout(ui);

    /* Click "File" opens the menu. */
    fire_press(ui, 20, 14);
    fire_release(ui, 20, 14);
    HAL_TEST_CHECK(hal_ui_menu_open(ui));

    /* Move onto the first item and click: the callback fires with the
     * right index; the menu closes. */
    hal_rectf item0 = hal_ui_menu_item_rect(ui, 0);
    fire_motion(ui, item0.x + 5, item0.y + 5);
    fire_press(ui, item0.x + 5, item0.y + 5);
    HAL_TEST_CHECK_EQ_I(g_menu_activated_item, 0);
    HAL_TEST_CHECK(!hal_ui_menu_open(ui));

    /* Escape closes without activating. */
    fire_press(ui, 20, 14);
    fire_release(ui, 20, 14);
    HAL_TEST_CHECK(hal_ui_menu_open(ui));
    hal_event esc = { .type = HAL_EVENT_KEY_DOWN };
    esc.key.scancode = HAL_KEY_ESC;
    HAL_TEST_CHECK(hal_ui_handle_event(ui, &esc));
    HAL_TEST_CHECK(!hal_ui_menu_open(ui));

    hal_ui_destroy(ui);
}

int main(void) {
    test_canvas_fill_and_clip();
    test_canvas_aa_shapes();
    test_theme();
    test_layout_row_column();
    test_layout_panel();
    test_input_button();
    test_input_slider();
    test_input_toggle_and_entry();
    test_input_tree_and_menu();
    HAL_TEST_SUMMARY("test_ui");
}
