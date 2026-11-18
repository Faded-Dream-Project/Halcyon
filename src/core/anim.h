/*
 * anim.h — animation clips, channels, keyframes, sampling
 *
 * A clip owns channels; a channel targets one property of one object
 * (location X/Y/Z, rotation as a quaternion, scale X/Y/Z, or a
 * bone's local pose rotation). Keys are sorted by time; sampling
 * interpolates between neighbors with per-key easing (linear,
 * smooth ease-in-out, step) — quaternion channels slerp.
 *
 * The editor inserts keys from the current state (I) and plays clips
 * over the scene (hal_clip_sample writes transforms/bone poses).
 */

#ifndef HAL_ANIM_H
#define HAL_ANIM_H

#include "../platform/types.h"
#include "armature.h"
#include "material.h" /* hal_object_id (guarded typedef) + math */

/* scene.h includes this file; the scene is opaque here. */
struct hal_scene;

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hal_interp {
    HAL_INTERP_LINEAR = 0,
    HAL_INTERP_SMOOTH = 1, /* ease in-out (smoothstep on t) */
    HAL_INTERP_STEP = 2,
} hal_interp;

typedef enum hal_channel_type {
    HAL_CH_LOC_X = 0,
    HAL_CH_LOC_Y,
    HAL_CH_LOC_Z,
    HAL_CH_ROT,    /* quaternion                       */
    HAL_CH_SCALE_X,
    HAL_CH_SCALE_Y,
    HAL_CH_SCALE_Z,
    HAL_CH_BONE,   /* bone local pose quaternion       */
    HAL_CH__COUNT,
} hal_channel_type;

typedef struct hal_keyframe {
    hal_f32 time;   /* seconds                                   */
    hal_vec3f vec;  /* value for vec channels                    */
    hal_quatf rot;  /* value for quaternion channels             */
    hal_interp interp; /* easing OUT of this key to the next    */
} hal_keyframe;

typedef struct hal_anim_channel {
    hal_channel_type type;
    hal_object_id object;
    int bone; /* HAL_CH_BONE only; -1 otherwise             */
    hal_keyframe *keys;
    size_t count, cap;
} hal_anim_channel;

typedef struct hal_clip {
    char name[64];
    hal_anim_channel *channels;
    size_t count, cap;
    hal_f32 start, end; /* seconds                               */
    bool loop;
} hal_clip;

/* Scene-owned clip list. */
int hal_scene_clip_add(struct hal_scene *scene, const char *name);
size_t hal_scene_clip_count(const struct hal_scene *scene);
hal_clip *hal_scene_clip(struct hal_scene *scene, int index);

/* Finds or creates the channel for (type, object, bone). */
hal_anim_channel *hal_clip_channel(hal_clip *clip, hal_channel_type type,
                                   hal_object_id object, int bone);

/* Inserts or replaces a key at `time` (keys stay sorted). */
bool hal_clip_key_vec(hal_clip *clip, hal_channel_type type,
                      hal_object_id object, int bone, hal_f32 time,
                      hal_vec3f value, hal_interp interp);
bool hal_clip_key_rot(hal_clip *clip, hal_object_id object, int bone,
                      hal_f32 time, hal_quatf value, hal_interp interp);

/* Captures the object's whole TRS at `time` (the editor's I key). */
bool hal_clip_key_object(hal_clip *clip, struct hal_scene *scene,
                         hal_object_id object, hal_f32 time);

/* Samples the clip at `time` and writes into the scene (location,
 * rotation, scale, bone poses). Time outside [start,end] clamps (or
 * wraps when the clip loops). */
void hal_clip_sample(hal_clip *clip, struct hal_scene *scene,
                       hal_f32 time);

/* The clip's key time bounds (start/end are updated by inserts). */
void hal_clip_bounds(hal_clip *clip, hal_f32 *out_start,
                     hal_f32 *out_end);

/* Evaluates ONE object's channels at `time` (the exporter's path: no
 * scene writes). Channels that do not exist leave their out value
 * untouched; returns a bitmask of which were evaluated (bit 0
 * translation, 1 rotation, 2 scale). */
int hal_clip_sample_object(const hal_clip *clip, hal_object_id object,
                           hal_f32 time, hal_vec3f *out_loc,
                           hal_quatf *out_rot, hal_vec3f *out_scale);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ANIM_H */
