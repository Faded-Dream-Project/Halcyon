/*
 * material.c — materials, scene image lists, object material slots
 */

#include "material.h"

#include "scene.h"

#include <stdlib.h>
#include <string.h>

hal_material hal_material_default(void) {
    hal_material m;
    memset(&m, 0, sizeof(m));
    strcpy(m.name, "Material");
    m.base_color = hal_color_rgb8(0xB9, 0xB2, 0xE0);
    m.metallic = 0.0f;
    m.roughness = 0.5f;
    m.emissive = hal_vec3f_zero();
    m.emissive_strength = 0.0f;
    m.texture = -1;
    m.filter = 1; /* bilinear */
    return m;
}

int hal_scene_material_add(hal_scene *scene, const hal_material *mat) {
    if (!scene || !mat)
        return -1;
    /* The scene's material array piggybacks on the image list's
     * growth helpers? No — keep it explicit and simple. */
    hal_material *grown =
        realloc(scene->materials,
                (scene->material_count + 1) * sizeof(*grown));
    if (!grown)
        return -1;
    scene->materials = grown;
    scene->materials[scene->material_count] = *mat;
    return (int)scene->material_count++;
}

size_t hal_scene_material_count(const hal_scene *scene) {
    return scene ? scene->material_count : 0;
}

hal_material *hal_scene_material(hal_scene *scene, int index) {
    if (!scene || index < 0 || (size_t)index >= scene->material_count)
        return NULL;
    return &scene->materials[index];
}

int hal_scene_image_add(hal_scene *scene, const char *path) {
    if (!scene || !path || !path[0])
        return -1;
    char **grown =
        realloc(scene->images,
                (scene->image_count + 1) * sizeof(*grown));
    if (!grown)
        return -1;
    scene->images = grown;
    char *copy = malloc(strlen(path) + 1);
    if (!copy)
        return -1;
    strcpy(copy, path);
    scene->images[scene->image_count] = copy;
    return (int)scene->image_count++;
}

size_t hal_scene_image_count(const hal_scene *scene) {
    return scene ? scene->image_count : 0;
}

const char *hal_scene_image(const hal_scene *scene, int index) {
    if (!scene || index < 0 || (size_t)index >= scene->image_count)
        return NULL;
    return scene->images[index];
}

void hal_object_set_material(hal_scene *scene, hal_object_id id,
                             int material) {
    hal_object *obj = hal_scene_object(scene, id);
    if (obj)
        obj->material = material;
}

int hal_object_material(const hal_object *obj) {
    return obj ? obj->material : -1;
}
