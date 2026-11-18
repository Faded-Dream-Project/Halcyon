/*
 * scene.c — Halcyon scene graph implementation
 */

#include "scene.h"

#include "armature.h"

#include <stdlib.h>
#include <string.h>

/* ---- Small string helper (owned copies, never NULL) ---- */

static char *hal_strdup_or(const char *src, const char *fallback) {
    const char *s = (src && src[0]) ? src : fallback;
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out)
        memcpy(out, s, n);
    return out;
}

/* ---- Scene lifecycle ---- */

hal_scene *hal_scene_create(const char *name) {
    hal_scene *scene = calloc(1, sizeof(*scene));
    if (!scene)
        return NULL;
    scene->name = hal_strdup_or(name, "Scene");
    if (!scene->name) {
        free(scene);
        return NULL;
    }
    scene->next_id = 1; /* 0 is HAL_OBJECT_ID_NONE */
    scene->materials = NULL;
    scene->material_count = 0;
    scene->images = NULL;
    scene->image_count = 0;
    return scene;
}

void hal_scene_destroy(hal_scene *scene) {
    if (!scene)
        return;
    for (size_t i = 0; i < scene->count; i++) {
        hal_mesh_destroy(scene->objects[i].mesh);
        hal_armature_destroy(scene->objects[i].armature);
        free(scene->objects[i].name);
    }
    free(scene->objects);
    free(scene->materials);
    for (size_t i = 0; i < scene->image_count; i++)
        free(scene->images[i]);
    free(scene->images);
    free(scene->name);
    free(scene);
}

/* ---- Objects ---- */

static hal_object *scene_grow(hal_scene *scene) {
    if (scene->count == scene->capacity) {
        size_t cap = scene->capacity ? scene->capacity * 2 : 16;
        hal_object *grown = realloc(scene->objects, cap * sizeof(*grown));
        if (!grown)
            return NULL;
        scene->objects = grown;
        scene->capacity = cap;
    }
    return &scene->objects[scene->count];
}

/* Marks the subtree rooted at `object` (and every object below it)
 * dirty. Linear scan per level — scene sizes here are hundreds of
 * objects, and correctness beats a child-sibling index until profiling
 * says otherwise. */
static void mark_subtree_dirty(hal_scene *scene, hal_object *object) {
    object->world_dirty = true;
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].parent == object->id)
            mark_subtree_dirty(scene, &scene->objects[i]);
}

hal_object_id hal_scene_object_add(hal_scene *scene, const char *name,
                                   hal_object_type type) {
    if (!scene)
        return HAL_OBJECT_ID_NONE;

    hal_object *slot = scene_grow(scene);
    if (!slot)
        return HAL_OBJECT_ID_NONE;

    char *owned_name = hal_strdup_or(name, "Object");
    if (!owned_name)
        return HAL_OBJECT_ID_NONE;

    slot->id = scene->next_id++;
    slot->name = owned_name;
    slot->type = type;
    slot->mesh = NULL;
    slot->armature = NULL;
    slot->skin_armature = HAL_OBJECT_ID_NONE;
    slot->material = -1;
    slot->location = hal_vec3f_zero();
    slot->rotation = hal_quatf_identity();
    slot->scale = hal_vec3f_one();
    slot->parent = HAL_OBJECT_ID_NONE;
    slot->visible = true;
    slot->selected = false;
    slot->smooth_shading = false;
    slot->world = hal_mat4f_identity();
    slot->world_dirty = true;

    scene->count++;
    return slot->id;
}

static size_t object_index(const hal_scene *scene, hal_object_id id) {
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].id == id)
            return i;
    return (size_t)-1;
}

void hal_scene_object_remove(hal_scene *scene, hal_object_id id) {
    if (!scene)
        return;
    size_t idx = object_index(scene, id);
    if (idx == (size_t)-1)
        return;

    /* Children re-parent to the removed object's parent. */
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].parent == id)
            scene->objects[i].parent = scene->objects[idx].parent;

    hal_mesh_destroy(scene->objects[idx].mesh);
    hal_armature_destroy(scene->objects[idx].armature);
    free(scene->objects[idx].name);
    memmove(&scene->objects[idx], &scene->objects[idx + 1],
            (scene->count - idx - 1) * sizeof(hal_object));
    scene->count--;

    /* Reparented children need fresh world matrices; after the memmove
     * we can no longer tell exactly which objects they were, so the
     * honest rule is: a removal dirties the whole scene. Correct and
     * one linear pass; the next world-matrix query re-optimizes. */
    for (size_t i = 0; i < scene->count; i++)
        scene->objects[i].world_dirty = true;
}

hal_object *hal_scene_object(hal_scene *scene, hal_object_id id) {
    if (!scene)
        return NULL;
    size_t idx = object_index(scene, id);
    if (idx == (size_t)-1)
        return NULL;
    return &scene->objects[idx];
}

size_t hal_scene_object_count(const hal_scene *scene) {
    return scene ? scene->count : 0;
}

hal_object *hal_scene_object_at(hal_scene *scene, size_t index) {
    if (!scene || index >= scene->count)
        return NULL;
    return &scene->objects[index];
}

void hal_object_set_name(hal_scene *scene, hal_object_id id,
                         const char *name) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    char *owned = hal_strdup_or(name, "Object");
    if (!owned)
        return;
    free(obj->name);
    obj->name = owned;
}

void hal_object_set_mesh(hal_scene *scene, hal_object_id id,
                         hal_mesh *mesh) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    hal_mesh_destroy(obj->mesh);
    obj->mesh = mesh;
}

void hal_object_set_armature(hal_scene *scene, hal_object_id id,
                             hal_armature *armature) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    hal_armature_destroy(obj->armature);
    obj->armature = armature;
}

void hal_object_set_skin(hal_scene *scene, hal_object_id mesh_obj,
                         hal_object_id armature_obj) {
    hal_object *obj = hal_scene_object(scene, mesh_obj);
    if (!obj)
        return;
    obj->skin_armature = armature_obj;
}

/* ---- Parenting ---- */

static bool is_descendant_of(hal_scene *scene, hal_object_id id,
                             hal_object_id ancestor) {
    hal_object *obj = hal_scene_object(scene, id);
    while (obj && obj->parent != HAL_OBJECT_ID_NONE) {
        if (obj->parent == ancestor)
            return true;
        obj = hal_scene_object(scene, obj->parent);
    }
    return false;
}

bool hal_object_set_parent(hal_scene *scene, hal_object_id id,
                           hal_object_id parent) {
    if (!scene)
        return false;
    if (id == HAL_OBJECT_ID_NONE)
        return false;
    if (parent != HAL_OBJECT_ID_NONE) {
        if (!hal_scene_object(scene, parent))
            return false;
        /* No cycles: the parent must not be the object itself or a
         * descendant of it. */
        if (parent == id || is_descendant_of(scene, parent, id))
            return false;
    }

    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return false;
    obj->parent = parent;
    mark_subtree_dirty(scene, obj);
    return true;
}

/* ---- Transforms ---- */

void hal_object_set_location(hal_scene *scene, hal_object_id id,
                             hal_vec3f location) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    obj->location = location;
    mark_subtree_dirty(scene, obj);
}

void hal_object_set_rotation(hal_scene *scene, hal_object_id id,
                             hal_quatf rotation) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    obj->rotation = hal_quatf_normalize(rotation);
    mark_subtree_dirty(scene, obj);
}

void hal_object_set_rotation_euler(hal_scene *scene, hal_object_id id,
                                   hal_vec3f euler_xyz_radians) {
    hal_object_set_rotation(
        scene, id, hal_quatf_from_euler_xyz(euler_xyz_radians));
}

void hal_object_set_scale(hal_scene *scene, hal_object_id id,
                          hal_vec3f scale) {
    hal_object *obj = hal_scene_object(scene, id);
    if (!obj)
        return;
    obj->scale = scale;
    mark_subtree_dirty(scene, obj);
}

hal_mat4f hal_object_local_matrix(const hal_object *object) {
    if (!object)
        return hal_mat4f_identity();
    return hal_mat4f_from_trs(object->location, object->rotation,
                              object->scale);
}

hal_mat4f hal_object_world_matrix(hal_scene *scene, hal_object *obj) {
    if (!scene || !obj)
        return hal_mat4f_identity();

    if (!obj->world_dirty)
        return obj->world;

    /* Recompute the whole chain root-first (parents must be fresh
     * before children). The recursion depth is bounded by the tree
     * depth, which the cycle-free parenting rule guarantees. */
    if (obj->parent != HAL_OBJECT_ID_NONE) {
        hal_object *parent = hal_scene_object(scene, obj->parent);
        if (parent) {
            hal_mat4f pw = hal_object_world_matrix(scene, parent);
            obj->world = hal_mat4f_mul(pw, hal_object_local_matrix(obj));
            obj->world_dirty = false;
            return obj->world;
        }
    }

    obj->world = hal_object_local_matrix(obj);
    obj->world_dirty = false;
    return obj->world;
}

/* ---- Selection / visibility ---- */

void hal_scene_select_only(hal_scene *scene, hal_object_id id) {
    if (!scene)
        return;
    for (size_t i = 0; i < scene->count; i++)
        scene->objects[i].selected = (scene->objects[i].id == id);
}

hal_object_id hal_scene_selected(const hal_scene *scene) {
    if (!scene)
        return HAL_OBJECT_ID_NONE;
    for (size_t i = 0; i < scene->count; i++)
        if (scene->objects[i].selected)
            return scene->objects[i].id;
    return HAL_OBJECT_ID_NONE;
}

void hal_object_set_visible(hal_scene *scene, hal_object_id id, bool visible) {
    hal_object *obj = hal_scene_object(scene, id);
    if (obj)
        obj->visible = visible;
}

void hal_object_set_smooth_shading(hal_scene *scene, hal_object_id id,
                                   bool smooth) {
    hal_object *obj = hal_scene_object(scene, id);
    if (obj)
        obj->smooth_shading = smooth;
}
