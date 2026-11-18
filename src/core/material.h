/*
 * material.h — PBR-style materials and texture slots
 *
 * A scene owns a flat material list; mesh objects reference one by
 * index (-1 = none). The model is glTF's metallic-roughness: base
 * color, metallic, roughness, emissive color + strength. A material
 * may reference one image texture (a file path owned by the scene's
 * image list) used as the base-color map.
 *
 * Viewport scope (honest): the viewport renders base color + a
 * roughness-flavored highlight and emissive glow; texture SAMPLING in
 * the 3D view arrives later — textures are authored, shown in the UV
 * editor, and exported today.
 */

#ifndef HAL_MATERIAL_H
#define HAL_MATERIAL_H

#include "../platform/types.h"
#include "math.h"

/* scene.h includes this file first; its types are opaque here. */
struct hal_scene;
struct hal_object;

#ifndef HAL_OBJECT_ID_DEFINED
#define HAL_OBJECT_ID_DEFINED
typedef unsigned hal_object_id;
#endif

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_material {
    char name[64];
    hal_color base_color;    /* straight RGBA                  */
    hal_f32 metallic;        /* 0..1                           */
    hal_f32 roughness;       /* 0..1                           */
    hal_vec3f emissive;      /* linear color                   */
    hal_f32 emissive_strength;
    int texture;             /* scene image index, -1 = none   */
} hal_material;

/* A neutral default (white, non-metal, half-rough). */
hal_material hal_material_default(void);

/* Scene-owned material and image lists. */
int hal_scene_material_add(struct hal_scene *scene, const hal_material *mat);
size_t hal_scene_material_count(const struct hal_scene *scene);
hal_material *hal_scene_material(struct hal_scene *scene, int index);

int hal_scene_image_add(struct hal_scene *scene, const char *path);
size_t hal_scene_image_count(const struct hal_scene *scene);
const char *hal_scene_image(const struct hal_scene *scene, int index);

/* The object's material index (-1 none). */
void hal_object_set_material(struct hal_scene *scene, hal_object_id id,
                             int material);
struct hal_object;
int hal_object_material(const struct hal_object *obj);

#ifdef __cplusplus
}
#endif

#endif /* HAL_MATERIAL_H */
