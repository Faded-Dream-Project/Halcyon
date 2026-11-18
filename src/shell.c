/*
 * shell.c — the Halcyon editor shell: UI assembly, scene wiring,
 * menus, shortcuts, project and autosave driving
 */

#include "shell.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/project.h"
#include "render/pick.h"
#include "render/raster.h"

#include <math.h>
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

    /* Edit mode (Tab): the object being edited and the active
     * selection mode (0 vert, 1 edge, 2 face). */
    bool edit_mode;
    hal_object_id edit_obj;
    int sel_mode;

    /* The move gizmo: axis under drag (-1 = none). */
    int gizmo_axis;
    hal_pointf gizmo_last;
    hal_vec3f gizmo_orig_loc;

    /* Modal rotate/scale: drag delta drives the transform until
     * click confirms (undo pushed) or Escape cancels (restored). */
    int modal_op; /* 0 none, 1 rotate, 2 scale */
    hal_vec3f modal_orig;      /* scale at start            */
    hal_vec3f modal_orig_loc;  /* location at start         */
    hal_quatf modal_orig_rot;  /* rotation at start         */
    hal_f32 modal_amount;
    hal_pointf modal_last;
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

static void set_status(hal_app *app, const char *fmt, ...);

/* ---- Object transform undo (rotate/scale confirm) ---- */

typedef struct trs_cmd {
    hal_object_id id;
    hal_vec3f old_loc, new_loc;
    hal_quatf old_rot, new_rot;
    hal_vec3f old_scale, new_scale;
} trs_cmd;

static void trs_apply(void *state, void *ctx) {
    trs_cmd *c = state;
    hal_object_set_location(ctx, c->id, c->new_loc);
    hal_object_set_rotation(ctx, c->id, c->new_rot);
    hal_object_set_scale(ctx, c->id, c->new_scale);
}

static void trs_revert(void *state, void *ctx) {
    trs_cmd *c = state;
    hal_object_set_location(ctx, c->id, c->old_loc);
    hal_object_set_rotation(ctx, c->id, c->old_rot);
    hal_object_set_scale(ctx, c->id, c->old_scale);
}

static void trs_free(void *state) {
    free(state);
}

/* ---- Mesh operations (undoable: snapshot + swap) ---- */

typedef struct mesh_cmd {
    hal_object_id id;
    hal_mesh *snapshot; /* pre-op mesh; swap target for undo/redo */
} mesh_cmd;

static void mesh_swap(hal_scene *scene, hal_object_id id,
                      hal_mesh **with) {
    hal_object *o = hal_scene_object(scene, id);
    if (!o)
        return;
    hal_mesh *tmp = o->mesh;
    o->mesh = *with;
    *with = tmp;
}

static void mesh_cmd_apply(void *state, void *ctx) {
    mesh_cmd *c = state;
    mesh_swap(ctx, c->id, &c->snapshot);
}

static void mesh_cmd_revert(void *state, void *ctx) {
    mesh_cmd *c = state;
    mesh_swap(ctx, c->id, &c->snapshot);
}

static void mesh_cmd_free(void *state) {
    mesh_cmd *c = state;
    hal_mesh_destroy(c->snapshot);
    free(c);
}

/* Runs a mesh operator on the edited object with a snapshot pushed
 * onto the undo stack. op returns true when the mesh changed. */
static void apply_mesh_op(hal_app *app, const char *label,
                          bool (*op)(hal_mesh *m, void *user), void *user) {
    hal_object *obj = hal_scene_object(app->scene, app->edit_obj);
    if (!obj || !obj->mesh)
        return;

    hal_mesh *snap = hal_mesh_clone(obj->mesh);
    if (!snap)
        return;

    bool changed = op(obj->mesh, user);
    if (!changed) {
        hal_mesh_destroy(snap);
        set_status(app, "%s: nothing to do", label);
        return;
    }

    mesh_cmd *cmd = malloc(sizeof(*cmd));
    if (!cmd) {
        /* Roll back to keep state consistent. */
        hal_mesh *tmp = obj->mesh;
        obj->mesh = snap;
        hal_mesh_destroy(tmp);
        return;
    }
    cmd->id = app->edit_obj;
    cmd->snapshot = snap;
    hal_undo_stack_push(app->undo, label, mesh_cmd_apply,
                        mesh_cmd_revert, mesh_cmd_free, cmd);
    app->dirty = true;
    hal_autosave_mark_dirty(app->autosave);
    set_status(app, "%s: %zu verts, %zu faces", label,
               obj->mesh->vert_count, obj->mesh->face_count);
}

/* Thin adapters so every operator fits apply_mesh_op's signature. */
static bool op_extrude(hal_mesh *m, void *user) {
    (void)user;
    return hal_mesh_extrude_faces(m, 0.5f);
}
static bool op_inset(hal_mesh *m, void *user) {
    (void)user;
    return hal_mesh_inset_faces(m, 0.25f);
}
static bool op_bevel(hal_mesh *m, void *user) {
    (void)user;
    return hal_mesh_bevel_edges(m, 0.1f);
}
static bool op_subdivide(hal_mesh *m, void *user) {
    (void)user;
    return hal_mesh_subdivide(m);
}
static bool op_merge(hal_mesh *m, void *user) {
    (void)user;
    return hal_mesh_merge_selected_verts(m);
}
typedef struct op_delete_user {
    hal_delete_mode mode;
} op_delete_user;
static bool op_delete(hal_mesh *m, void *user) {
    op_delete_user *u = user;
    return hal_mesh_delete_selected(m, u->mode);
}
typedef struct op_loop_user {
    hal_u32 edge;
    hal_f32 t;
} op_loop_user;
static bool op_loop_cut(hal_mesh *m, void *user) {
    op_loop_user *u = user;
    return hal_mesh_loop_cut(m, u->edge, u->t);
}
typedef struct op_mirror_user {
    hal_mirror_axis axis;
} op_mirror_user;
static bool op_mirror(hal_mesh *m, void *user) {
    op_mirror_user *u = user;
    return hal_mesh_mirror(m, u->axis);
}
static bool op_flip(hal_mesh *m, void *user) {
    (void)user;
    hal_mesh_flip_winding(m);
    hal_mesh_rebuild_edges(m);
    return true;
}
static bool op_recalc(hal_mesh *m, void *user) {
    (void)user;
    hal_mesh_recalc_normals(m);
    return true;
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
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Extrude", "E", menu_action, app);
    hal_menu_item_add(bar, edit, "Inset", "I", menu_action, app);
    hal_menu_item_add(bar, edit, "Bevel Edge", "B", menu_action, app);
    hal_menu_item_add(bar, edit, "Loop Cut", "Ctrl+R", menu_action, app);
    hal_menu_item_add(bar, edit, "Subdivide", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Merge Verts", "M", menu_action, app);
    hal_menu_item_add(bar, edit, "Delete", "X", menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Mirror X", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Mirror Y", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Mirror Z", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Flip Normals", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Recalc Normals", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Smooth Shading", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Select All", "A", menu_action, app);
    hal_menu_item_add(bar, edit, "Deselect All", "Alt+A", menu_action, app);

    int add = hal_menu_add(bar, "Add");
    hal_menu_item_add(bar, add, "Cube", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Sphere", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Plane", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Cylinder", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Cone", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Torus", NULL, menu_action, app);
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
        else if (strcmp(name, "Cylinder") == 0)
            mesh = hal_mesh_cylinder(0.5f, 2.0f, 16);
        else if (strcmp(name, "Cone") == 0)
            mesh = hal_mesh_cone(0.5f, 1.0f, 16);
        else if (strcmp(name, "Torus") == 0)
            mesh = hal_mesh_torus(1.0f, 0.35f, 16, 8);
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

/* The Add menu's mesh-producing names, in menu order. */
static const char *ADD_NAMES[6] = { "Cube", "Sphere", "Plane",
                                    "Cylinder", "Cone", "Torus" };

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
        switch (item) {
        case 0:
            hal_undo_stack_undo(app->undo);
            refresh_properties(app);
            set_status(app, "Undo");
            break;
        case 1:
            hal_undo_stack_redo(app->undo);
            refresh_properties(app);
            set_status(app, "Redo");
            break;
        case 3:
            apply_mesh_op(app, "Extrude", op_extrude, NULL);
            break;
        case 4:
            apply_mesh_op(app, "Inset", op_inset, NULL);
            break;
        case 5:
            apply_mesh_op(app, "Bevel", op_bevel, NULL);
            break;
        case 6: {
            /* Loop cut through the edge under the cursor (or the
             * selected edge when the pointer is elsewhere). */
            hal_object *obj = hal_scene_object(app->scene, app->edit_obj);
            if (!obj || !obj->mesh)
                break;
            hal_rectf vp = hal_app_viewport_rect(app);
            hal_size ws;
            if (!hal_ok(hal_window_get_size(app->window, &ws)))
                break;
            hal_mat4f view = hal_camera_view(&app->camera);
            hal_mat4f proj = hal_camera_projection(
                &app->camera, (hal_f32)ws.width / (hal_f32)ws.height);
            hal_mat4f vpm = hal_mat4f_mul(proj, view);
            hal_u32 edge = 0;
            bool have = false;
            if (app->last_pointer.x >= vp.x &&
                app->last_pointer.y >= vp.y) {
                int e = hal_pick_edge(
                    obj->mesh, hal_object_world_matrix(app->scene, obj),
                    &app->camera, vpm, vp, app->last_pointer.x,
                    app->last_pointer.y, 10.0f);
                if (e >= 0) {
                    edge = (hal_u32)e;
                    have = true;
                }
            }
            if (!have && obj->mesh->edge_sel)
                for (size_t e = 0; e < obj->mesh->edge_count; e++)
                    if (obj->mesh->edge_sel[e]) {
                        edge = (hal_u32)e;
                        have = true;
                        break;
                    }
            if (have) {
                static op_loop_user lu;
                lu.edge = edge;
                lu.t = 0.5f;
                apply_mesh_op(app, "Loop Cut", op_loop_cut, &lu);
            } else {
                set_status(app, "Loop Cut: hover an edge first");
            }
            break;
        }
        case 7:
            apply_mesh_op(app, "Subdivide", op_subdivide, NULL);
            break;
        case 8:
            apply_mesh_op(app, "Merge", op_merge, NULL);
            break;
        case 9: {
            static op_delete_user du;
            du.mode = app->sel_mode == 0 ? HAL_DELETE_VERTS
                                         : HAL_DELETE_FACES;
            apply_mesh_op(app, "Delete", op_delete, &du);
            break;
        }
        case 11: {
            static op_mirror_user mu = { HAL_MIRROR_X };
            apply_mesh_op(app, "Mirror X", op_mirror, &mu);
            break;
        }
        case 12: {
            static op_mirror_user mu = { HAL_MIRROR_Y };
            apply_mesh_op(app, "Mirror Y", op_mirror, &mu);
            break;
        }
        case 13: {
            static op_mirror_user mu = { HAL_MIRROR_Z };
            apply_mesh_op(app, "Mirror Z", op_mirror, &mu);
            break;
        }
        case 15:
            apply_mesh_op(app, "Flip Normals", op_flip, NULL);
            break;
        case 16:
            apply_mesh_op(app, "Recalc Normals", op_recalc, NULL);
            break;
        case 17: {
            hal_object *obj =
                hal_scene_object(app->scene, hal_scene_selected(app->scene));
            if (obj) {
                hal_object_set_smooth_shading(app->scene, obj->id,
                                              !obj->smooth_shading);
                set_status(app, "Smooth shading %s",
                           obj->smooth_shading ? "on" : "off");
            }
            break;
        }
        case 19: {
            hal_object *obj =
                hal_scene_object(app->scene, app->edit_obj);
            if (obj && obj->mesh)
                hal_mesh_select_all(obj->mesh, true);
            break;
        }
        case 20: {
            hal_object *obj =
                hal_scene_object(app->scene, app->edit_obj);
            if (obj && obj->mesh)
                hal_mesh_select_all(obj->mesh, false);
            break;
        }
        default:
            break;
        }
    } else if (menu == 2) { /* Add */
        if (item <= 5)
            add_object(app, ADD_NAMES[item],
                       HAL_OBJECT_MESH, hal_vec3f_zero());
        else if (item == 6)
            add_object(app, "Light", HAL_OBJECT_LIGHT,
                       hal_vec3f_make(2.0f, 3.0f, 2.0f));
    }
}

/* The Add menu's mesh-producing names, in menu order. */
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

/* ---- Viewport helpers: gizmo hit-testing, edit clicks ---- */

/* Distance from a point to a screen segment. */
static hal_f32 seg_dist(hal_vec2f a, hal_vec2f b, hal_f32 px, hal_f32 py) {
    hal_vec2f ab = { b.x - a.x, b.y - a.y };
    hal_f32 len2 = ab.x * ab.x + ab.y * ab.y;
    hal_f32 t = 0.0f;
    if (len2 > 1e-9f) {
        t = ((px - a.x) * ab.x + (py - a.y) * ab.y) / len2;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
    }
    hal_f32 cx = a.x + ab.x * t;
    hal_f32 cy = a.y + ab.y * t;
    hal_f32 dx = cx - px;
    hal_f32 dy = cy - py;
    return (hal_f32)__builtin_sqrt((double)(dx * dx + dy * dy));
}

/* Which gizmo axis (0..2, or -1) is under the pixel? */
static int gizmo_axis_at(hal_app *app, hal_object *obj, hal_mat4f vpm,
                         hal_rectf vp, hal_f32 px, hal_f32 py) {
    hal_mat4f world = hal_object_world_matrix(app->scene, obj);
    hal_vec3f origin =
        hal_mat4f_transform_point(world, hal_vec3f_zero());
    hal_f32 w;
    hal_vec3f s0 =
        hal_camera_project(&app->camera, vpm, origin, vp, &w);
    if (w <= 0.0f)
        return -1;

    static const hal_vec3f axes[3] = {
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
    };
    int best = -1;
    hal_f32 best_d = 8.0f;
    for (int i = 0; i < 3; i++) {
        hal_vec3f s1 = hal_camera_project(
            &app->camera, vpm,
            hal_vec3f_add(origin, hal_vec3f_mul(axes[i], 1.0f)), vp, &w);
        if (w <= 0.0f)
            continue;
        hal_vec2f a = { s0.x, s0.y };
        hal_vec2f b = { s1.x, s1.y };
        hal_f32 d = seg_dist(a, b, px, py);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* A click in the viewport while in edit mode: pick and select. */
static void edit_click(hal_app *app, hal_mat4f vpm, hal_rectf vp,
                       hal_f32 px, hal_f32 py, bool extend) {
    hal_object *obj = hal_scene_object(app->scene, app->edit_obj);
    if (!obj || !obj->mesh) {
        set_status(app, "Edit: object has no mesh");
        return;
    }
    hal_mat4f world = hal_object_world_matrix(app->scene, obj);
    int hit = -1;
    if (app->sel_mode == 0)
        hit = hal_pick_vert(obj->mesh, world, &app->camera, vpm, vp, px,
                            py, 10.0f);
    else if (app->sel_mode == 1)
        hit = hal_pick_edge(obj->mesh, world, &app->camera, vpm, vp, px,
                            py, 8.0f);
    else
        hit = hal_pick_face(obj->mesh, world,
                            hal_camera_ray(&app->camera, vpm, vp, px, py),
                            NULL);

    if (!extend)
        hal_mesh_select_all(obj->mesh, false);
    if (hit < 0) {
        set_status(app, "Edit: nothing under the click");
        return;
    }
    if (app->sel_mode == 0)
        obj->mesh->vert_sel[hit] = !obj->mesh->vert_sel[hit] || !extend;
    else if (app->sel_mode == 1)
        obj->mesh->edge_sel[hit] = !obj->mesh->edge_sel[hit] || !extend;
    else
        obj->mesh->face_sel[hit] = !obj->mesh->face_sel[hit] || !extend;
    set_status(app, "Edit: %s %d selected",
               app->sel_mode == 0   ? "vert"
               : app->sel_mode == 1 ? "edge"
                                    : "face",
               hit);
}

bool hal_app_handle_event(hal_app *app, const hal_event *event) {
    if (!app)
        return false;

    if (hal_ui_handle_event(app->ui, event))
        return true;

    /* Viewport interaction: events the UI did not consume that land
     * inside the viewport rect. Order: modal transform > gizmo drag >
     * camera drag > click selection. */
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

    hal_size ws;
    hal_window_get_size(app->window, &ws);
    hal_mat4f view = hal_camera_view(&app->camera);
    hal_mat4f proj = hal_camera_projection(
        &app->camera, (hal_f32)ws.width / (hal_f32)ws.height);
    hal_mat4f vpm = hal_mat4f_mul(proj, view);

    /* --- Modal rotate/scale: drag drives, click confirms --- */
    if (app->modal_op != 0) {
        hal_object *obj =
            hal_scene_object(app->scene, hal_scene_selected(app->scene));
        if (!obj) {
            app->modal_op = 0;
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_MOTION) {
            hal_f32 dx = px - app->drag_last.x;
            app->drag_last = (hal_pointf){ px, py };
            app->modal_amount += dx;
            if (app->modal_op == 1) /* rotate about world Y */
                hal_object_set_rotation(
                    app->scene, obj->id,
                    hal_quatf_mul(hal_quatf_from_axis_angle(
                                      hal_vec3f_axis_y(),
                                      app->modal_amount * 0.01f),
                                  obj->rotation));
            else /* uniform scale */
                hal_object_set_scale(
                    app->scene, obj->id,
                    hal_vec3f_mul(app->modal_orig,
                                  (hal_f32)exp(app->modal_amount *
                                               0.01)));
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN &&
            event->pointer_button.button == HAL_BUTTON_LEFT) {
            /* Confirm: push the old/new transform. */
            trs_cmd *cmd = malloc(sizeof(*cmd));
            if (cmd) {
                cmd->id = obj->id;
                cmd->old_loc = app->modal_orig_loc;
                cmd->old_rot = app->modal_orig_rot;
                cmd->old_scale = app->modal_orig;
                cmd->new_loc = obj->location;
                cmd->new_rot = obj->rotation;
                cmd->new_scale = obj->scale;
                hal_undo_stack_push(app->undo, app->modal_op == 1
                                                    ? "Rotate"
                                                    : "Scale",
                                    trs_apply, trs_revert, trs_free,
                                    cmd);
            }
            app->modal_op = 0;
            app->dirty = true;
            return true;
        }
        if (event->type == HAL_EVENT_KEY_DOWN &&
            event->key.scancode == HAL_KEY_ESC) {
            hal_object_set_location(app->scene, obj->id,
                                    app->modal_orig_loc);
            hal_object_set_rotation(app->scene, obj->id,
                                    app->modal_orig_rot);
            hal_object_set_scale(app->scene, obj->id, app->modal_orig);
            app->modal_op = 0;
            return true;
        }
        return true; /* the modal op owns every event */
    }

    /* --- Move gizmo drag (object mode) --- */
    if (app->gizmo_axis >= 0) {
        if (event->type == HAL_EVENT_POINTER_MOTION) {
            hal_object *obj = hal_scene_object(
                app->scene, hal_scene_selected(app->scene));
            if (obj) {
                hal_f32 dx = px - app->gizmo_last.x;
                hal_f32 dy = py - app->gizmo_last.y;
                app->gizmo_last = (hal_pointf){ px, py };
                hal_vec3f axis =
                    app->gizmo_axis == 0
                        ? hal_vec3f_axis_x()
                        : app->gizmo_axis == 1 ? hal_vec3f_axis_y()
                                               : hal_vec3f_axis_z();
                /* Screen direction of the axis at the object. */
                hal_mat4f world =
                    hal_object_world_matrix(app->scene, obj);
                hal_vec3f origin = hal_mat4f_transform_point(
                    world, hal_vec3f_zero());
                hal_f32 w0, w1;
                hal_vec3f s0 = hal_camera_project(
                    &app->camera, vpm, origin, vp, &w0);
                hal_vec3f s1 = hal_camera_project(
                    &app->camera, vpm,
                    hal_vec3f_add(origin, hal_vec3f_mul(axis, 1.0f)),
                    vp, &w1);
                hal_vec2f dir = { s1.x - s0.x, s1.y - s0.y };
                hal_f32 len = (hal_f32)__builtin_sqrt(
                    (double)(dir.x * dir.x + dir.y * dir.y));
                if (len > 1e-3f && w0 > 0.0f) {
                    /* World units per pixel at the object's depth. */
                    hal_f32 dist = hal_vec3f_distance(
                        hal_camera_eye(&app->camera), origin);
                    hal_f32 world_per_px =
                        (2.0f * dist *
                         (hal_f32)__builtin_tan(
                             (double)app->camera.fov_y * 0.5)) /
                        vp.height;
                    hal_f32 delta =
                        (dx * dir.x + dy * dir.y) / len * world_per_px;
                    hal_object_set_location(
                        app->scene, obj->id,
                        hal_vec3f_add(obj->location,
                                      hal_vec3f_mul(axis, delta)));
                }
            }
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_BUTTON_UP) {
            hal_object *obj = hal_scene_object(
                app->scene, hal_scene_selected(app->scene));
            if (obj) {
                trs_cmd *cmd = malloc(sizeof(*cmd));
                if (cmd) {
                    cmd->id = obj->id;
                    cmd->old_loc = app->gizmo_orig_loc;
                    cmd->new_loc = obj->location;
                    cmd->old_rot = cmd->new_rot = obj->rotation;
                    cmd->old_scale = cmd->new_scale = obj->scale;
                    hal_undo_stack_push(app->undo, "Move", trs_apply,
                                        trs_revert, trs_free, cmd);
                }
                app->dirty = true;
            }
            app->gizmo_axis = -1;
            return true;
        }
    }

    /* --- Camera: MMB or Alt+LMB orbits, Shift+MMB pans --- */
    if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN && in_vp) {
        hal_u32 mods = event->pointer_button.modifiers;
        bool orbit = event->pointer_button.button == HAL_BUTTON_MIDDLE ||
                     (event->pointer_button.button == HAL_BUTTON_LEFT &&
                      (mods & HAL_MOD_ALT) != 0);
        bool pan = event->pointer_button.button == HAL_BUTTON_MIDDLE &&
                   (mods & HAL_MOD_SHIFT) != 0;
        if (!orbit && !pan &&
            event->pointer_button.button == HAL_BUTTON_LEFT) {
            /* A plain left click: gizmo first, then selection. */
            hal_object *sel =
                hal_scene_object(app->scene,
                                 hal_scene_selected(app->scene));
            if (!app->edit_mode && sel) {
                int axis = gizmo_axis_at(app, sel, vpm, vp, px, py);
                if (axis >= 0) {
                    app->gizmo_axis = axis;
                    app->gizmo_last = (hal_pointf){ px, py };
                    app->gizmo_orig_loc = sel->location;
                    return true;
                }
            }
            if (app->edit_mode) {
                edit_click(app, vpm, vp, px, py,
                           (mods & HAL_MOD_SHIFT) != 0);
                return true;
            }
            hal_pick pick =
                hal_pick_scene(app->scene, &app->camera, vpm, vp, px, py);
            if (pick.kind == HAL_PICK_OBJECT) {
                hal_scene_select_only(app->scene, pick.object);
                refresh_properties(app);
            } else {
                hal_scene_select_only(app->scene, HAL_OBJECT_ID_NONE);
                refresh_properties(app);
            }
            return true;
        }
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
        if (ctrl)
            return false; /* other ctrl combos: not ours */

        /* Plain keys (hal_scancode is unsigned; compare likewise). */
        hal_u32 key_plain = key;
        if (key_plain == 15) { /* Tab: toggle edit mode */
            if (app->edit_mode) {
                hal_object *obj =
                    hal_scene_object(app->scene, app->edit_obj);
                if (obj && obj->mesh)
                    hal_mesh_select_all(obj->mesh, false);
                app->edit_mode = false;
                set_status(app, "Object mode");
            } else {
                hal_object_id sel = hal_scene_selected(app->scene);
                hal_object *obj = hal_scene_object(app->scene, sel);
                if (obj && obj->mesh) {
                    app->edit_mode = true;
                    app->edit_obj = sel;
                    set_status(app, "Edit mode: %s", obj->name);
                } else {
                    set_status(app, "Edit mode needs a mesh object");
                }
            }
            return true;
        }
        if (!app->edit_mode) {
            hal_object *sel =
                hal_scene_object(app->scene,
                                 hal_scene_selected(app->scene));
            if (sel && (key_plain == 19 /* R */ || key_plain == 31 /* S */)) {
                app->modal_op = key_plain == 19 ? 1 : 2;
                app->modal_orig = sel->scale;
                app->modal_orig_loc = sel->location;
                app->modal_orig_rot = sel->rotation;
                app->modal_amount = 0.0f;
                app->drag_last = app->last_pointer;
                set_status(app, "%s: move the mouse, click to accept, "
                                "Esc to cancel",
                           key == 19 ? "Rotate" : "Scale");
                return true;
            }
            return false;
        }

        /* Edit-mode keys. */
        if (key_plain == 2 || key_plain == 3 ||
            key_plain == 4) { /* 1/2/3 */
            app->sel_mode = (int)key_plain - 2;
            set_status(app, "Select mode: %s",
                       app->sel_mode == 0   ? "vertex"
                       : app->sel_mode == 1 ? "edge"
                                            : "face");
            return true;
        }
        if (key_plain == 8 &&
            (event->key.modifiers & HAL_MOD_CTRL)) { /* Ctrl+R */
            menu_action(NULL, 1, 6, app);
            return true;
        }
        switch (key_plain) {
        case 8: /* E */
            apply_mesh_op(app, "Extrude", op_extrude, NULL);
            return true;
        case 23: /* I */
            apply_mesh_op(app, "Inset", op_inset, NULL);
            return true;
        case 48: /* B */
            apply_mesh_op(app, "Bevel", op_bevel, NULL);
            return true;
        case 50: /* M */
            apply_mesh_op(app, "Merge", op_merge, NULL);
            return true;
        case 45: /* X */
            menu_action(NULL, 1, 9, app);
            return true;
        case 30: /* A */
            if (event->key.modifiers & HAL_MOD_ALT) {
                menu_action(NULL, 1, 20, app);
                set_status(app, "Deselected");
            } else {
                menu_action(NULL, 1, 19, app);
                set_status(app, "Selected all");
            }
            return true;
        default:
            return false;
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

    /* Overlays: edit-mode selection and the move gizmo. */
    {
        hal_rectf vp2 = hal_app_viewport_rect(app);
        hal_size ws2;
        hal_window_get_size(app->window, &ws2);
        hal_mat4f view2 = hal_camera_view(&app->camera);
        hal_mat4f proj2 = hal_camera_projection(
            &app->camera, (hal_f32)ws2.width / (hal_f32)ws2.height);
        hal_mat4f vpm2 = hal_mat4f_mul(proj2, view2);
        const hal_theme *t = hal_ui_theme(app->ui);

        if (app->edit_mode) {
            hal_object *obj =
                hal_scene_object(app->scene, app->edit_obj);
            if (obj && obj->mesh) {
                hal_mat4f world =
                    hal_object_world_matrix(app->scene, obj);
                hal_mesh *mm = obj->mesh;
                /* Selected verts: accent dots. */
                for (size_t v = 0; v < mm->vert_count; v++) {
                    if (!mm->vert_sel || !mm->vert_sel[v])
                        continue;
                    hal_f32 w;
                    hal_vec3f sp = hal_camera_project(
                        &app->camera, vpm2,
                        hal_mat4f_transform_point(world, mm->verts[v]),
                        vp2, &w);
                    if (w > 0.0f)
                        hal_canvas_fill_circle(&canvas, sp.x, sp.y, 3.5f,
                                               t->colors[HAL_THEME_ACCENT]);
                }
                /* Selected edges: accent lines. */
                for (size_t e = 0; e < mm->edge_count; e++) {
                    if (!mm->edge_sel || !mm->edge_sel[e])
                        continue;
                    hal_f32 w0, w1;
                    hal_vec3f a = hal_camera_project(
                        &app->camera, vpm2,
                        hal_mat4f_transform_point(
                            world, mm->verts[mm->edges[e * 2]]),
                        vp2, &w0);
                    hal_vec3f b = hal_camera_project(
                        &app->camera, vpm2,
                        hal_mat4f_transform_point(
                            world, mm->verts[mm->edges[e * 2 + 1]]),
                        vp2, &w1);
                    if (w0 > 0.0f && w1 > 0.0f)
                        hal_canvas_draw_line(&canvas, a.x, a.y, b.x, b.y,
                                             t->colors[HAL_THEME_ACCENT]);
                }
                /* Selected faces: centroid dots. */
                for (size_t f = 0; f < mm->face_count; f++) {
                    if (!mm->face_sel || !mm->face_sel[f])
                        continue;
                    hal_vec3f c = hal_vec3f_zero();
                    size_t n = mm->face_off[f + 1] - mm->face_off[f];
                    for (size_t i = 0; i < n; i++)
                        c = hal_vec3f_add(
                            c, mm->verts[mm->loops[mm->face_off[f] + i]]);
                    c = hal_vec3f_mul(c, 1.0f / (hal_f32)n);
                    hal_f32 w;
                    hal_vec3f sp = hal_camera_project(
                        &app->camera, vpm2,
                        hal_mat4f_transform_point(world, c), vp2, &w);
                    if (w > 0.0f)
                        hal_canvas_fill_circle(&canvas, sp.x, sp.y, 3.0f,
                                               t->colors[HAL_THEME_TEXT]);
                }
            }
        } else {
            /* The move gizmo on the selected object. */
            hal_object *sel = hal_scene_object(
                app->scene, hal_scene_selected(app->scene));
            if (sel) {
                hal_mat4f world =
                    hal_object_world_matrix(app->scene, sel);
                hal_vec3f origin = hal_mat4f_transform_point(
                    world, hal_vec3f_zero());
                hal_f32 w;
                hal_vec3f s0 = hal_camera_project(&app->camera, vpm2,
                                                  origin, vp2, &w);
                if (w > 0.0f) {
                    static const hal_vec3f axes[3] = {
                        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }
                    };
                    static const hal_color colors[3] = {
                        { 0.88f, 0.42f, 0.46f, 1.0f },
                        { 0.60f, 0.76f, 0.47f, 1.0f },
                        { 0.38f, 0.69f, 0.93f, 1.0f },
                    };
                    hal_f32 len = 60.0f;
                    for (int i = 0; i < 3; i++) {
                        hal_vec3f s1 = hal_camera_project(
                            &app->camera, vpm2,
                            hal_vec3f_add(origin, axes[i]), vp2, &w);
                        if (w <= 0.0f)
                            continue;
                        hal_vec2f d = { s1.x - s0.x, s1.y - s0.y };
                        hal_f32 dl = (hal_f32)__builtin_sqrt(
                            (double)(d.x * d.x + d.y * d.y));
                        if (dl < 1e-3f)
                            continue;
                        hal_vec2f tip = { s0.x + d.x / dl * len,
                                          s0.y + d.y / dl * len };
                        hal_canvas_draw_line(&canvas, s0.x, s0.y, tip.x,
                                             tip.y, colors[i]);
                        hal_canvas_fill_circle(&canvas, tip.x, tip.y,
                                               4.0f, colors[i]);
                        if (app->gizmo_axis == i)
                            hal_canvas_fill_circle(&canvas, tip.x,
                                                   tip.y, 7.0f,
                                                   colors[i]);
                    }
                }
            }
        }
    }

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
