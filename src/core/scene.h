/*
 * scene.h — Halcyon's scene graph
 *
 * A scene owns a flat array of objects with stable ids; parenting is a
 * per-object parent id (0 = world). Object transforms are stored as
 * location + rotation (unit quaternion) + scale, the glTF triple
 * (decision D-005); world matrices are cached and recomputed lazily
 * with subtree dirty propagation.
 *
 * M1 scope: the graph itself — objects, transforms, parenting, world
 * matrices, selection and visibility. Mesh payloads (M4), armatures
 * (M6), animation channels (M7) attach to these objects later without
 * changing the graph's shape.
 */

#ifndef HAL_SCENE_H
#define HAL_SCENE_H

#include "../platform/types.h"
#include "math.h"
#include "material.h"
#include "mesh.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HAL_OBJECT_ID_DEFINED
#define HAL_OBJECT_ID_DEFINED
typedef hal_u32 hal_object_id;
#endif
#define HAL_OBJECT_ID_NONE 0u

typedef enum hal_object_type {
    HAL_OBJECT_MESH = 1,     /* M4 attaches the mesh payload          */
    HAL_OBJECT_ARMATURE = 2, /* M6                                   */
    HAL_OBJECT_CAMERA = 3,   /* viewport cameras, later              */
    HAL_OBJECT_LIGHT = 4,    /* M3's fixed headlight first           */
} hal_object_type;

typedef struct hal_object {
    hal_object_id id;
    char *name;              /* owned; never NULL                     */
    hal_object_type type;
    hal_mesh *mesh;          /* owned payload; NULL unless MESH       */
    int material;            /* scene material index, -1 = none       */

    /* Local transform (the glTF TRS triple). */
    hal_vec3f location;
    hal_quatf rotation;      /* kept normalized by the setters        */
    hal_vec3f scale;

    hal_object_id parent;    /* HAL_OBJECT_ID_NONE = no parent        */

    bool visible;
    bool selected;
    bool smooth_shading;   /* per-vertex normals when set           */

    /* World matrix cache — read through hal_object_world_matrix. */
    hal_mat4f world;
    bool world_dirty;
} hal_object;

typedef struct hal_scene {
    char *name;              /* owned; never NULL ("Scene")           */
    hal_object *objects;     /* dense array, holes never left         */
    size_t count;
    size_t capacity;
    hal_object_id next_id;
    hal_material *materials; /* owned material list                   */
    size_t material_count;
    char **images;           /* owned image file paths (textures)     */
    size_t image_count;
} hal_scene;

/* ---- Scene lifecycle ---- */

hal_scene *hal_scene_create(const char *name);
void hal_scene_destroy(hal_scene *scene);

/* ---- Objects ---- */

/* Adds an object of `type`, named `name` (copied; NULL = "Object"),
 * with an identity transform, no parent, visible, unselected. Returns
 * its id (never 0 — ids are unique for the scene's lifetime). */
hal_object_id hal_scene_object_add(hal_scene *scene, const char *name,
                                   hal_object_type type);

/* Removes an object; its children are re-parented to the object's own
 * parent (they never dangle). Unknown ids are a no-op. */
void hal_scene_object_remove(hal_scene *scene, hal_object_id id);

/* Looks an object up by id; NULL when absent. The pointer stays valid
 * until the next add/remove on the scene. Not const: the returned
 * object is mutable (world-matrix caching also writes through it). */
hal_object *hal_scene_object(hal_scene *scene, hal_object_id id);

size_t hal_scene_object_count(const hal_scene *scene);

/* Index-based access for iteration (0 .. count-1); NULL out of range. */
hal_object *hal_scene_object_at(hal_scene *scene, size_t index);

/* Sets an object's name (copied; NULL or empty = "Object"). */
void hal_object_set_name(hal_scene *scene, hal_object_id id,
                         const char *name);

/* Attaches a mesh to a MESH object (the object takes ownership; the
 * previous mesh, if any, is destroyed). NULL detaches. */
void hal_object_set_mesh(hal_scene *scene, hal_object_id id,
                         hal_mesh *mesh);

/* ---- Parenting ----
 * The parent must exist (or be HAL_OBJECT_ID_NONE). Reparenting to a
 * descendant of the object (a cycle) is refused: returns false, tree
 * unchanged. Marks world matrices dirty as needed. */
bool hal_object_set_parent(hal_scene *scene, hal_object_id id,
                           hal_object_id parent);

/* ---- Transforms ----
 * Setters keep the quaternion normalized and mark the subtree's world
 * matrices dirty. */

void hal_object_set_location(hal_scene *scene, hal_object_id id,
                             hal_vec3f location);
void hal_object_set_rotation(hal_scene *scene, hal_object_id id,
                             hal_quatf rotation);
void hal_object_set_rotation_euler(hal_scene *scene, hal_object_id id,
                                   hal_vec3f euler_xyz_radians);
void hal_object_set_scale(hal_scene *scene, hal_object_id id,
                          hal_vec3f scale);

/* The local T*R*S matrix. */
hal_mat4f hal_object_local_matrix(const hal_object *object);

/* The cached world matrix, recomputed first if the chain is dirty
 * (hence the non-const object: the cache is written through it). */
hal_mat4f hal_object_world_matrix(hal_scene *scene, hal_object *object);

/* ---- Selection / visibility ---- */

void hal_scene_select_only(hal_scene *scene, hal_object_id id);
/* Returns the first selected object's id (0 when none). */
hal_object_id hal_scene_selected(const hal_scene *scene);

void hal_object_set_visible(hal_scene *scene, hal_object_id id, bool visible);
void hal_object_set_smooth_shading(hal_scene *scene, hal_object_id id,
                                   bool smooth);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SCENE_H */
