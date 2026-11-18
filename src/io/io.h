/*
 * io.h — Halcyon import/export
 *
 * Pure functions over scene data, no UI. Formats:
 *   - glTF 2.0 binary (.glb): meshes (triangulated), normals,
 *     transforms (TRS, hierarchy flattened to world), the first-class
 *     target. Materials/UVs arrive with M6.
 *   - OBJ: one object per mesh, global vertex index space.
 *   - STL: binary, all visible meshes baked to world space (STL has
 *     no object structure).
 *   - OBJ import (objects, verts, polygon faces).
 *   - glTF import (.glb): what the exporter writes plus the common
 *     subset (FLOAT attributes, UNSIGNED_* indices, node chains).
 */

#ifndef HAL_IO_H
#define HAL_IO_H

#include "../core/scene.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* glTF export options: unit conversion (scene meters -> target
 * units; 1.0 = meters, 100.0 = centimeters) and the target's up
 * axis. glTF data must be Y-up right-handed, so z_up bakes a +90
 * degree X rotation into every ROOT node (children ride along; skin
 * inverse binds stay consistent because the conversion is uniform).
 */
typedef struct hal_gltf_export_opts {
    hal_f32 unit_scale; /* default 1.0                     */
    bool z_up;          /* default false (Y-up)            */
} hal_gltf_export_opts;

hal_gltf_export_opts hal_gltf_export_opts_default(void);

hal_result hal_io_gltf_export(hal_scene *scene, const char *path,
                              char *err, size_t err_cap);
hal_result hal_io_gltf_export_ex(hal_scene *scene, const char *path,
                                 const hal_gltf_export_opts *opts,
                                 char *err, size_t err_cap);
hal_result hal_io_gltf_import(const char *path, hal_scene **out_scene,
                              char *err, size_t err_cap);

hal_result hal_io_obj_export(const hal_scene *scene, const char *path,
                             char *err, size_t err_cap);
hal_result hal_io_obj_import(const char *path, hal_scene **out_scene,
                             char *err, size_t err_cap);

hal_result hal_io_stl_export(hal_scene *scene, const char *path,
                             char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* HAL_IO_H */
