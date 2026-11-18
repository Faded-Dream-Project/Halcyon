/*
 * shell.c — the Halcyon editor shell: UI assembly, scene wiring,
 * menus, shortcuts, project and autosave driving
 */

#include "shell.h"
#include "core/mesh.h"
#include "core/project.h"
#include "render/raster.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout metrics (logical px). */
#define SHELL_MENUBAR_H 30.0f
#define SHELL_STATUSBAR_H 26.0f
#define SHELL_TOOLBAR_W 42.0f
#define SHELL_SIDEBAR_W 240.0f
#define SHELL_VIEWPORT_MIN 100.0f

struct hal_app {
    hal_platform *platform;
    hal_window *window;
    hal_ui *ui;
    hal_scene *scene;
    hal_undo_stack *undo;
    hal_autosave *autosave;
    bool running;
    bool dirty;              /* unsaved changes                      */
    char project_path[512];  /* "" = untitled                        */

    /* Widgets the shell talks to. */
    hal_widget *outliner;
    hal_widget *status;
    hal_widget *name_entry;
    hal_widget *loc_entry[3];
    hal_widget *sidebar_panel;
    hal_widget *menubar;

    /* Scene -> outliner rebuild guard (selection sync reentrancy). */
    bool syncing;

    /* Entry buffers (owned here, pointed at by the entries). */
    char name_buf[64];
    char loc_buf[3][32];

    /* The 3D viewport. */
    hal_camera camera;
    hal_raster raster;
    hal_render_opts render_opts;
    bool wireframe;          /* bound to the tool-rail toggle      */
    bool viewport_drag;      /* an orbit/pan drag is active        */
    bool drag_is_pan;        /* pan vs orbit (decided at press)    */
    hal_pointf drag_last;
    hal_pointf last_pointer;
};

/* ---- Undo command: an object transform edit ---- */

typedef struct transform_cmd {
    hal_object_id id;
    hal_vec3f old_location;
    hal_vec3f new_location;
} transform_cmd;

static void transform_apply(void *state, void *ctx) {
    transform_cmd *c = state;
    hal_object_set_location(ctx, c->id, c->new_location);
}

static void transform_revert(void *state, void *ctx) {
    transform_cmd *c = state;
    hal_object_set_location(ctx, c->id, c->old_location);
}

static void transform_free(void *state) {
    free(state);
}

/* Pushes a location change through the undo stack and refreshes the
 * properties panel. The caller has already written the new value. */
static void push_location_undo(hal_app *app, hal_object_id id,
                               hal_vec3f old_location) {
    hal_object *obj = hal_scene_object(app->scene, id);
    if (!obj)
        return;
    transform_cmd *cmd = malloc(sizeof(*cmd));
    if (!cmd)
        return;
    cmd->id = id;
    cmd->old_location = old_location;
    cmd->new_location = obj->location;
    char label[64];
    snprintf(label, sizeof(label), "Move %s", obj->name);
    hal_undo_stack_push(app->undo, label, transform_apply,
                        transform_revert, transform_free, cmd);
}

/* ---- Outliner <-> scene sync ---- */

static void rebuild_outliner(hal_app *app) {
    app->syncing = true;
    hal_tree_clear(app->outliner);

    /* Objects with a parent nest under the parent's row. */
    size_t count = hal_scene_object_count(app->scene);
    hal_tree_row **rows = calloc(count ? count : 1, sizeof(*rows));
    if (!rows) {
        app->syncing = false;
        return;
    }

    /* Two passes: roots first, then children (parents precede their
     * children in the scene array — the project format guarantees it,
     * and the scene's own add order does too). */
    hal_object *obj;
    for (size_t i = 0; i < count; i++) {
        obj = hal_scene_object_at(app->scene, i);
        if (obj->parent == HAL_OBJECT_ID_NONE)
            rows[i] = hal_tree_add(app->outliner, NULL, obj->name,
                                   (void *)(uintptr_t)obj->id);
    }
    for (size_t i = 0; i < count; i++) {
        obj = hal_scene_object_at(app->scene, i);
        if (obj->parent == HAL_OBJECT_ID_NONE)
            continue;
        hal_tree_row *parent_row = NULL;
        for (size_t j = 0; j < count; j++)
            if (hal_scene_object_at(app->scene, j)->id == obj->parent) {
                parent_row = rows[j];
                break;
            }
        rows[i] = hal_tree_add(app->outliner, parent_row, obj->name,
                               (void *)(uintptr_t)obj->id);
    }
    free(rows);
    app->syncing = false;
}

/* ---- Property panel ---- */

static void refresh_properties(hal_app *app) {
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj) {
        app->name_buf[0] = '\0';
        for (int i = 0; i < 3; i++)
            app->loc_buf[i][0] = '\0';
        hal_widget_set_visible(app->sidebar_panel, true);
    } else {
        snprintf(app->name_buf, sizeof(app->name_buf), "%s", obj->name);
        snprintf(app->loc_buf[0], sizeof(app->loc_buf[0]), "%.3f",
                 (double)obj->location.x);
        snprintf(app->loc_buf[1], sizeof(app->loc_buf[1]), "%.3f",
                 (double)obj->location.y);
        snprintf(app->loc_buf[2], sizeof(app->loc_buf[2]), "%.3f",
                 (double)obj->location.z);
    }
    hal_ui_invalidate(app->ui);
}

static void on_name_changed(hal_widget *entry, void *user) {
    hal_app *app = user;
    (void)entry;
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || app->syncing)
        return;
    /* Rename through the scene (name is not undoable yet — undo of
     * name edits lands with M4's edit-mode commands). */
    hal_object_set_name(app->scene, obj->id, app->name_buf);
    app->dirty = true;
    hal_autosave_mark_dirty(app->autosave);
    rebuild_outliner(app);
}

static void on_loc_changed(hal_widget *entry, void *user) {
    hal_app *app = user;
    int axis = -1;
    for (int i = 0; i < 3; i++)
        if (entry == app->loc_entry[i])
            axis = i;
    if (axis < 0)
        return;

    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || app->syncing)
        return;

    hal_vec3f old = obj->location;
    char *end = NULL;
    double v = strtod(app->loc_buf[axis], &end);
    if (!end || end == app->loc_buf[axis])
        return; /* not a number yet; the user is still typing */

    hal_vec3f new_loc = old;
    if (axis == 0)
        new_loc.x = (hal_f32)v;
    else if (axis == 1)
        new_loc.y = (hal_f32)v;
    else
        new_loc.z = (hal_f32)v;

    hal_object_set_location(app->scene, obj->id, new_loc);
    push_location_undo(app, obj->id, old);
    app->dirty = true;
    hal_autosave_mark_dirty(app->autosave);
}

static void on_outliner_select(hal_widget *tree, hal_tree_row *row,
                               void *user) {
    hal_app *app = user;
    (void)tree;
    if (app->syncing || !row)
        return;
    hal_object_id id = (hal_object_id)(uintptr_t)hal_tree_row_user(row);
    hal_scene_select_only(app->scene, id);
    refresh_properties(app);
}

/* ---- Menus ---- */

static void menu_action(hal_widget *menubar, int menu, int item,
                        void *user);

static void build_menus(hal_app *app) {
    hal_widget *bar = hal_menubar_create(hal_ui_root(app->ui));
    app->menubar = bar;

    int file = hal_menu_add(bar, "File");
    hal_menu_item_add(bar, file, "New", "Ctrl+N", menu_action, app);
    hal_menu_item_add(bar, file, "Open...", "Ctrl+O", menu_action, app);
    hal_menu_item_add(bar, file, "Save", "Ctrl+S", menu_action, app);
    hal_menu_item_add(bar, file, "Save As...", NULL, menu_action, app);
    hal_menu_item_add(bar, file, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, file, "Quit", "Ctrl+Q", menu_action, app);

    int edit = hal_menu_add(bar, "Edit");
    hal_menu_item_add(bar, edit, "Undo", "Ctrl+Z", menu_action, app);
    hal_menu_item_add(bar, edit, "Redo", "Ctrl+Shift+Z", menu_action, app);

    int add = hal_menu_add(bar, "Add");
    hal_menu_item_add(bar, add, "Cube", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Light", NULL, menu_action, app);
}

/* ---- Project actions ---- */

static void set_status(hal_app *app, const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    hal_widget_set_text(app->status, text);
}

static void scene_new(hal_app *app) {
    hal_scene *fresh = hal_scene_create("Scene");
    if (!fresh)
        return;
    hal_scene_destroy(app->scene);
    app->scene = fresh;
    hal_undo_stack_clear(app->undo);
    app->project_path[0] = '\0';
    app->dirty = false;
    rebuild_outliner(app);
    refresh_properties(app);
    set_status(app, "New project");
}

static void dialog_open_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Open cancelled");
        return;
    }
    char err[256] = { 0 };
    hal_scene *loaded = NULL;
    if (hal_ok(hal_project_load(path, &loaded, err, sizeof(err)))) {
        hal_scene_destroy(app->scene);
        app->scene = loaded;
        hal_undo_stack_clear(app->undo);
        snprintf(app->project_path, sizeof(app->project_path), "%s", path);
        app->dirty = false;
        rebuild_outliner(app);
        refresh_properties(app);
        set_status(app, "Opened %s", path);
    } else {
        set_status(app, "Open failed: %s", err);
        hal_dialog_message(app->platform, app->window, "Halcyon",
                           err[0] ? err : "could not open the project");
    }
    free(path);
}

static void dialog_save_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Save cancelled");
        return;
    }
    /* Ensure a .halcyon extension unless the user typed one. */
    if (!strstr(path, ".halcyon")) {
        size_t len = strlen(path);
        if (len + 8 < sizeof(app->project_path)) {
            memcpy(app->project_path, path, len);
            memcpy(app->project_path + len, ".halcyon", 9);
            free(path);
            path = app->project_path;
        }
    } else {
        snprintf(app->project_path, sizeof(app->project_path), "%s", path);
        free(path);
    }

    char err[256] = { 0 };
    if (hal_ok(hal_project_save(app->scene, app->project_path, err,
                                sizeof(err)))) {
        app->dirty = false;
        hal_autosave_mark_clean(app->autosave);
        set_status(app, "Saved %s", app->project_path);
    } else {
        set_status(app, "Save failed: %s", err);
    }
}

static void save_project(hal_app *app) {
    if (app->project_path[0]) {
        char err[256] = { 0 };
        if (hal_ok(hal_project_save(app->scene, app->project_path, err,
                                    sizeof(err)))) {
            app->dirty = false;
            hal_autosave_mark_clean(app->autosave);
            set_status(app, "Saved %s", app->project_path);
        } else {
            set_status(app, "Save failed: %s", err);
        }
    } else {
        hal_dialog_save_file(app->platform, "Save project", "*.halcyon",
                             "untitled.halcyon", dialog_save_done, app,
                             app->window);
    }
}

static void add_object(hal_app *app, const char *name, hal_object_type type,
                       hal_vec3f location) {
    hal_object_id id = hal_scene_object_add(app->scene, name, type);
    if (id == HAL_OBJECT_ID_NONE)
        return;
    if (type == HAL_OBJECT_MESH) {
        hal_mesh *mesh = NULL;
        if (strcmp(name, "Sphere") == 0)
            mesh = hal_mesh_sphere(1.0f, 16, 24);
        else if (strcmp(name, "Plane") == 0)
            mesh = hal_mesh_plane(1.0f);
        else
            mesh = hal_mesh_cube(0.5f);
        hal_object_set_mesh(app->scene, id, mesh);
    }
    hal_object_set_location(app->scene, id, location);
    hal_scene_select_only(app->scene, id);
    app->dirty = true;
    hal_autosave_mark_dirty(app->autosave);
    rebuild_outliner(app);
    refresh_properties(app);
    set_status(app, "Added %s", name);
}

static void menu_action(hal_widget *menubar, int menu, int item,
                        void *user) {
    hal_app *app = user;
    (void)menubar;

    if (menu == 0) { /* File */
        switch (item) {
        case 0:
            scene_new(app);
            break;
        case 1:
            hal_dialog_open_file(app->platform, "Open project",
                                 "*.halcyon", dialog_open_done, app,
                                 app->window);
            break;
        case 2:
            save_project(app);
            break;
        case 3:
            hal_dialog_save_file(app->platform, "Save project as",
                                 "*.halcyon", "untitled.halcyon",
                                 dialog_save_done, app, app->window);
            break;
        case 5:
            hal_app_quit(app);
            break;
        default:
            break;
        }
    } else if (menu == 1) { /* Edit */
        if (item == 0) {
            hal_undo_stack_undo(app->undo);
            refresh_properties(app);
            set_status(app, "Undo");
        } else if (item == 1) {
            hal_undo_stack_redo(app->undo);
            refresh_properties(app);
            set_status(app, "Redo");
        }
    } else if (menu == 2) { /* Add */
        if (item == 0)
            add_object(app, "Cube", HAL_OBJECT_MESH,
                       hal_vec3f_make(0.0f, 0.0f, 0.0f));
        else if (item == 1)
            add_object(app, "Light", HAL_OBJECT_LIGHT,
                       hal_vec3f_make(2.0f, 3.0f, 2.0f));
    }
}

/* ---- Lifecycle ---- */

hal_app *hal_app_create(hal_platform *platform, hal_window *window) {
    hal_app *app = calloc(1, sizeof(*app));
    if (!app)
        return NULL;
    app->platform = platform;
    app->window = window;
    app->running = true;

    app->scene = hal_scene_create("Scene");
    app->undo = hal_undo_stack_create(app->scene, 128);
    app->ui = hal_ui_create();
    if (!app->scene || !app->undo || !app->ui) {
        hal_app_destroy(app);
        return NULL;
    }
    app->autosave = hal_autosave_create(NULL, 120); /* path set on save */
    if (!app->autosave) {
        hal_app_destroy(app);
        return NULL;
    }

    app->camera = hal_camera_default();
    app->render_opts = hal_render_opts_default();
    app->raster.depth = NULL;
    app->raster.w = 0;
    app->raster.h = 0;

    build_menus(app);

    /* Tool rail (left edge): shading mode for now; the M4 tools join
     * it as real operators land. */
    hal_widget *rail = hal_column_create(hal_ui_root(app->ui));
    hal_toggle_create(rail, "Wire", &app->wireframe);

    /* Right sidebar: outliner over properties; the outliner takes the
     * leftover height (expand), properties hugs its content. */
    hal_widget *sidebar = hal_column_create(hal_ui_root(app->ui));
    app->sidebar_panel = sidebar;
    hal_widget *outliner_panel = hal_panel_create(sidebar, "Outliner");
    hal_widget_set_expand(outliner_panel, true);
    app->outliner = hal_tree_create(outliner_panel);
    hal_tree_set_on_select(app->outliner, on_outliner_select, app);

    hal_widget *props_panel = hal_panel_create(sidebar, "Properties");
    hal_widget *props_col = hal_column_create(props_panel);
    hal_widget *name_row = hal_row_create(props_col);
    hal_label_create(name_row, "Name");
    app->name_entry = hal_entry_create(name_row, app->name_buf,
                                       sizeof(app->name_buf));
    hal_entry_set_on_change(app->name_entry, on_name_changed, app);

    hal_widget *loc_row = hal_row_create(props_col);
    hal_label_create(loc_row, "Location");
    for (int i = 0; i < 3; i++) {
        app->loc_entry[i] = hal_entry_create(loc_row, app->loc_buf[i],
                                             sizeof(app->loc_buf[i]));
        hal_entry_set_on_change(app->loc_entry[i], on_loc_changed, app);
    }

    /* Status bar. */
    app->status = hal_label_create(hal_ui_root(app->ui), "Welcome");

    /* A starting cube: something to look at from the first frame. */
    add_object(app, "Cube", HAL_OBJECT_MESH, hal_vec3f_zero());
    rebuild_outliner(app);
    refresh_properties(app);
    return app;
}

void hal_app_destroy(hal_app *app) {
    if (!app)
        return;
    hal_raster_end(&app->raster);
    /* The UI dies first (entries point into app buffers). */
    hal_ui_destroy(app->ui);
    hal_autosave_destroy(app->autosave);
    hal_undo_stack_destroy(app->undo);
    hal_scene_destroy(app->scene);
    free(app);
}

bool hal_app_running(const hal_app *app) {
    return app ? app->running : false;
}

void hal_app_quit(hal_app *app) {
    if (app)
        app->running = false;
}

bool hal_app_handle_event(hal_app *app, const hal_event *event) {
    if (!app)
        return false;

    if (hal_ui_handle_event(app->ui, event))
        return true;

    /* Viewport camera controls: events the UI did not consume that
     * land inside the viewport rect. MMB or Alt+LMB orbits,
     * Shift+MMB pans, the wheel zooms. */
    hal_rectf vp = hal_app_viewport_rect(app);
    bool in_vp = false;
    hal_f32 px = 0.0f, py = 0.0f;
    if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN ||
        event->type == HAL_EVENT_POINTER_BUTTON_UP ||
        event->type == HAL_EVENT_POINTER_MOTION ||
        event->type == HAL_EVENT_POINTER_SCROLL) {
        if (event->type == HAL_EVENT_POINTER_SCROLL) {
            px = event->scroll.position.x;
            py = event->scroll.position.y;
        } else if (event->type == HAL_EVENT_POINTER_MOTION) {
            px = event->pointer.position.x;
            py = event->pointer.position.y;
        } else {
            px = event->pointer_button.position.x;
            py = event->pointer_button.position.y;
        }
        app->last_pointer = (hal_pointf){ px, py };
        in_vp = px >= vp.x && py >= vp.y &&
                px < vp.x + vp.width && py < vp.y + vp.height;
    }

    if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN && in_vp) {
        hal_u32 mods = event->pointer_button.modifiers;
        bool orbit = event->pointer_button.button == HAL_BUTTON_MIDDLE ||
                     (event->pointer_button.button == HAL_BUTTON_LEFT &&
                      (mods & HAL_MOD_ALT) != 0);
        bool pan = event->pointer_button.button == HAL_BUTTON_MIDDLE &&
                   (mods & HAL_MOD_SHIFT) != 0;
        if (orbit || pan) {
            app->viewport_drag = true;
            app->drag_is_pan = pan;
            app->drag_last = (hal_pointf){ px, py };
            return true;
        }
    }
    if (event->type == HAL_EVENT_POINTER_BUTTON_UP && app->viewport_drag) {
        app->viewport_drag = false;
        return true;
    }
    if (event->type == HAL_EVENT_POINTER_MOTION && app->viewport_drag) {
        hal_f32 dx = px - app->drag_last.x;
        hal_f32 dy = py - app->drag_last.y;
        app->drag_last = (hal_pointf){ px, py };
        if (app->drag_is_pan) {
            /* Screen-space pan scaled by the distance: content moves
             * about with the pointer at any zoom level. */
            hal_f32 k = app->camera.distance * 0.0016f;
            hal_camera_pan(&app->camera, dx * k, -dy * k);
        } else {
            hal_camera_orbit(&app->camera, dx * 0.008f, dy * 0.008f);
        }
        return true;
    }
    if (event->type == HAL_EVENT_POINTER_SCROLL && in_vp) {
        hal_f32 f = event->scroll.delta_y > 0.0f ? 1.1f : 0.9f;
        hal_camera_zoom(&app->camera, f);
        return true;
    }

    /* App-level keys (the UI did not consume them). */
    if (event->type == HAL_EVENT_KEY_DOWN) {
        hal_u32 mod = event->key.modifiers;
        bool ctrl = (mod & HAL_MOD_CTRL) != 0;
        hal_scancode key = event->key.scancode;

        /* Keys are matched by letter position: N/O/S/Q/Z/Y. */
        if (ctrl && key == 17) { /* N */
            scene_new(app);
            return true;
        }
        if (ctrl && key == 24) { /* O */
            hal_dialog_open_file(app->platform, "Open project",
                                 "*.halcyon", dialog_open_done, app,
                                 app->window);
            return true;
        }
        if (ctrl && key == 31) { /* S */
            save_project(app);
            return true;
        }
        if (ctrl && key == 30) { /* Q */
            hal_app_quit(app);
            return true;
        }
        if (ctrl && key == 44) { /* Z */
            if (mod & HAL_MOD_SHIFT) {
                hal_undo_stack_redo(app->undo);
                set_status(app, "Redo");
            } else {
                hal_undo_stack_undo(app->undo);
                set_status(app, "Undo");
            }
            refresh_properties(app);
            return true;
        }
        if (ctrl && key == 21) { /* Y */
            hal_undo_stack_redo(app->undo);
            set_status(app, "Redo");
            refresh_properties(app);
            return true;
        }
    }
    return false;
}

/* ---- Layout + paint ---- */

static void layout_shell(hal_app *app, hal_f32 w, hal_f32 h) {
    hal_widget *root = hal_ui_root(app->ui);
    hal_widget_set_rect(root, (hal_rectf){ 0, 0, w, h });

    hal_sizef mbar = hal_widget_measured(app->menubar);
    hal_widget_set_rect(app->menubar,
                        (hal_rectf){ 0, 0, w, mbar.height });

    hal_widget *sidebar = app->sidebar_panel;
    hal_f32 sidebar_w = SHELL_SIDEBAR_W;
    hal_f32 sidebar_x = w - sidebar_w;
    if (sidebar_x < SHELL_TOOLBAR_W + SHELL_VIEWPORT_MIN)
        sidebar_x = w * 0.5f;
    hal_f32 body_h = h - mbar.height - SHELL_STATUSBAR_H;
    hal_widget_set_rect(sidebar,
                        (hal_rectf){ sidebar_x, mbar.height, w - sidebar_x,
                                    body_h });

    hal_widget *status = app->status;
    hal_sizef status_m = hal_widget_measured(status);
    hal_widget_set_rect(
        status, (hal_rectf){ 8, h - SHELL_STATUSBAR_H, status_m.width,
                             SHELL_STATUSBAR_H });

    hal_ui_layout(app->ui);
}

hal_rectf hal_app_viewport_rect(const hal_app *app) {
    if (!app)
        return (hal_rectf){ 0, 0, 0, 0 };
    hal_sizef mbar = hal_widget_measured(app->menubar);
    hal_f32 w = hal_widget_rect(hal_ui_root(app->ui)).width;
    hal_f32 h = hal_widget_rect(hal_ui_root(app->ui)).height;
    hal_f32 sidebar_w = hal_widget_rect(app->sidebar_panel).width;
    hal_f32 vw = w - SHELL_TOOLBAR_W - sidebar_w;
    if (vw < SHELL_VIEWPORT_MIN)
        vw = SHELL_VIEWPORT_MIN;
    return (hal_rectf){ SHELL_TOOLBAR_W, mbar.height, vw,
                        h - mbar.height - SHELL_STATUSBAR_H };
}

static void paint_viewport(hal_app *app, hal_canvas *canvas) {
    const hal_theme *t = hal_ui_theme(app->ui);
    hal_rectf vp = hal_app_viewport_rect(app);

    /* Theme-driven render options; the wireframe toggle rides along. */
    hal_render_opts opts = app->render_opts;
    opts.bg = t->colors[HAL_THEME_VIEWPORT_BG];
    opts.grid = t->colors[HAL_THEME_VIEWPORT_GRID];
    opts.object = hal_color_rgb8(0xb9, 0xb2, 0xe0);
    opts.shading = app->wireframe ? HAL_SHADING_WIREFRAME
                                  : HAL_SHADING_SOLID;

    hal_render_view(&app->raster, app->scene, &app->camera, canvas, vp,
                    &opts);
}

void hal_app_paint(hal_app *app, hal_surface *surface) {
    if (!app || !surface)
        return;

    hal_size win_size;
    hal_f32 scale = 1.0f;
    if (!hal_ok(hal_window_get_size(app->window, &win_size)))
        return;
    hal_window_get_scale(app->window, &scale);

    hal_ui_set_scale(app->ui, scale);
    layout_shell(app, (hal_f32)win_size.width, (hal_f32)win_size.height);

    hal_canvas canvas;
    if (!hal_canvas_init(&canvas, surface, scale))
        return;

    hal_ui_paint(app->ui, &canvas);

    /* The 3D view renders into its rect over the UI's empty middle. */
    paint_viewport(app, &canvas);

    hal_surface_invalidate_all(surface);
    hal_surface_present(surface);
}

/* ---- Accessors ---- */

hal_scene *hal_app_scene(hal_app *app) {
    return app ? app->scene : NULL;
}

hal_camera *hal_app_camera(hal_app *app) {
    return app ? &app->camera : NULL;
}

const hal_render_opts *hal_app_render_opts(const hal_app *app) {
    return app ? &app->render_opts : NULL;
}

hal_ui *hal_app_ui(hal_app *app) {
    return app ? app->ui : NULL;
}

hal_undo_stack *hal_app_undo(hal_app *app) {
    return app ? app->undo : NULL;
}

hal_autosave *hal_app_autosave(hal_app *app) {
    return app ? app->autosave : NULL;
}

void hal_app_refresh_outliner(hal_app *app) {
    if (app)
        rebuild_outliner(app);
}

const char *hal_app_project_path(const hal_app *app) {
    return app ? app->project_path : "";
}

bool hal_app_dirty(const hal_app *app) {
    return app ? app->dirty : false;
}
