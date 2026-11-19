/*
 * anim.c — clips, channels, keyframes, sampling
 */

#include "anim.h"

#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Scene clip list ---- */

int hal_scene_clip_add(hal_scene *scene, const char *name) {
    if (!scene)
        return -1;
    hal_clip *grown = realloc(scene->clips,
                              (scene->clip_count + 1) * sizeof(*grown));
    if (!grown)
        return -1;
    scene->clips = grown;
    hal_clip *clip = &scene->clips[scene->clip_count];
    memset(clip, 0, sizeof(*clip));
    snprintf(clip->name, sizeof(clip->name), "%s",
             name && name[0] ? name : "Clip");
    clip->start = 0.0f;
    clip->end = 1.0f;
    return (int)scene->clip_count++;
}

size_t hal_scene_clip_count(const hal_scene *scene) {
    return scene ? scene->clip_count : 0;
}

hal_clip *hal_scene_clip(hal_scene *scene, int index) {
    if (!scene || index < 0 || (size_t)index >= scene->clip_count)
        return NULL;
    return &scene->clips[index];
}

/* ---- Channels ---- */

hal_anim_channel *hal_clip_channel(hal_clip *clip, hal_channel_type type,
                                   hal_object_id object, int bone) {
    if (!clip)
        return NULL;
    for (size_t i = 0; i < clip->count; i++) {
        hal_anim_channel *ch = &clip->channels[i];
        if (ch->type == type && ch->object == object && ch->bone == bone)
            return ch;
    }
    if (clip->count == clip->cap) {
        size_t cap = clip->cap ? clip->cap * 2 : 8;
        hal_anim_channel *grown =
            realloc(clip->channels, cap * sizeof(*grown));
        if (!grown)
            return NULL;
        clip->channels = grown;
        clip->cap = cap;
    }
    hal_anim_channel *ch = &clip->channels[clip->count++];
    memset(ch, 0, sizeof(*ch));
    ch->type = type;
    ch->object = object;
    ch->bone = bone;
    return ch;
}

/* Binary-search-ish insert (keys stay sorted; replaces on exact
 * time). */
static bool key_insert(hal_anim_channel *ch, hal_keyframe key) {
    if (ch->count == ch->cap) {
        size_t cap = ch->cap ? ch->cap * 2 : 8;
        hal_keyframe *grown = realloc(ch->keys, cap * sizeof(*grown));
        if (!grown)
            return false;
        ch->keys = grown;
        ch->cap = cap;
    }
    size_t at = ch->count;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->keys[i].time >= key.time) {
            at = i;
            break;
        }
    }
    if (at < ch->count && ch->keys[at].time == key.time) {
        ch->keys[at] = key; /* replace */
        return true;
    }
    memmove(&ch->keys[at + 1], &ch->keys[at],
            (ch->count - at) * sizeof(hal_keyframe));
    ch->keys[at] = key;
    ch->count++;
    return true;
}

bool hal_clip_key_vec(hal_clip *clip, hal_channel_type type,
                      hal_object_id object, int bone, hal_f32 time,
                      hal_vec3f value, hal_interp interp) {
    if (!clip || type == HAL_CH_ROT || type == HAL_CH_BONE)
        return false;
    hal_anim_channel *ch = hal_clip_channel(clip, type, object, bone);
    if (!ch)
        return false;
    hal_keyframe k = { .time = time, .vec = value, .rot =
                           hal_quatf_identity(), .interp = interp };
    if (!key_insert(ch, k))
        return false;
    if (time < clip->start)
        clip->start = time;
    if (time > clip->end)
        clip->end = time;
    return true;
}

bool hal_clip_key_rot(hal_clip *clip, hal_object_id object, int bone,
                      hal_f32 time, hal_quatf value, hal_interp interp) {
    if (!clip)
        return false;
    hal_channel_type type = bone >= 0 ? HAL_CH_BONE : HAL_CH_ROT;
    hal_anim_channel *ch = hal_clip_channel(clip, type, object, bone);
    if (!ch)
        return false;
    hal_keyframe k = { .time = time, .vec = hal_vec3f_zero(),
                       .rot = value, .interp = interp };
    if (!key_insert(ch, k))
        return false;
    if (time < clip->start)
        clip->start = time;
    if (time > clip->end)
        clip->end = time;
    return true;
}

bool hal_clip_key_object(hal_clip *clip, hal_scene *scene,
                         hal_object_id object, hal_f32 time) {
    hal_object *obj = hal_scene_object(scene, object);
    if (!clip || !obj)
        return false;
    bool ok = true;
    ok = ok && hal_clip_key_vec(clip, HAL_CH_LOC_X, object, -1, time,
                                (hal_vec3f){ obj->location.x, 0, 0 },
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_vec(clip, HAL_CH_LOC_Y, object, -1, time,
                                (hal_vec3f){ 0, obj->location.y, 0 },
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_vec(clip, HAL_CH_LOC_Z, object, -1, time,
                                (hal_vec3f){ 0, 0, obj->location.z },
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_rot(clip, object, -1, time, obj->rotation,
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_vec(clip, HAL_CH_SCALE_X, object, -1, time,
                                (hal_vec3f){ obj->scale.x, 0, 0 },
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_vec(clip, HAL_CH_SCALE_Y, object, -1, time,
                                (hal_vec3f){ 0, obj->scale.y, 0 },
                                HAL_INTERP_SMOOTH);
    ok = ok && hal_clip_key_vec(clip, HAL_CH_SCALE_Z, object, -1, time,
                                (hal_vec3f){ 0, 0, obj->scale.z },
                                HAL_INTERP_SMOOTH);
    return ok;
}

/* ---- Sampling ---- */

static hal_f32 apply_ease(hal_interp interp, hal_f32 t) {
    switch (interp) {
    case HAL_INTERP_SMOOTH:
        return t * t * (3.0f - 2.0f * t); /* smoothstep */
    case HAL_INTERP_STEP:
        return 0.0f;
    default:
        return t;
    }
}

typedef struct sample_result {
    bool valid;
    hal_vec3f vec;
    hal_quatf rot;
} sample_result;

static sample_result channel_sample(const hal_anim_channel *ch,
                                    hal_f32 time) {
    sample_result r = { .valid = false,
                        .vec = hal_vec3f_zero(),
                        .rot = hal_quatf_identity() };
    if (ch->count == 0)
        return r;
    if (time <= ch->keys[0].time) {
        r.valid = true;
        r.vec = ch->keys[0].vec;
        r.rot = ch->keys[0].rot;
        return r;
    }
    if (time >= ch->keys[ch->count - 1].time) {
        r.valid = true;
        r.vec = ch->keys[ch->count - 1].vec;
        r.rot = ch->keys[ch->count - 1].rot;
        return r;
    }
    for (size_t i = 0; i + 1 < ch->count; i++) {
        const hal_keyframe *a = &ch->keys[i];
        const hal_keyframe *b = &ch->keys[i + 1];
        if (time >= a->time && time <= b->time) {
            hal_f32 span = b->time - a->time;
            hal_f32 t = span > 1e-9f ? (time - a->time) / span : 0.0f;
            t = apply_ease(a->interp, t);
            r.valid = true;
            r.vec = hal_vec3f_lerp(a->vec, b->vec, t);
            r.rot = hal_quatf_slerp(a->rot, b->rot, t);
            return r;
        }
    }
    return r; /* unreachable in practice */
}

void hal_clip_sample(hal_clip *clip, hal_scene *scene, hal_f32 time) {
    if (!clip || !scene || clip->count == 0)
        return;

    if (clip->loop && clip->end > clip->start) {
        hal_f32 span = clip->end - clip->start;
        time = clip->start +
               (hal_f32)fmod((double)(time - clip->start),
                             (double)span);
        if (time < clip->start)
            time += span;
    }

    for (size_t c = 0; c < clip->count; c++) {
        const hal_anim_channel *ch = &clip->channels[c];
        sample_result s = channel_sample(ch, time);
        if (!s.valid)
            continue;
        hal_object *obj = hal_scene_object(scene, ch->object);
        if (!obj)
            continue;

        switch (ch->type) {
        case HAL_CH_LOC_X: obj->location.x = s.vec.x; break;
        case HAL_CH_LOC_Y: obj->location.y = s.vec.y; break;
        case HAL_CH_LOC_Z: obj->location.z = s.vec.z; break;
        case HAL_CH_ROT:
            hal_object_set_rotation(scene, obj->id, s.rot);
            break;
        case HAL_CH_SCALE_X: obj->scale.x = s.vec.x; break;
        case HAL_CH_SCALE_Y: obj->scale.y = s.vec.y; break;
        case HAL_CH_SCALE_Z: obj->scale.z = s.vec.z; break;
        case HAL_CH_BONE:
            if (obj->armature && ch->bone >= 0 &&
                (size_t)ch->bone < obj->armature->count)
                obj->armature->bones[ch->bone].pose = s.rot;
            break;
        default:
            break;
        }
    }
}

void hal_clip_bounds(hal_clip *clip, hal_f32 *out_start,
                     hal_f32 *out_end) {
    if (!clip)
        return;
    /* The clip's declared range; keys may not touch the ends. */
    if (out_start)
        *out_start = clip->start;
    if (out_end)
        *out_end = clip->end;
}

int hal_clip_sample_object(const hal_clip *clip, hal_object_id object,
                           hal_f32 time, hal_vec3f *out_loc,
                           hal_quatf *out_rot, hal_vec3f *out_scale) {
    if (!clip)
        return 0;
    int mask = 0;
    hal_vec3f loc = { 0, 0, 0 };
    hal_vec3f scale = { 0, 0, 0 };
    for (size_t c = 0; c < clip->count; c++) {
        const hal_anim_channel *ch = &clip->channels[c];
        if (ch->object != object)
            continue;
        sample_result s = channel_sample(ch, time);
        if (!s.valid)
            continue;
        switch (ch->type) {
        case HAL_CH_LOC_X: loc.x = s.vec.x; mask |= 1; break;
        case HAL_CH_LOC_Y: loc.y = s.vec.y; mask |= 1; break;
        case HAL_CH_LOC_Z: loc.z = s.vec.z; mask |= 1; break;
        case HAL_CH_ROT:
            if (out_rot)
                *out_rot = s.rot;
            mask |= 2;
            break;
        case HAL_CH_SCALE_X: scale.x = s.vec.x; mask |= 4; break;
        case HAL_CH_SCALE_Y: scale.y = s.vec.y; mask |= 4; break;
        case HAL_CH_SCALE_Z: scale.z = s.vec.z; mask |= 4; break;
        default: break; /* bone channels are not object paths */
        }
    }
    if ((mask & 1) && out_loc)
        *out_loc = loc;
    if ((mask & 4) && out_scale)
        *out_scale = scale;
    return mask;
}

bool hal_clip_sample_bone(const hal_clip *clip, hal_object_id armature,
                          int bone, hal_f32 time, hal_quatf *out_rot) {
    if (!clip || !out_rot)
        return false;
    for (size_t i = 0; i < clip->count; i++) {
        const hal_anim_channel *ch = &clip->channels[i];
        if (ch->type != HAL_CH_BONE || ch->object != armature ||
            ch->bone != bone)
            continue;
        sample_result s = channel_sample(ch, time);
        if (s.valid) {
            *out_rot = s.rot;
            return true;
        }
    }
    return false;
}

/* ---- Curve editing ---- */

static int key_time_cmp(const void *a, const void *b) {
    const hal_keyframe *ka = a, *kb = b;
    if (ka->time < kb->time)
        return -1;
    if (ka->time > kb->time)
        return 1;
    return 0;
}

bool hal_clip_key_move(hal_clip *clip, size_t channel, size_t key,
                       hal_f32 time, hal_vec3f vec, hal_quatf rot) {
    if (!clip || channel >= clip->count)
        return false;
    hal_anim_channel *ch = &clip->channels[channel];
    if (key >= ch->count)
        return false;
    ch->keys[key].time = time;
    ch->keys[key].vec = vec;
    ch->keys[key].rot = rot;
    qsort(ch->keys, ch->count, sizeof(hal_keyframe), key_time_cmp);

    /* The clip range follows the keys. */
    clip->end = clip->start;
    for (size_t i = 0; i < clip->count; i++)
        for (size_t k = 0; k < clip->channels[i].count; k++)
            if (clip->channels[i].keys[k].time > clip->end)
                clip->end = clip->channels[i].keys[k].time;
    return true;
}

bool hal_clip_key_delete(hal_clip *clip, size_t channel,
                         size_t key) {
    if (!clip || channel >= clip->count)
        return false;
    hal_anim_channel *ch = &clip->channels[channel];
    if (key >= ch->count)
        return false;
    memmove(&ch->keys[key], &ch->keys[key + 1],
            (ch->count - key - 1) * sizeof(hal_keyframe));
    ch->count--;
    if (ch->count == 0) {
        /* The channel dies with its last key. */
        free(ch->keys);
        memmove(&clip->channels[channel],
                &clip->channels[channel + 1],
                (clip->count - channel - 1) *
                    sizeof(hal_anim_channel));
        clip->count--;
    }
    clip->end = clip->start;
    for (size_t i = 0; i < clip->count; i++)
        for (size_t k = 0; k < clip->channels[i].count; k++)
            if (clip->channels[i].keys[k].time > clip->end)
                clip->end = clip->channels[i].keys[k].time;
    return true;
}

hal_clip *hal_clip_clone(const hal_clip *clip) {
    if (!clip)
        return NULL;
    hal_clip *out = malloc(sizeof(*out));
    if (!out)
        return NULL;
    *out = *clip;
    out->count = 0;
    out->cap = clip->cap;
    out->channels = NULL;
    if (clip->count > 0) {
        out->channels = malloc(clip->count *
                               sizeof(hal_anim_channel));
        if (!out->channels) {
            free(out);
            return NULL;
        }
        for (size_t i = 0; i < clip->count; i++) {
            out->channels[i] = clip->channels[i];
            out->channels[i].keys = NULL;
            if (clip->channels[i].count > 0) {
                out->channels[i].keys = malloc(
                    clip->channels[i].count *
                    sizeof(hal_keyframe));
                if (!out->channels[i].keys) {
                    out->channels[i].count = 0;
                    continue;
                }
                memcpy(out->channels[i].keys,
                       clip->channels[i].keys,
                       clip->channels[i].count *
                           sizeof(hal_keyframe));
            }
        }
        out->count = clip->count;
    }
    return out;
}

void hal_clip_destroy(hal_clip *clip) {
    if (!clip)
        return;
    for (size_t i = 0; i < clip->count; i++)
        free(clip->channels[i].keys);
    free(clip->channels);
    free(clip);
}
