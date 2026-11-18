/*
 * shell.c — the Halcyon editor shell: UI assembly, scene wiring,
 * menus, shortcuts, project and autosave driving
 */

#include "shell.h"
#include "core/anim.h"
#include "core/armature.h"
#include "core/material.h"
#include "core/mesh.h"
#include "core/mesh_ops.h"
#include "core/uv.h"
#include "io/io.h"
#include "core/project.h"
#include "render/pick.h"
#include "render/preview.h"
#include "render/raster.h"
#include "ui/theme.h"
#include "render/raster.h"
#include "ui/theme.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout metrics (logical px). */
#define SHELL_MENUBAR_H 30.0f
#define SHELL_STATUSBAR_H 26.0f
#define SHELL_TIMELINE_H 64.0f
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
    char mat_buf[3][16];
    hal_f32 metallic_value, rough_value;

    /* Sidebar widgets. */
    hal_widget *mat_entry[3];
    hal_widget *metallic_slider, *rough_slider;
    hal_widget *uvview;
    hal_widget *curves;

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

    /* Pose mode (Tab on an armature): the selected bone drives R and
     * IK. */
    bool pose_mode;
    hal_object_id pose_obj;
    int bone_modal;         /* bone index during modal rotation */
    hal_quatf bone_modal_orig;

    /* Preview rendering: which Render menu item opened the dialog. */
    int render_kind;

    /* Material textures decode through this cache (owned). */
    hal_texture_cache *textures;

    /* glTF export options (File menu toggles). */
    bool export_z_up;
    bool export_cm;

    /* Weight paint mode: drag over a bound mesh to adjust the
     * active bone's weights (Shift subtracts). */
    bool paint_mode;
    bool painting;
    bool paint_subtract;
    int paint_bone;
    hal_mesh *paint_snapshot; /* stroke undo base (owned)  */
    hal_object_id paint_obj;

    /* Animation: the active clip, the playhead, playback. */
    int active_clip;        /* -1 none                            */
    hal_f32 play_time;      /* seconds                            */
    bool playing;
    bool timeline_drag;     /* scrubbing the playhead             */
    hal_rectf timeline_rect;

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

        hal_material *mat = obj->material >= 0
                                ? hal_scene_material(app->scene,
                                                     obj->material)
                                : NULL;
        if (mat) {
            snprintf(app->mat_buf[0], sizeof(app->mat_buf[0]), "%.2f",
                     (double)mat->base_color.r);
            snprintf(app->mat_buf[1], sizeof(app->mat_buf[1]), "%.2f",
                     (double)mat->base_color.g);
            snprintf(app->mat_buf[2], sizeof(app->mat_buf[2]), "%.2f",
                     (double)mat->base_color.b);
            app->metallic_value = mat->metallic;
            app->rough_value = mat->roughness;
        } else {
            for (int i = 0; i < 3; i++)
                app->mat_buf[i][0] = '\0';
            app->metallic_value = 0.0f;
            app->rough_value = 0.5f;
        }
        hal_uvview_bind(app->uvview, obj->mesh);
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

static void on_mat_changed(hal_widget *entry, void *user) {
    hal_app *app = user;
    (void)entry;
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || obj->material < 0 || app->syncing)
        return;
    hal_material *mat = hal_scene_material(app->scene, obj->material);
    if (!mat)
        return;
    int axis = -1;
    for (int i = 0; i < 3; i++)
        if (app->mat_entry[i] == entry)
            axis = i;
    if (axis < 0)
        return;
    char *end = NULL;
    double v = strtod(app->mat_buf[axis], &end);
    if (!end || end == app->mat_buf[axis] || v < 0.0 || v > 1.0)
        return; /* still typing / out of range */
    if (axis == 0)
        mat->base_color.r = (hal_f32)v;
    else if (axis == 1)
        mat->base_color.g = (hal_f32)v;
    else
        mat->base_color.b = (hal_f32)v;
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

/* ---- Animation (clip) undo ---- */

typedef struct anim_cmd {
    size_t clip_index;
    hal_clip *snapshot; /* pre-edit contents; swap for undo/redo */
} anim_cmd;

/* Swaps the clip's CONTENTS in place (the scene owns the slot; the
 * snapshot owns its own channel arrays, so pointer-swapping is
 * ownership-swapping). */
static void anim_swap(hal_scene *scene, size_t index,
                      hal_clip **with) {
    if (!scene || index >= scene->clip_count)
        return;
    hal_clip tmp = scene->clips[index];
    scene->clips[index] = **with;
    **with = tmp;
}

static void anim_cmd_apply(void *state, void *ctx) {
    anim_cmd *c = state;
    anim_swap(ctx, c->clip_index, &c->snapshot);
}

static void anim_cmd_revert(void *state, void *ctx) {
    anim_cmd *c = state;
    anim_swap(ctx, c->clip_index, &c->snapshot);
}

static void anim_cmd_free(void *state) {
    anim_cmd *c = state;
    hal_clip_destroy(c->snapshot);
    free(c);
}

/* The curve editor's edit callback: BEGIN snapshots, END pushes one
 * undo command per edit, INFO reports status. */
static hal_clip *curves_edit_snapshot = NULL;

static void curves_edit_notify(int phase, const char *text,
                               void *user) {
    hal_app *app = user;
    switch (phase) {
    case HAL_CURVES_EDIT_BEGIN: {
        hal_clip *clip =
            hal_scene_clip(app->scene, app->active_clip);
        hal_clip_destroy(curves_edit_snapshot);
        curves_edit_snapshot = hal_clip_clone(clip);
        break;
    }
    case HAL_CURVES_EDIT_END: {
        if (curves_edit_snapshot && app->active_clip >= 0) {
            anim_cmd *cmd = malloc(sizeof(*cmd));
            if (cmd) {
                cmd->clip_index = (size_t)app->active_clip;
                cmd->snapshot = curves_edit_snapshot;
                curves_edit_snapshot = NULL;
                hal_undo_stack_push(app->undo, "Curve Edit",
                                    anim_cmd_apply,
                                    anim_cmd_revert, anim_cmd_free,
                                    cmd);
            }
        }
        hal_clip_destroy(curves_edit_snapshot);
        curves_edit_snapshot = NULL;
        app->dirty = true;
        if (text)
            set_status(app, "Curve editor: %s", text);
        break;
    }
    default:
        if (text)
            set_status(app, "Curve editor: easing %s", text);
        break;
    }
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
typedef struct op_unwrap_user {
    int mode; /* 0 planar X, 1 planar Y, 2 planar Z, 3 box, 4 smart */
} op_unwrap_user;
static bool op_unwrap(hal_mesh *m, void *user) {
    op_unwrap_user *u = user;
    switch (u->mode) {
    case 0: return hal_mesh_unwrap_planar(m, HAL_UV_X);
    case 1: return hal_mesh_unwrap_planar(m, HAL_UV_Y);
    case 2: return hal_mesh_unwrap_planar(m, HAL_UV_Z);
    case 3: return hal_mesh_unwrap_box(m);
    default: return hal_mesh_unwrap_smart(m);
    }
}
static bool op_recalc(hal_mesh *m, void *user) {
    (void)user;
    hal_mesh_recalc_normals(m);
    return true;
}

/* Batch export: one glTF per mesh object, prefix_NNN.glb. */
static void batch_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Batch export cancelled");
        return;
    }
    /* Strip the file name to its directory + base prefix. */
    char base[512];
    snprintf(base, sizeof(base), "%s", path);
    char *slash = strrchr(base, '/');
    const char *file = slash ? slash + 1 : base;
    char dir[512];
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - base), base);
    } else {
        strcpy(dir, ".");
    }
    /* Cut the extension off the file name. */
    const char *dot = NULL;
    for (const char *p = file; *p; p++)
        if (*p == '.')
            dot = p;
    size_t name_len = dot ? (size_t)(dot - file) : strlen(file);
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%.*s", (int)name_len, file);

    char err[256] = { 0 };
    char out[900];
    size_t exported = 0;
    for (size_t i = 0; i < app->scene->count; i++) {
        hal_object *obj = &app->scene->objects[i];
        if (!obj->mesh)
            continue;
        /* Build a one-object scene sharing the mesh pointer. */
        hal_scene *one = hal_scene_create(obj->name);
        hal_object_id id =
            hal_scene_object_add(one, obj->name, HAL_OBJECT_MESH);
        hal_object_set_location(one, id, obj->location);
        hal_object_set_rotation(one, id, obj->rotation);
        hal_object_set_scale(one, id, obj->scale);
        /* Clone so the temp scene owns its copy. */
        hal_object_set_mesh(one, id, hal_mesh_clone(obj->mesh));
        snprintf(out, sizeof(out), "%s/%s_%03zu.glb", dir, prefix,
                 exported + 1);
        hal_gltf_export_opts opts = hal_gltf_export_opts_default();
        opts.z_up = app->export_z_up;
        opts.unit_scale = app->export_cm ? 100.0f : 1.0f;
        if (hal_io_gltf_export_ex(one, out, &opts, err,
                                  sizeof(err)) == HAL_OK)
            exported++;
        hal_scene_destroy(one);
    }
    set_status(app, "Batch exported %zu objects", exported);
    free(path);
}

/* Texture load completion: an image for the selected object's
 * material (a material is created when the object has none). */
static void tex_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Texture load cancelled");
        return;
    }
    hal_object_id sel = hal_scene_selected(app->scene);
    hal_object *obj = hal_scene_object(app->scene, sel);
    if (!obj || !obj->mesh) {
        free(path);
        return;
    }
    int img = hal_scene_image_add(app->scene, path);
    if (img < 0) {
        set_status(app, "Texture: out of memory");
        free(path);
        return;
    }
    if (obj->material < 0) {
        hal_material mat = hal_material_default();
        snprintf(mat.name, sizeof(mat.name), "Material %zu",
                 hal_scene_material_count(app->scene));
        obj->material = hal_scene_material_add(app->scene, &mat);
    }
    hal_material *mat = hal_scene_material(app->scene, obj->material);
    if (mat)
        mat->texture = img;
    /* Warm the cache; a failed decode reports immediately. */
    if (hal_texture_cache_get(app->textures, path))
        set_status(app, "Texture: %s", path);
    else
        set_status(app, "Texture: cannot decode %s", path);
    app->dirty = true;
    free(path);
}

/* Box collider export: the selected object's WORLD axis-aligned
 * bounding box as a one-box glTF scene (the standard simple collider
 * for game engines; convex hulls are future work — PROGRESS.md). */
static void collider_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Collider export cancelled");
        return;
    }
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || !obj->mesh) {
        set_status(app, "Collider: select a mesh object");
        free(path);
        return;
    }
    hal_vec3f bmin = hal_vec3f_make(1e30f, 1e30f, 1e30f);
    hal_vec3f bmax = hal_vec3f_make(-1e30f, -1e30f, -1e30f);
    hal_mat4f world = hal_object_world_matrix(app->scene, obj);
    for (size_t v = 0; v < obj->mesh->vert_count; v++) {
        hal_vec3f p = hal_mat4f_transform_point(world,
                                                obj->mesh->verts[v]);
        bmin.x = fminf(bmin.x, p.x);
        bmin.y = fminf(bmin.y, p.y);
        bmin.z = fminf(bmin.z, p.z);
        bmax.x = fmaxf(bmax.x, p.x);
        bmax.y = fmaxf(bmax.y, p.y);
        bmax.z = fmaxf(bmax.z, p.z);
    }
    hal_vec3f size = hal_vec3f_sub(bmax, bmin);
    hal_vec3f center = hal_vec3f_mul(hal_vec3f_add(bmin, bmax), 0.5f);

    hal_scene *one = hal_scene_create("Collider");
    hal_object_id id = hal_scene_object_add(one, obj->name,
                                            HAL_OBJECT_MESH);
    /* A box mesh from the bounds: 8 corners, 6 quads. */
    hal_mesh *box = hal_mesh_create();
    hal_f32 hx = size.x * 0.5f, hy = size.y * 0.5f,
            hz = size.z * 0.5f;
    hal_vec3f c[8] = {
        { -hx, -hy, -hz }, { hx, -hy, -hz }, { hx, -hy, hz },
        { -hx, -hy, hz },  { -hx, hy, -hz }, { hx, hy, -hz },
        { hx, hy, hz },    { -hx, hy, hz },
    };
    for (int i = 0; i < 8; i++)
        hal_mesh_vert_add(box, c[i]);
    /* Outward windings (same layout as hal_mesh_cube). */
    hal_u32 q0[4] = { 0, 1, 2, 3 };
    hal_u32 q1[4] = { 7, 6, 5, 4 };
    hal_u32 q2[4] = { 4, 5, 1, 0 };
    hal_u32 q3[4] = { 5, 6, 2, 1 };
    hal_u32 q4[4] = { 6, 7, 3, 2 };
    hal_u32 q5[4] = { 7, 4, 0, 3 };
    hal_mesh_face_add(box, q0, 4);
    hal_mesh_face_add(box, q1, 4);
    hal_mesh_face_add(box, q2, 4);
    hal_mesh_face_add(box, q3, 4);
    hal_mesh_face_add(box, q4, 4);
    hal_mesh_face_add(box, q5, 4);
    hal_mesh_rebuild_edges(box);
    hal_object_set_mesh(one, id, box);
    hal_object_set_location(one, id, center);

    char err[256] = { 0 };
    hal_gltf_export_opts opts = hal_gltf_export_opts_default();
    opts.z_up = app->export_z_up;
    opts.unit_scale = app->export_cm ? 100.0f : 1.0f;
    hal_result r =
        hal_io_gltf_export_ex(one, path, &opts, err, sizeof(err));
    set_status(app, r == HAL_OK ? "Collider exported to %s"
                                : "Collider export failed",
               r == HAL_OK ? path : (err[0] ? err : "I/O error"));
    hal_scene_destroy(one);
    free(path);
}

/* Convex collider export: the quickhull of the selected object's
 * WORLD-space vertices, named <object>_UCX (the widely-read convex
 * collision naming convention — see docs/decisions.md D-016).
 * Degenerate inputs (coplanar point sets) report and suggest the box
 * collider instead. */
static void convex_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Convex export cancelled");
        return;
    }
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || !obj->mesh) {
        set_status(app, "Convex: select a mesh object");
        free(path);
        return;
    }

    /* The source points in WORLD space. */
    hal_mat4f world = hal_object_world_matrix(app->scene, obj);
    hal_mesh *src = hal_mesh_create();
    bool ok = src != NULL;
    for (size_t v = 0; ok && v < obj->mesh->vert_count; v++)
        hal_mesh_vert_add(
            src, hal_mat4f_transform_point(world,
                                           obj->mesh->verts[v]));
    hal_mesh *hull = ok ? hal_mesh_convex_hull(src) : NULL;
    hal_mesh_destroy(src);
    if (!hull) {
        set_status(app,
                   "Convex: degenerate point set — use the box"
                   " collider");
        free(path);
        return;
    }

    char name[128];
    snprintf(name, sizeof(name), "%s_UCX", obj->name);
    hal_scene *one = hal_scene_create("Collider");
    hal_object_id id =
        hal_scene_object_add(one, name, HAL_OBJECT_MESH);
    hal_object_set_mesh(one, id, hull); /* world-space, identity */

    char err[256] = { 0 };
    hal_gltf_export_opts opts = hal_gltf_export_opts_default();
    opts.z_up = app->export_z_up;
    opts.unit_scale = app->export_cm ? 100.0f : 1.0f;
    hal_result r =
        hal_io_gltf_export_ex(one, path, &opts, err, sizeof(err));
    set_status(app, r == HAL_OK ? "Convex collider (%zu tris) -> %s"
                                : "Convex export failed",
               r == HAL_OK ? hull->face_count : 0,
               r == HAL_OK ? path : (err[0] ? err : "I/O error"));
    hal_scene_destroy(one);
    free(path);
}

/* Theme load: swap the UI theme and refresh the snapshot render
 * options (they copy theme colors at startup). */
static void theme_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Theme load cancelled");
        return;
    }
    char err[256] = { 0 };
    hal_theme *theme = hal_theme_load(path, err, sizeof(err));
    if (!theme) {
        set_status(app, "Theme failed: %s", err[0] ? err : "bad file");
        free(path);
        return;
    }
    const hal_theme *old = hal_ui_theme(app->ui);
    app->render_opts.bg = theme->colors[HAL_THEME_VIEWPORT_BG];
    app->render_opts.grid = theme->colors[HAL_THEME_VIEWPORT_GRID];
    app->render_opts.object = theme->colors[HAL_THEME_ACCENT];
    hal_ui_set_theme(app->ui, theme);
    (void)old;
    set_status(app, "Theme: %s", theme->name ? theme->name : path);
    app->dirty = true;
    free(path);
}

/* ---- Preview renders ---- */

static void render_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Render cancelled");
        return;
    }
    char err[256] = { 0 };
    hal_result r = HAL_ERR_IO;
    /* The menu item that opened the dialog (0 preview, 1 turntable,
     * 2 thumbnail). */
    switch (app->render_kind) {
    case 0: {
        hal_camera cam = hal_camera_frame_scene(app->scene,
                                                app->camera, 1.15f);
        r = hal_preview_render(app->scene, cam, 1280, 720, true, path,
                               err, sizeof(err));
        break;
    }
    case 1: {
        /* Strip ".png" from the chosen file to get the prefix. */
        size_t n = strlen(path);
        if (n > 4 && strcmp(path + n - 4, ".png") == 0)
            path[n - 4] = '\0';
        hal_camera cam = hal_camera_frame_scene(app->scene,
                                                app->camera, 1.15f);
        r = hal_preview_turntable(app->scene, cam, 640, 480, 24, path,
                                  err, sizeof(err));
        break;
    }
    default:
        r = hal_preview_thumbnail(app->scene, 256, path, err,
                                  sizeof(err));
        break;
    }
    set_status(app, r == HAL_OK ? "Rendered %s" : "Render failed: %s",
               r == HAL_OK ? path : (err[0] ? err : "I/O error"));
    free(path);
}

/* ---- Export / import ---- */

typedef struct export_req {
    hal_app *app;
    int format; /* 0 glb, 1 obj, 2 stl */
} export_req;

static void export_done(char *path, void *user) {
    export_req *req = user;
    hal_app *app = req->app;
    int format = req->format;
    free(req);
    if (!path) {
        set_status(app, "Export cancelled");
        return;
    }
    char err[256] = { 0 };
    hal_result r = HAL_ERR_IO;
    if (format == 0) {
        hal_gltf_export_opts opts = hal_gltf_export_opts_default();
        opts.z_up = app->export_z_up;
        opts.unit_scale = app->export_cm ? 100.0f : 1.0f;
        r = hal_io_gltf_export_ex(app->scene, path, &opts, err,
                                  sizeof(err));
    }
    else if (format == 1)
        r = hal_io_obj_export(app->scene, path, err, sizeof(err));
    else if (format == 2)
        r = hal_io_stl_export(app->scene, path, err, sizeof(err));
    if (r == HAL_OK)
        set_status(app, "Exported %s", path);
    else
        set_status(app, "Export failed: %s",
                   err[0] ? err : "I/O error");
    free(path);
}

static void export_as(hal_app *app, int format) {
    export_req *req = malloc(sizeof(*req));
    if (!req)
        return;
    req->app = app;
    req->format = format;
    const char *title = format == 0 ? "Export glTF"
                        : format == 1 ? "Export OBJ"
                                      : "Export STL";
    const char *filter = format == 0 ? "*.glb"
                         : format == 1 ? "*.obj"
                                       : "*.stl";
    if (hal_ok(hal_dialog_save_file(app->platform, title, filter,
                                    "model.glb", export_done, req,
                                    app->window)))
        set_status(app, "Choose the export location...");
    else
        free(req);
}

static void import_done(char *path, void *user) {
    hal_app *app = user;
    if (!path) {
        set_status(app, "Import cancelled");
        return;
    }
    char err[256] = { 0 };
    hal_scene *loaded = NULL;
    hal_result r = HAL_ERR_IO;
    size_t n = strlen(path);
    if (n > 4 && strcmp(path + n - 4, ".glb") == 0)
        r = hal_io_gltf_import(path, &loaded, err, sizeof(err));
    else if (n > 4 && strcmp(path + n - 4, ".obj") == 0)
        r = hal_io_obj_import(path, &loaded, err, sizeof(err));
    else {
        set_status(app, "Import: unsupported file type");
        free(path);
        return;
    }

    if (r == HAL_OK && loaded) {
        hal_scene_destroy(app->scene);
        app->scene = loaded;
        hal_undo_stack_clear(app->undo);
        app->edit_mode = false;
        app->project_path[0] = '\0';
        app->dirty = true;
        rebuild_outliner(app);
        refresh_properties(app);
        set_status(app, "Imported %s", path);
    } else {
        set_status(app, "Import failed: %s",
                   err[0] ? err : "bad file");
    }
    free(path);
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
    hal_menu_item_add(bar, file, "Export glTF (.glb)...", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, file, "Export OBJ...", NULL, menu_action, app);
    hal_menu_item_add(bar, file, "Export STL...", NULL, menu_action, app);
    hal_menu_item_add(bar, file, "Import glTF/OBJ...", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, file, "Batch Export glTF (per object)...",
                      NULL, menu_action, app);
    hal_menu_item_add(bar, file, "Export glTF: Z-Up", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, file, "Export glTF: Centimeters", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, file, "Export Box Collider (.glb)...",
                      NULL, menu_action, app);
    hal_menu_item_add(bar, file, "Export Convex Collider (.glb)...",
                      NULL, menu_action, app);
    hal_menu_item_add(bar, file, "Load Theme...", NULL, menu_action,
                      app);
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
    hal_menu_item_add(bar, edit, "Unwrap Planar X", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Unwrap Planar Y", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Unwrap Planar Z", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Unwrap Box", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Unwrap Smart (lite)", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "New Material", NULL, menu_action, app);
    hal_menu_item_add(bar, edit, "Load Texture...", NULL, menu_action,
                      app);
    hal_menu_item_add(bar, edit, "Toggle Texture Filter", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Origin to BBox Center", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, "Origin to World Origin", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Select All", "A", menu_action, app);
    hal_menu_item_add(bar, edit, "Deselect All", "Alt+A", menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Bind Mesh to Armature", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, "Add Bone (child)", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, "Solve IK (tip to mouse)", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, edit, NULL, NULL, NULL, NULL);
    hal_menu_item_add(bar, edit, "Generate LOD", NULL, menu_action,
                      app);

    int render_menu = hal_menu_add(bar, "Render");
    hal_menu_item_add(bar, render_menu, "Preview PNG...", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, render_menu, "Turntable Frames...", NULL,
                      menu_action, app);
    hal_menu_item_add(bar, render_menu, "Thumbnail...", NULL,
                      menu_action, app);

    int anim_menu = hal_menu_add(bar, "Animation");
    hal_menu_item_add(bar, anim_menu, "New Clip", NULL, menu_action, app);
    hal_menu_item_add(bar, anim_menu, "Insert Key", "I", menu_action, app);
    hal_menu_item_add(bar, anim_menu, "Play / Pause", "Space",
                      menu_action, app);
    hal_menu_item_add(bar, anim_menu, "Toggle Loop", NULL, menu_action,
                      app);

    int add = hal_menu_add(bar, "Add");
    hal_menu_item_add(bar, add, "Cube", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Sphere", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Plane", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Cylinder", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Cone", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Torus", NULL, menu_action, app);
    hal_menu_item_add(bar, add, "Preset: Crate", NULL, menu_action,
                      app);
    hal_menu_item_add(bar, add, "Preset: Barrel", NULL, menu_action,
                      app);
    hal_menu_item_add(bar, add, "Preset: Sword", NULL, menu_action,
                      app);
    hal_menu_item_add(bar, add, "Preset: Table", NULL, menu_action,
                      app);
    hal_menu_item_add(bar, add, "Armature", NULL, menu_action, app);
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

/* Multi-object asset presets (the M9 game workflow). Each builds a
 * small composition from primitives with sensible materials. */
static void add_preset(hal_app *app, int which) {
    if (!app)
        return;
    switch (which) {
    case 0: { /* Crate: a wooden cube. */
        add_object(app, "Crate", HAL_OBJECT_MESH, hal_vec3f_zero());
        hal_object *o =
            &app->scene->objects[app->scene->count - 1];
        hal_object_id id = o->id;
        hal_material mat = hal_material_default();
        strcpy(mat.name, "Wood");
        mat.base_color = hal_color_rgb8(0x9C, 0x6B, 0x3F);
        mat.roughness = 0.85f;
        hal_object_set_material(
            app->scene, id,
            hal_scene_material_add(app->scene, &mat));
        (void)o;
        break;
    }
    case 1: { /* Barrel: a cylinder with a metal band material. */
        add_object(app, "Barrel", HAL_OBJECT_MESH,
                   hal_vec3f_zero());
        hal_object *o =
            &app->scene->objects[app->scene->count - 1];
        hal_object_id id = o->id;
        if (o && o->mesh) {
            hal_mesh *m = hal_mesh_cylinder(0.45f, 1.2f, 16);
            hal_object_set_mesh(app->scene, id, m);
            hal_object_set_location(app->scene, id,
                                    hal_vec3f_make(0, 0.6f, 0));
        }
        hal_material mat = hal_material_default();
        strcpy(mat.name, "BarrelSteel");
        mat.base_color = hal_color_rgb8(0x5E, 0x66, 0x6E);
        mat.metallic = 0.7f;
        mat.roughness = 0.35f;
        hal_object_set_material(
            app->scene, id,
            hal_scene_material_add(app->scene, &mat));
        break;
    }
    case 2: { /* Sword: blade + guard + handle. */
        add_object(app, "Blade", HAL_OBJECT_MESH, hal_vec3f_zero());
        hal_object *ob =
            &app->scene->objects[app->scene->count - 1];
        hal_object_id blade = ob->id;
        if (ob && ob->mesh) {
            hal_mesh *m = hal_mesh_create();
            hal_f32 w = 0.06f, t = 0.015f, len = 1.0f;
            hal_vec3f c[8] = {
                { -w, 0, -t }, { w, 0, -t }, { w, 0, t }, { -w, 0, t },
                { -w * 0.4f, len, -t * 0.4f },
                { w * 0.4f, len, -t * 0.4f },
                { w * 0.4f, len, t * 0.4f },
                { -w * 0.4f, len, t * 0.4f },
            };
            for (int i = 0; i < 8; i++)
                hal_mesh_vert_add(m, c[i]);
            hal_u32 q[6][4] = {
                { 0, 3, 7, 4 }, { 1, 5, 6, 2 }, { 0, 1, 2, 3 },
                { 4, 7, 6, 5 }, { 0, 4, 5, 1 }, { 3, 2, 6, 7 },
            };
            for (int i = 0; i < 6; i++)
                hal_mesh_face_add(m, q[i], 4);
            hal_mesh_rebuild_edges(m);
            hal_object_set_mesh(app->scene, blade, m);
            hal_object_set_location(app->scene, blade,
                                    hal_vec3f_make(0, 0.12f, 0));
        }
        hal_material mm = hal_material_default();
        strcpy(mm.name, "Steel");
        mm.base_color = hal_color_rgb8(0xC8, 0xCC, 0xD4);
        mm.metallic = 0.9f;
        mm.roughness = 0.2f;
        hal_object_set_material(
            app->scene, blade,
            hal_scene_material_add(app->scene, &mm));

        add_object(app, "Guard", HAL_OBJECT_MESH,
                   hal_vec3f_make(0, 0.08f, 0));
        hal_object *og =
            &app->scene->objects[app->scene->count - 1];
        hal_object_id guard = og->id;
        if (og && og->mesh)
            hal_object_set_mesh(app->scene, guard,
                                hal_mesh_cube(0.16f));
        hal_object_set_scale(app->scene, guard,
                             hal_vec3f_make(3.2f, 0.25f, 0.6f));
        add_object(app, "Handle", HAL_OBJECT_MESH,
                   hal_vec3f_make(0, -0.12f, 0));
        hal_object *oh =
            &app->scene->objects[app->scene->count - 1];
        if (oh && oh->mesh)
            hal_object_set_mesh(app->scene, oh->id,
                                hal_mesh_cylinder(0.025f, 0.24f, 8));
        break;
    }
    default: { /* Table: 4 legs + a top. */
        add_object(app, "TableTop", HAL_OBJECT_MESH,
                   hal_vec3f_make(0, 0.72f, 0));
        hal_object *ot =
            &app->scene->objects[app->scene->count - 1];
        hal_object_id top = ot->id;
        if (ot && ot->mesh)
            hal_object_set_mesh(app->scene, top,
                                hal_mesh_cube(0.5f));
        hal_object_set_scale(app->scene, top,
                             hal_vec3f_make(1.6f, 0.06f, 0.9f));
        for (int i = 0; i < 4; i++) {
            hal_f32 lx = i & 1 ? 0.7f : -0.7f;
            hal_f32 lz = i & 2 ? 0.35f : -0.35f;
            char name[32];
            snprintf(name, sizeof(name), "Leg%d", i + 1);
            add_object(app, name, HAL_OBJECT_MESH,
                       hal_vec3f_make(lx, 0.35f, lz));
            hal_object *ol =
                &app->scene->objects[app->scene->count - 1];
            if (ol && ol->mesh)
                hal_object_set_mesh(app->scene, ol->id,
                                    hal_mesh_cylinder(0.045f, 0.7f, 8));
        }
        break;
    }
    }
    app->dirty = true;
    rebuild_outliner(app);
    set_status(app, "Preset added");
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
            export_as(app, 0);
            break;
        case 6:
            export_as(app, 1);
            break;
        case 7:
            export_as(app, 2);
            break;
        case 8:
            if (hal_ok(hal_dialog_open_file(
                    app->platform, "Import model", "*.glb;*.obj",
                    import_done, app, app->window)))
                set_status(app, "Choose a file to import...");
            break;
        case 9:
            if (hal_ok(hal_dialog_save_file(
                    app->platform, "Batch export prefix", "*.glb",
                    "object.glb", batch_done, app, app->window)))
                set_status(app, "Choose the batch output location...");
            break;
        case 10:
            app->export_z_up = !app->export_z_up;
            set_status(app, "glTF export up-axis: %s",
                       app->export_z_up ? "Z-up (+90 X baked into "
                                          "roots)"
                                        : "Y-up (glTF native)");
            break;
        case 11:
            app->export_cm = !app->export_cm;
            set_status(app, "glTF export units: %s",
                       app->export_cm ? "centimeters (x100)"
                                      : "meters");
            break;
        case 12:
            if (hal_ok(hal_dialog_save_file(
                    app->platform, "Export box collider", "*.glb",
                    "collider.glb", collider_done, app,
                    app->window)))
                set_status(app, "Choose the collider output...");
            break;
        case 13:
            if (hal_ok(hal_dialog_save_file(
                    app->platform, "Export convex collider", "*.glb",
                    "collider.glb", convex_done, app,
                    app->window)))
                set_status(app, "Choose the collider output...");
            break;
        case 14:
            if (hal_ok(hal_dialog_open_file(
                    app->platform, "Load theme", "*.haltheme",
                    theme_done, app, app->window)))
                set_status(app, "Choose a theme file...");
            break;
        case 16:
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
        case 19:
        case 20:
        case 21:
        case 22:
        case 23: {
            static op_unwrap_user uu;
            uu.mode = item - 19;
            apply_mesh_op(app, "Unwrap", op_unwrap, &uu);
            break;
        }
        case 24: {
            /* New Material: creates one and assigns it to the
             * selected object. */
            hal_object_id sel = hal_scene_selected(app->scene);
            hal_object *obj = hal_scene_object(app->scene, sel);
            if (!obj)
                break;
            hal_material mat = hal_material_default();
            snprintf(mat.name, sizeof(mat.name), "Material %zu",
                     hal_scene_material_count(app->scene));
            int idx = hal_scene_material_add(app->scene, &mat);
            if (idx >= 0) {
                hal_object_set_material(app->scene, sel, idx);
                set_status(app, "Created %s", mat.name);
                app->dirty = true;
            }
            break;
        }
        case 34: {
            /* Bind: the selected mesh skins to the selected
             * armature. */
            hal_object_id mesh_id = hal_scene_selected(app->scene);
            hal_object *mesh_obj = hal_scene_object(app->scene, mesh_id);
            hal_object_id arm_id = HAL_OBJECT_ID_NONE;
            for (size_t i = 0; i < app->scene->count; i++)
                if (app->scene->objects[i].armature &&
                    app->scene->objects[i].id != mesh_id)
                    arm_id = app->scene->objects[i].id;
            hal_object *arm_obj = hal_scene_object(app->scene, arm_id);
            if (!mesh_obj || !mesh_obj->mesh || !arm_obj ||
                !arm_obj->armature) {
                set_status(app, "Bind: select a mesh, have an armature");
                break;
            }
            if (hal_mesh_bind_armature(mesh_obj->mesh,
                                       arm_obj->armature)) {
                hal_object_set_skin(app->scene, mesh_id, arm_id);
                set_status(app, "Bound %s to %s", mesh_obj->name,
                           arm_obj->name);
                app->dirty = true;
            }
            break;
        }
        case 35: {
            /* Add a child bone to the selected bone of the posed
             * armature. */
            hal_object *arm_obj =
                hal_scene_object(app->scene, app->pose_obj);
            if (!arm_obj || !arm_obj->armature) {
                set_status(app, "Add Bone: enter pose mode (Tab) first");
                break;
            }
            hal_armature *arm = arm_obj->armature;
            int parent = -1;
            for (size_t b = 0; b < arm->count; b++)
                if (arm->bones[b].selected)
                    parent = (int)b;
            char name[64];
            snprintf(name, sizeof(name), "Bone %zu", arm->count);
            if (parent >= 0) {
                hal_vec3f tail = arm->bones[parent].tail;
                hal_vec3f dir = hal_vec3f_mul(
                    hal_vec3f_normalize(hal_vec3f_sub(
                        tail, arm->bones[parent].head)),
                    hal_vec3f_distance(arm->bones[parent].head, tail));
                hal_armature_bone_add(arm, name, tail,
                                      hal_vec3f_add(tail, dir), parent);
            } else {
                hal_armature_bone_add(arm, name, hal_vec3f_zero(),
                                      hal_vec3f_make(0, 1, 0), -1);
            }
            set_status(app, "Added %s", name);
            app->dirty = true;
            break;
        }
        case 36: {
            /* IK: the selected bone (tip) solves toward the mouse
             * ray at the tip's height plane. */
            hal_object *arm_obj =
                hal_scene_object(app->scene, app->pose_obj);
            if (!arm_obj || !arm_obj->armature) {
                set_status(app, "IK: enter pose mode (Tab) first");
                break;
            }
            hal_armature *arm = arm_obj->armature;
            int tip = -1;
            for (size_t b = 0; b < arm->count; b++)
                if (arm->bones[b].selected)
                    tip = (int)b;
            if (tip < 0 || arm->bones[tip].parent < 0) {
                set_status(app, "IK: select a child bone");
                break;
            }
            hal_rectf vp = hal_app_viewport_rect(app);
            hal_size ws;
            hal_window_get_size(app->window, &ws);
            hal_mat4f view = hal_camera_view(&app->camera);
            hal_mat4f proj = hal_camera_projection(
                &app->camera, (hal_f32)ws.width / (hal_f32)ws.height);
            hal_mat4f vpm = hal_mat4f_mul(proj, view);
            hal_bone_pose *poses =
                malloc(arm->count * sizeof(*poses));
            if (!poses)
                break;
            hal_armature_pose(arm, poses);
            hal_vec3f tip_world = hal_mat4f_transform_point(
                hal_object_world_matrix(app->scene, arm_obj),
                poses[tip].tail);
            free(poses);
            hal_ray ray = hal_camera_ray(&app->camera, vpm, vp,
                                         app->last_pointer.x,
                                         app->last_pointer.y);
            /* Intersect the ray with the horizontal plane at the
             * tip's height (simple, predictable). */
            hal_f32 denom = ray.dir.y;
            if (fabsf(denom) < 1e-6f) {
                set_status(app, "IK: view too level for the target");
                break;
            }
            hal_f32 t = (tip_world.y - ray.origin.y) / denom;
            if (t <= 0.0f) {
                set_status(app, "IK: target behind the camera");
                break;
            }
            hal_vec3f target = hal_vec3f_add(
                ray.origin, hal_vec3f_mul(ray.dir, t));
            hal_mat4f world_inv = hal_mat4f_invert(
                hal_object_world_matrix(app->scene, arm_obj));
            hal_vec3f local =
                hal_mat4f_transform_point(world_inv, target);
            if (hal_armature_ik_2bone(arm, tip, local))
                set_status(app, "IK solved");
            else
                set_status(app, "IK: chain not solvable");
            app->dirty = true;
            break;
        }
        case 25: { /* Load Texture for the selected object's material */
            hal_object_id sel = hal_scene_selected(app->scene);
            hal_object *obj = hal_scene_object(app->scene, sel);
            if (!obj || !obj->mesh) {
                set_status(app, "Load Texture: select a mesh object");
                break;
            }
            if (hal_ok(hal_dialog_open_file(
                    app->platform, "Choose a texture image",
                    "*.png;*.jpg;*.jpeg;*.bmp", tex_done, app,
                    app->window)))
                set_status(app, "Choose an image...");
            break;
        }
        case 26: {
            hal_object *obj =
                hal_scene_object(app->scene,
                                 hal_scene_selected(app->scene));
            if (obj && obj->material >= 0) {
                hal_material *mat =
                    hal_scene_material(app->scene, obj->material);
                if (mat) {
                    mat->filter = !mat->filter;
                    set_status(app, "Texture filter: %s",
                               mat->filter ? "bilinear" : "nearest");
                }
            }
            break;
        }
        case 28: {
            /* Origin to bbox center: shift verts, compensate the
             * location so the world geometry stays put. */
            hal_object_id sel = hal_scene_selected(app->scene);
            hal_object *obj = hal_scene_object(app->scene, sel);
            if (obj && obj->mesh) {
                hal_mesh *snap = hal_mesh_clone(obj->mesh);
                hal_vec3f off = hal_mesh_origin_to_center(obj->mesh);
                hal_object_set_location(app->scene, sel,
                                        hal_vec3f_add(obj->location, off));
                mesh_cmd *cmd = malloc(sizeof(*cmd));
                if (cmd && snap) {
                    cmd->id = sel;
                    cmd->snapshot = snap;
                    hal_undo_stack_push(app->undo, "Origin to Center",
                                        mesh_cmd_apply, mesh_cmd_revert,
                                        mesh_cmd_free, cmd);
                }
                set_status(app, "Origin moved to bbox center");
                app->dirty = true;
            }
            break;
        }
        case 29: {
            hal_object_id sel = hal_scene_selected(app->scene);
            hal_object *obj = hal_scene_object(app->scene, sel);
            if (obj) {
                hal_object_set_location(app->scene, sel,
                                        hal_vec3f_zero());
                set_status(app, "Origin at the world origin");
                app->dirty = true;
            }
            break;
        }
        case 31: {
            hal_object *obj =
                hal_scene_object(app->scene, app->edit_obj);
            if (obj && obj->mesh)
                hal_mesh_select_all(obj->mesh, true);
            break;
        }
        case 32: {
            hal_object *obj =
                hal_scene_object(app->scene, app->edit_obj);
            if (obj && obj->mesh)
                hal_mesh_select_all(obj->mesh, false);
            break;
        }
        case 38: {
            /* Generate LOD: a decimated clone as a new object. */
            hal_object *obj = hal_scene_object(
                app->scene, hal_scene_selected(app->scene));
            if (!obj || !obj->mesh) {
                set_status(app, "Generate LOD: select a mesh object");
                break;
            }
            hal_vec3f bmin, bmax;
            hal_mesh_bounds(obj->mesh, &bmin, &bmax);
            hal_f32 diag = hal_vec3f_distance(bmin, bmax);
            hal_mesh *lod = hal_mesh_clone(obj->mesh);
            if (lod && hal_mesh_decimate_cluster(lod, diag / 8.0f)) {
                char name[128];
                snprintf(name, sizeof(name), "%s_LOD", obj->name);
                hal_object_id id = hal_scene_object_add(
                    app->scene, name, HAL_OBJECT_MESH);
                hal_object_set_location(app->scene, id,
                                        obj->location);
                hal_object_set_mesh(app->scene, id, lod);
                rebuild_outliner(app);
                set_status(app, "LOD: %s (%zu verts -> %zu)", name,
                           obj->mesh->vert_count, lod->vert_count);
                app->dirty = true;
            } else {
                hal_mesh_destroy(lod);
                set_status(app, "Generate LOD: nothing to simplify");
            }
            break;
        }
        default:
            break;
        }
    } else if (menu == 2) { /* Render */
        switch (item) {
        case 0: /* Preview PNG */
        case 1: /* Turntable */
        case 2: { /* Thumbnail */
            app->render_kind = item;
            if (hal_ok(hal_dialog_save_file(
                    app->platform,
                    item == 0   ? "Render preview"
                    : item == 1 ? "Turntable prefix"
                                : "Thumbnail",
                    "*.png", "render.png", render_done, app,
                    app->window)))
                set_status(app, "Choose the output location...");
            break;
        }
        default:
            break;
        }
    } else if (menu == 3) { /* Animation */
        switch (item) {
        case 0: {
            char name[64];
            snprintf(name, sizeof(name), "Clip %zu",
                     hal_scene_clip_count(app->scene));
            int idx = hal_scene_clip_add(app->scene, name);
            if (idx >= 0) {
                app->active_clip = idx;
                app->play_time = 0.0f;
                set_status(app, "Created %s", name);
            }
            break;
        }
        case 1: { /* Insert Key: object TRS, or the posed bone. */
            if (app->active_clip < 0) {
                set_status(app, "Insert Key: create a clip first");
                break;
            }
            hal_clip *clip =
                hal_scene_clip(app->scene, app->active_clip);
            if (!clip)
                break;
            if (app->pose_mode) {
                hal_object *arm_obj =
                    hal_scene_object(app->scene, app->pose_obj);
                int bone = -1;
                if (arm_obj && arm_obj->armature)
                    for (size_t b = 0; b < arm_obj->armature->count; b++)
                        if (arm_obj->armature->bones[b].selected)
                            bone = (int)b;
                if (bone >= 0) {
                    hal_clip_key_rot(clip, app->pose_obj, bone,
                                     app->play_time,
                                     arm_obj->armature->bones[bone].pose,
                                     HAL_INTERP_SMOOTH);
                    set_status(app, "Bone key at %.2fs", app->play_time);
                } else {
                    set_status(app, "Insert Key: select a bone");
                }
            } else {
                hal_object_id sel = hal_scene_selected(app->scene);
                if (hal_clip_key_object(clip, app->scene, sel,
                                        app->play_time))
                    set_status(app, "Object keys at %.2fs",
                               app->play_time);
            }
            break;
        }
        case 2: /* Play / Pause */
            if (app->active_clip >= 0) {
                app->playing = !app->playing;
                set_status(app, app->playing ? "Playing" : "Paused");
            }
            break;
        case 3: {
            hal_clip *clip =
                hal_scene_clip(app->scene, app->active_clip);
            if (clip) {
                clip->loop = !clip->loop;
                set_status(app, "Loop %s", clip->loop ? "on" : "off");
            }
            break;
        }
        default:
            break;
        }
    } else if (menu == 4) { /* Add */
        if (item <= 5) {
            add_object(app, ADD_NAMES[item],
                       HAL_OBJECT_MESH, hal_vec3f_zero());
        } else if (item >= 6 && item <= 9) {
            add_preset(app, item - 6);
        } else if (item == 10) {
            hal_object_id id = hal_scene_object_add(app->scene,
                                                    "Armature",
                                                    HAL_OBJECT_ARMATURE);
            hal_object_set_armature(app->scene, id,
                                    hal_armature_default());
            hal_scene_select_only(app->scene, id);
            app->dirty = true;
            rebuild_outliner(app);
            set_status(app, "Added Armature (Tab to pose)");
        } else if (item == 11) {
            add_object(app, "Light", HAL_OBJECT_LIGHT,
                       hal_vec3f_make(2.0f, 3.0f, 2.0f));
        }
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
    hal_toggle_create(rail, "Paint", &app->paint_mode);

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

    /* Material section + the UV editor (bound on selection). */
    hal_widget *mat_panel = hal_panel_create(sidebar, "Material");
    hal_widget *mat_col = hal_column_create(mat_panel);
    hal_widget *col_row = hal_row_create(mat_col);
    hal_label_create(col_row, "Color");
    for (int i = 0; i < 3; i++) {
        app->mat_entry[i] = hal_entry_create(col_row, app->mat_buf[i],
                                             sizeof(app->mat_buf[i]));
        hal_entry_set_on_change(app->mat_entry[i], on_mat_changed, app);
    }
    hal_widget *mr_row = hal_row_create(mat_col);
    hal_label_create(mr_row, "Metal");
    app->metallic_slider =
        hal_slider_create(mr_row, 0.0f, 1.0f, &app->metallic_value);
    hal_widget *rg_row = hal_row_create(mat_col);
    hal_label_create(rg_row, "Rough");
    app->rough_slider =
        hal_slider_create(rg_row, 0.0f, 1.0f, &app->rough_value);

    hal_widget *uv_panel = hal_panel_create(sidebar, "UV Editor");
    app->uvview = hal_uvview_create(uv_panel);

    hal_widget *curve_panel = hal_panel_create(sidebar, "Curves");
    app->curves = hal_curves_create(curve_panel);
    /* Editing (drag keys, cycle easing, insert/delete); the clip is
     * re-bound every paint, the edit callback persists. */
    hal_curves_interactive(app->curves, NULL, curves_edit_notify,
                           app);

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
    hal_texture_cache_destroy(app->textures);
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

/* ---- Weight painting ----
 * The stroke: begin snapshots the mesh (undo base), stroke applies
 * the brush under the cursor, end pushes one undo command for the
 * whole stroke. The active bone is the one selected in pose mode
 * (kept in paint_bone), defaulting to 0. */
static void paint_stroke(hal_app *app, hal_mat4f vpm, hal_rectf vp,
                         hal_f32 px, hal_f32 py);
static void paint_begin(hal_app *app, hal_mat4f vpm, hal_rectf vp,
                        hal_f32 px, hal_f32 py, bool subtract) {
    hal_object *obj =
        hal_scene_object(app->scene, hal_scene_selected(app->scene));
    if (!obj || !obj->mesh || !obj->mesh->skin_weights ||
        obj->skin_armature == HAL_OBJECT_ID_NONE) {
        set_status(app, "Paint: select a mesh bound to an armature");
        return;
    }
    app->paint_obj = hal_scene_selected(app->scene);
    app->paint_snapshot = hal_mesh_clone(obj->mesh);
    app->painting = app->paint_snapshot != NULL;
    app->paint_subtract = subtract;
    if (app->painting)
        paint_stroke(app, vpm, vp, px, py);
}

static void paint_stroke(hal_app *app, hal_mat4f vpm, hal_rectf vp,
                         hal_f32 px, hal_f32 py) {
    hal_object *obj = hal_scene_object(app->scene, app->paint_obj);
    if (!obj || !obj->mesh)
        return;
    /* The brush point: the mouse ray hit on the mesh (nearest face),
     * or a point on a sphere around the target at the ray's closest
     * approach (misses still glide over the surface region). */
    hal_ray ray = hal_camera_ray(&app->camera, vpm, vp, px, py);
    hal_mat4f world = hal_object_world_matrix(app->scene, obj);
    hal_f32 t = 0.0f;
    int face = hal_pick_face(obj->mesh, world, ray, &t);
    hal_vec3f point;
    if (face >= 0) {
        point = hal_vec3f_add(ray.origin,
                              hal_vec3f_mul(ray.dir, t));
    } else {
        /* Closest point of the ray to the object's world center. */
        hal_vec3f c = hal_mat4f_transform_point(
            world, hal_vec3f_zero());
        hal_vec3f to_c = hal_vec3f_sub(c, ray.origin);
        hal_f32 d = hal_vec3f_dot(to_c, ray.dir);
        d = d > 0.0f ? d : 0.0f;
        point = hal_vec3f_add(ray.origin,
                              hal_vec3f_mul(ray.dir, d));
    }
    hal_f32 strength =
        (app->paint_subtract ? -0.25f : 0.25f);
    if (hal_mesh_paint_weight(obj->mesh, world, point, 0.4f,
                              app->paint_bone, strength)) {
        app->dirty = true;
        hal_autosave_mark_dirty(app->autosave);
    }
}

static void paint_end(hal_app *app) {
    app->painting = false;
    hal_object *obj = hal_scene_object(app->scene, app->paint_obj);
    if (!obj || !app->paint_snapshot) {
        hal_mesh_destroy(app->paint_snapshot);
        app->paint_snapshot = NULL;
        return;
    }
    mesh_cmd *cmd = malloc(sizeof(*cmd));
    if (cmd) {
        cmd->id = app->paint_obj;
        /* The snapshot is the PRE-stroke mesh; the object holds the
         * post-stroke mesh — the swap-command contract. */
        cmd->snapshot = app->paint_snapshot;
        app->paint_snapshot = NULL;
        hal_undo_stack_push(app->undo, "Weight Paint", mesh_cmd_apply,
                            mesh_cmd_revert, mesh_cmd_free, cmd);
    } else {
        hal_mesh_destroy(app->paint_snapshot);
        app->paint_snapshot = NULL;
    }
}

/* Picks a bone of the posed armature by screen-space distance to the
 * projected head-tail segment. Returns the bone index or -1. */
static int pick_bone(hal_app *app, hal_object *arm_obj, hal_mat4f vpm,
                     hal_rectf vp, hal_f32 px, hal_f32 py) {
    hal_armature *arm = arm_obj->armature;
    hal_bone_pose *poses = malloc(arm->count * sizeof(*poses));
    if (!poses)
        return -1;
    hal_armature_pose(arm, poses);
    hal_mat4f world = hal_object_world_matrix(app->scene, arm_obj);
    hal_mat4f full = hal_mat4f_mul(vpm, world);
    int best = -1;
    hal_f32 best_d = 10.0f;
    for (size_t b = 0; b < arm->count; b++) {
        hal_f32 w0, w1;
        hal_vec3f s0 = hal_camera_project(&app->camera, full,
                                          poses[b].head, vp, &w0);
        hal_vec3f s1 = hal_camera_project(&app->camera, full,
                                          poses[b].tail, vp, &w1);
        if (w0 > 0.0f && w1 > 0.0f) {
            hal_f32 d = seg_dist((hal_vec2f){ s0.x, s0.y },
                                 (hal_vec2f){ s1.x, s1.y }, px, py);
            if (d < best_d) {
                best_d = d;
                best = (int)b;
            }
        }
    }
    free(poses);
    return best;
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
    if (app->modal_op == 3) {
        /* Bone rotation: about world Y, applied to the bone's pose. */
        hal_object *arm_obj =
            hal_scene_object(app->scene, app->pose_obj);
        hal_armature *arm = arm_obj ? arm_obj->armature : NULL;
        if (!arm || app->bone_modal < 0 ||
            (size_t)app->bone_modal >= arm->count) {
            app->modal_op = 0;
            return true;
        }
        hal_bone *bone = &arm->bones[app->bone_modal];
        if (event->type == HAL_EVENT_POINTER_MOTION) {
            app->modal_amount += px - app->drag_last.x;
            app->drag_last = (hal_pointf){ px, py };
            bone->pose = hal_quatf_mul(
                hal_quatf_from_axis_angle(
                    hal_vec3f_axis_y(), app->modal_amount * 0.01f),
                app->bone_modal_orig);
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN) {
            app->modal_op = 0; /* accept (pose edits undo later) */
            set_status(app, "Bone rotated");
            return true;
        }
        if (event->type == HAL_EVENT_KEY_DOWN &&
            event->key.scancode == HAL_KEY_ESC) {
            bone->pose = app->bone_modal_orig;
            app->modal_op = 0;
            return true;
        }
        return true;
    }
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

    /* --- Weight painting stroke --- */
    if (app->painting && event->type == HAL_EVENT_POINTER_MOTION) {
        paint_stroke(app, vpm, vp, px, py);
        return true;
    }
    if (app->painting &&
        event->type == HAL_EVENT_POINTER_BUTTON_UP) {
        paint_end(app);
        return true;
    }

    /* --- Timeline scrubbing (before camera handling) --- */
    {
        hal_rectf tr = app->timeline_rect;
        bool in_tl = tr.height > 0.0f && px >= tr.x && py >= tr.y &&
                     px < tr.x + tr.width && py < tr.y + tr.height;
        hal_clip *clip = hal_scene_clip(app->scene, app->active_clip);
        if (app->timeline_drag && clip &&
            event->type == HAL_EVENT_POINTER_MOTION) {
            hal_f32 span = clip->end - clip->start;
            if (span > 1e-4f) {
                app->play_time =
                    clip->start + (px - tr.x) / tr.width * span;
                if (app->play_time < clip->start)
                    app->play_time = clip->start;
                if (app->play_time > clip->end)
                    app->play_time = clip->end;
                hal_clip_sample(clip, app->scene, app->play_time);
            }
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_BUTTON_DOWN && in_tl &&
            clip) {
            app->timeline_drag = true;
            app->playing = false;
            hal_f32 span = clip->end - clip->start;
            if (span > 1e-4f)
                app->play_time =
                    clip->start + (px - tr.x) / tr.width * span;
            hal_clip_sample(clip, app->scene, app->play_time);
            return true;
        }
        if (event->type == HAL_EVENT_POINTER_BUTTON_UP &&
            app->timeline_drag) {
            app->timeline_drag = false;
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
            if (app->paint_mode &&
                event->pointer_button.button == HAL_BUTTON_LEFT) {
                paint_begin(app, vpm, vp, px, py,
                            (mods & HAL_MOD_SHIFT) != 0);
                return true;
            }
            if (app->pose_mode) {
                hal_object *arm_obj =
                    hal_scene_object(app->scene, app->pose_obj);
                if (arm_obj && arm_obj->armature) {
                    int bone = pick_bone(app, arm_obj, vpm, vp, px, py);
                    for (size_t b = 0; b < arm_obj->armature->count; b++)
                        arm_obj->armature->bones[b].selected =
                            (int)b == bone;
                    if (bone >= 0) {
                        app->paint_bone = bone;
                        set_status(app, "Bone: %s",
                                   arm_obj->armature->bones[bone].name);
                    }
                }
                return true;
            }
            if (app->paint_mode) {
                paint_begin(app, vpm, vp, px, py,
                            (mods & HAL_MOD_SHIFT) != 0);
                return true;
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
        if (key_plain == 15) { /* Tab: edit / pose mode */
            if (app->pose_mode) {
                app->pose_mode = false;
                set_status(app, "Object mode");
                return true;
            }
            hal_object *sel0 =
                hal_scene_object(app->scene,
                                 hal_scene_selected(app->scene));
            if (sel0 && sel0->armature) {
                app->pose_mode = true;
                app->pose_obj = sel0->id;
                app->edit_mode = false;
                set_status(app, "Pose mode: %s (click bones, R, IK)",
                           sel0->name);
                return true;
            }
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
        if (app->pose_mode && key_plain == 19) { /* R: bone rotate */
            hal_object *arm_obj =
                hal_scene_object(app->scene, app->pose_obj);
            int bone = -1;
            if (arm_obj && arm_obj->armature)
                for (size_t b = 0; b < arm_obj->armature->count; b++)
                    if (arm_obj->armature->bones[b].selected)
                        bone = (int)b;
            if (bone >= 0) {
                app->bone_modal = bone;
                app->bone_modal_orig = arm_obj->armature->bones[bone].pose;
                app->modal_op = 3; /* bone rotate */
                app->modal_amount = 0.0f;
                app->drag_last = app->last_pointer;
                set_status(app, "Rotate bone: move, click to accept");
            }
            return true;
        }
        if (!app->edit_mode && !app->pose_mode) {
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

        /* Animation keys (any mode). */
        if (key_plain == 23 /* I */ &&
            !(event->key.modifiers & HAL_MOD_ALT)) {
            menu_action(NULL, 2, 1, app);
            return true;
        }
        if (key_plain == 57) { /* Space */
            menu_action(NULL, 2, 2, app);
            return true;
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
                menu_action(NULL, 1, 32, app);
                set_status(app, "Deselected");
            } else {
                menu_action(NULL, 1, 31, app);
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
    bool timeline = app->active_clip >= 0;
    hal_f32 timeline_h = timeline ? SHELL_TIMELINE_H : 0.0f;
    app->timeline_rect = (hal_rectf){
        0, h - SHELL_STATUSBAR_H - timeline_h, w, timeline_h
    };
    hal_f32 body_h =
        h - mbar.height - SHELL_STATUSBAR_H - timeline_h;
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
    return (hal_rectf){
        SHELL_TOOLBAR_W, mbar.height, vw,
        h - mbar.height - SHELL_STATUSBAR_H -
            (app && app->active_clip >= 0 ? SHELL_TIMELINE_H : 0.0f)
    };
}

/* The timeline: ruler, channel rows with key dots, the playhead. */
static void paint_timeline(hal_app *app, hal_canvas *canvas) {
    const hal_theme *t = hal_ui_theme(app->ui);
    hal_clip *clip = hal_scene_clip(app->scene, app->active_clip);
    if (!clip)
        return;
    hal_rectf r = app->timeline_rect;
    if (r.height <= 0.0f)
        return;

    hal_canvas_fill_rect(canvas, r, t->colors[HAL_THEME_PANEL]);

    hal_f32 span = clip->end - clip->start;
    if (span < 1e-3f)
        span = 1.0f;
#define TL_X(time) (r.x + ((time) - clip->start) / span * r.width)

    /* Ruler ticks every half second (at least 6 ticks). */
    hal_f32 step = 0.5f;
    while (span / step < 4.0f && step > 0.01f)
        step *= 0.5f;
    hal_font *font = hal_ui_font(app->ui, t->font_size_small);
    for (hal_f32 time = clip->start; time <= clip->end + 1e-4f;
         time += step) {
        hal_f32 x = TL_X(time);
        hal_canvas_draw_line(canvas, x, r.y, x, r.y + r.height,
                             t->colors[HAL_THEME_BORDER]);
        if (font) {
            char label[16];
            snprintf(label, sizeof(label), "%.1f", (double)time);
            hal_canvas_draw_text(canvas, font, label, strlen(label),
                                 x + 2.0f, r.y + 12.0f,
                                 t->colors[HAL_THEME_TEXT_DIM]);
        }
    }

    /* One row per channel: key dots; a bone channel's row carries a
     * small marker. */
    hal_f32 row_h = 10.0f;
    hal_f32 row_y = r.y + 18.0f;
    for (size_t c = 0; c < clip->count; c++) {
        hal_anim_channel *ch = &clip->channels[c];
        for (size_t k = 0; k < ch->count; k++) {
            hal_f32 x = TL_X(ch->keys[k].time);
            hal_color col = ch->type == HAL_CH_BONE
                                ? t->colors[HAL_THEME_SUCCESS]
                                : ch->type == HAL_CH_ROT
                                      ? t->colors[HAL_THEME_WARNING]
                                      : t->colors[HAL_THEME_ACCENT];
            hal_canvas_fill_circle(canvas, x, row_y + row_h * 0.5f,
                                   3.0f, col);
        }
        row_y += row_h;
        if (row_y > r.y + r.height - 6.0f)
            break; /* more channels than rows: later rows not drawn */
    }

    /* The playhead. */
    hal_f32 px = TL_X(app->play_time);
    hal_canvas_draw_line(canvas, px, r.y, px, r.y + r.height,
                         t->colors[HAL_THEME_TEXT]);
    if (font) {
        char label[96];
        snprintf(label, sizeof(label), "%s %.2fs%s", clip->name,
                 (double)app->play_time,
                 app->playing ? "  >" : "  ||");
        hal_canvas_draw_text(canvas, font, label, strlen(label),
                             r.x + 6.0f, r.y + r.height - 4.0f,
                             t->colors[HAL_THEME_TEXT]);
    }
#undef TL_X
}

static void paint_viewport(hal_app *app, hal_canvas *canvas) {
    const hal_theme *t = hal_ui_theme(app->ui);
    hal_rectf vp = hal_app_viewport_rect(app);

    /* Theme-driven render options; the wireframe toggle rides along. */
    hal_render_opts opts = app->render_opts;
    opts.textures = app->textures;
    opts.bg = t->colors[HAL_THEME_VIEWPORT_BG];
    opts.grid = t->colors[HAL_THEME_VIEWPORT_GRID];
    opts.object = hal_color_rgb8(0xb9, 0xb2, 0xe0);
    opts.shading = app->wireframe ? HAL_SHADING_WIREFRAME
                                  : HAL_SHADING_SOLID;

    hal_render_view(&app->raster, app->scene, &app->camera, canvas, vp,
                    &opts);
}

void hal_app_tick(hal_app *app, double now_seconds) {
    if (!app || !app->playing)
        return;
    hal_clip *clip = hal_scene_clip(app->scene, app->active_clip);
    if (!clip) {
        app->playing = false;
        return;
    }
    static double last = -1.0;
    double dt = last >= 0.0 ? now_seconds - last : 0.0;
    last = now_seconds;
    if (dt > 0.25)
        dt = 0.25;
    if (dt <= 0.0)
        return;

    app->play_time += (hal_f32)dt;
    if (app->play_time > clip->end) {
        if (clip->loop)
            app->play_time =
                clip->start +
                (hal_f32)fmod((double)(app->play_time - clip->start),
                              (double)(clip->end - clip->start));
        else {
            app->play_time = clip->end;
            app->playing = false;
        }
    }
    hal_clip_sample(clip, app->scene, app->play_time);
    app->dirty = true;
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

    /* The timeline strip (when a clip is active). */
    paint_timeline(app, &canvas);
    hal_curves_bind(
        app->curves,
        hal_scene_clip(app->scene, app->active_clip));
    hal_curves_set_playhead(app->curves, app->play_time);

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
