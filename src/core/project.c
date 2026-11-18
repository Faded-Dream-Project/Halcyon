/*
 * project.c — .halcyon project file save/load
 *
 * Save: serialize the scene graph into the documented line grammar,
 * atomically (temp file + rename). Load: strict fail-closed parse into
 * a fresh scene. See docs/project-format.md for the user-facing
 * grammar description.
 */

#include "project.h"

#define HAL_PROJECT_FORMAT_VERSION 3

#include "anim.h"
#include "armature.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Error reporting helper ---- */

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- Save ---- */

static const char *object_type_token(hal_object_type type) {
    switch (type) {
    case HAL_OBJECT_MESH:     return "mesh";
    case HAL_OBJECT_ARMATURE: return "armature";
    case HAL_OBJECT_CAMERA:   return "camera";
    case HAL_OBJECT_LIGHT:    return "light";
    }
    return "mesh";
}

hal_result hal_project_save(const hal_scene *scene, const char *path,
                            char *err, size_t err_cap) {
    if (!scene || !path || !path[0]) {
        set_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }

    char *tmp = malloc(strlen(path) + 8);
    if (!tmp) {
        set_err(err, err_cap, "out of memory");
        return HAL_ERR_OUT_OF_MEMORY;
    }
    sprintf(tmp, "%s.tmp~", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        set_err(err, err_cap, "cannot write %s: %s", tmp, strerror(errno));
        free(tmp);
        return HAL_ERR_IO;
    }

    bool ok = true;
    ok = ok && fprintf(f, "halcyon %d\n", HAL_PROJECT_FORMAT_VERSION) > 0;
    ok = ok && fprintf(f, "scene \"%s\"\n", scene->name) > 0;

    for (size_t mi = 0; ok && mi < scene->material_count; mi++) {
        const hal_material *mat = &scene->materials[mi];
        ok = ok && fprintf(f, "material %d \"%s\"\n", (int)mi,
                           mat->name) > 0;
        ok = ok && fprintf(f, "  color %.9g %.9g %.9g %.9g\n",
                           (double)mat->base_color.r,
                           (double)mat->base_color.g,
                           (double)mat->base_color.b,
                           (double)mat->base_color.a) > 0;
        ok = ok && fprintf(f, "  mr %.9g %.9g\n",
                           (double)mat->metallic,
                           (double)mat->roughness) > 0;
        ok = ok && fprintf(f, "  emissive %.9g %.9g %.9g %.9g\n",
                           (double)mat->emissive.x,
                           (double)mat->emissive.y,
                           (double)mat->emissive.z,
                           (double)mat->emissive_strength) > 0;
        ok = ok && fprintf(f, "  texture %d\n", mat->texture) > 0;
        ok = ok && fprintf(f, "end\n") > 0;
    }
    for (size_t ii = 0; ok && ii < scene->image_count; ii++)
        ok = ok && fprintf(f, "image \"%s\"\n", scene->images[ii]) > 0;

    for (size_t ci = 0; ok && ci < scene->clip_count; ci++) {
        const hal_clip *clip = &scene->clips[ci];
        ok = ok && fprintf(f, "clip %zu \"%s\"\n", ci, clip->name) > 0;
        ok = ok && fprintf(f, "  range %.9g %.9g\n",
                           (double)clip->start,
                           (double)clip->end) > 0;
        ok = ok && fprintf(f, "  loop %d\n", clip->loop ? 1 : 0) > 0;
        for (size_t c = 0; ok && c < clip->count; c++) {
            const hal_anim_channel *ch = &clip->channels[c];
            ok = ok && fprintf(f, "  channel %d %u %d\n", (int)ch->type,
                               (unsigned)ch->object, ch->bone) > 0;
            for (size_t k = 0; ok && k < ch->count; k++) {
                const hal_keyframe *key = &ch->keys[k];
                ok = ok && fprintf(f, "  key %.9g"
                                          " %.9g %.9g %.9g"
                                          " %.9g %.9g %.9g %.9g %d\n",
                                   (double)key->time,
                                   (double)key->vec.x,
                                   (double)key->vec.y,
                                   (double)key->vec.z,
                                   (double)key->rot.x,
                                   (double)key->rot.y,
                                   (double)key->rot.z,
                                   (double)key->rot.w,
                                   (int)key->interp) > 0;
            }
            ok = ok && fprintf(f, "  end\n") > 0;
        }
        ok = ok && fprintf(f, "end\n") > 0;
    }

    for (size_t i = 0; ok && i < scene->count; i++) {
        const hal_object *o = &scene->objects[i];
        ok = ok && fprintf(f, "object %u \"%s\" %s\n", (unsigned)o->id,
                           o->name, object_type_token(o->type)) > 0;
        ok = ok && fprintf(f, "  parent %u\n", (unsigned)o->parent) > 0;
        ok = ok && fprintf(f, "  loc %.9g %.9g %.9g\n", (double)o->location.x,
                           (double)o->location.y, (double)o->location.z) > 0;
        ok = ok && fprintf(f, "  rot %.9g %.9g %.9g %.9g\n",
                           (double)o->rotation.x, (double)o->rotation.y,
                           (double)o->rotation.z, (double)o->rotation.w) > 0;
        ok = ok && fprintf(f, "  scale %.9g %.9g %.9g\n", (double)o->scale.x,
                           (double)o->scale.y, (double)o->scale.z) > 0;
        ok = ok && fprintf(f, "  flags%s%s%s%s\n",
                           o->visible ? " visible" : "",
                           o->selected ? " selected" : "",
                           o->smooth_shading ? " smooth" : "",
                           (!o->visible && !o->selected &&
                            !o->smooth_shading)
                               ? " -"
                               : "") > 0;
        if (o->material >= 0)
            ok = ok && fprintf(f, "  mat %d\n", o->material) > 0;
        if (o->skin_armature != HAL_OBJECT_ID_NONE)
            ok = ok && fprintf(f, "  skin %u\n",
                               (unsigned)o->skin_armature) > 0;
        if (o->armature) {
            for (size_t b = 0; ok && b < o->armature->count; b++) {
                const hal_bone *bone = &o->armature->bones[b];
                ok = ok &&
                     fprintf(f,
                             "  bone %zu \"%s\" %d"
                             " %.9g %.9g %.9g %.9g %.9g %.9g"
                             " %.9g %.9g %.9g %.9g\n",
                             b, bone->name, bone->parent,
                             (double)bone->head.x,
                             (double)bone->head.y,
                             (double)bone->head.z,
                             (double)bone->tail.x,
                             (double)bone->tail.y,
                             (double)bone->tail.z,
                             (double)bone->pose.x,
                             (double)bone->pose.y,
                             (double)bone->pose.z,
                             (double)bone->pose.w) > 0;
            }
        }
        if (o->mesh) {
            const hal_mesh *mm = o->mesh;
            ok = ok && fprintf(f, "  mesh %zu %zu %zu\n", mm->vert_count,
                               mm->edge_count, mm->face_count) > 0;
            for (size_t v = 0; v < mm->vert_count; v++)
                ok = ok && fprintf(f, "  v %.9g %.9g %.9g\n",
                                   (double)mm->verts[v].x,
                                   (double)mm->verts[v].y,
                                   (double)mm->verts[v].z) > 0;
            for (size_t e = 0; e < mm->edge_count; e++)
                ok = ok && fprintf(f, "  e %u %u\n",
                                   (unsigned)mm->edges[e * 2],
                                   (unsigned)mm->edges[e * 2 + 1]) > 0;
            for (size_t f2 = 0; f2 < mm->face_count; f2++) {
                ok = ok && fprintf(f, "  f %zu",
                                   mm->face_off[f2 + 1] -
                                       mm->face_off[f2]) > 0;
                for (size_t li = mm->face_off[f2];
                     ok && li < mm->face_off[f2 + 1]; li++)
                    ok = fprintf(f, " %u", (unsigned)mm->loops[li]) > 0;
                ok = ok && fprintf(f, "\n") > 0;
            }
        }
        if (o->mesh && o->mesh->skin_weights) {
            ok = ok && fprintf(f, "  weights %zu\n",
                               o->mesh->vert_count) > 0;
            for (size_t v = 0; ok && v < o->mesh->vert_count; v++) {
                ok = ok && fprintf(f, "  wv") > 0;
                for (size_t wi = 0; ok && wi < 4; wi++)
                    ok = fprintf(f, " %d %.9g",
                                 o->mesh->skin_bones[v * 4 + wi],
                                 (double)o->mesh->skin_weights[v * 4 + wi]) >
                         0;
                ok = ok && fprintf(f, "\n") > 0;
            }
        }
        ok = ok && fprintf(f, "end\n") > 0;
    }

    if (ok) {
        ok = fflush(f) == 0;
        if (ok) {
            /* fsync before rename: the rename is atomic, but the data
             * must be on disk before the old file is replaced. */
            int fd = fileno(f);
            if (fd >= 0)
                ok = fsync(fd) == 0;
        }
    }

    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        set_err(err, err_cap, "write failed: %s", strerror(errno));
        unlink(tmp);
        free(tmp);
        return HAL_ERR_IO;
    }

    if (rename(tmp, path) != 0) {
        set_err(err, err_cap, "cannot replace %s: %s", path,
                strerror(errno));
        unlink(tmp);
        free(tmp);
        return HAL_ERR_IO;
    }

    free(tmp);
    return HAL_OK;
}

/* ---- Load: the strict parser ----
 *
 * A hand-rolled tokenizer over one line at a time. Every line must
 * match the grammar exactly; anything else is an error naming the
 * line. No allocations beyond the scene's own. */

typedef struct parser {
    const char *data;
    size_t len;
    size_t pos;
    int line;
} parser;

static int parser_peek(parser *p) {
    return p->pos < p->len ? (unsigned char)p->data[p->pos] : -1;
}

static int parser_next(parser *p) {
    if (p->pos >= p->len)
        return -1;
    int c = (unsigned char)p->data[p->pos++];
    if (c == '\n')
        p->line++;
    return c;
}

/* Skips spaces and tabs (not newlines). */
static void skip_spaces(parser *p) {
    while (parser_peek(p) == ' ' || parser_peek(p) == '\t' ||
           parser_peek(p) == '\r')
        parser_next(p);
}

/* Reads a bare word ([A-Za-z0-9_.-]+); returns a malloc'd string or
 * NULL at end-of-line / on an empty word. */
static char *parse_word(parser *p) {
    skip_spaces(p);
    size_t start = p->pos;
    while (parser_peek(p) != -1 && parser_peek(p) != '\n' &&
           parser_peek(p) != ' ' && parser_peek(p) != '\t' &&
           parser_peek(p) != '\r')
        parser_next(p);
    if (p->pos == start)
        return NULL;
    size_t n = p->pos - start;
    char *word = malloc(n + 1);
    if (!word)
        return NULL;
    memcpy(word, p->data + start, n);
    word[n] = '\0';
    return word;
}

/* Reads a "quoted string" (no escapes in v1; a quote inside is an
 * error). Returns a malloc'd string or NULL. */
static char *parse_quoted(parser *p) {
    skip_spaces(p);
    if (parser_peek(p) != '"')
        return NULL;
    parser_next(p);
    size_t start = p->pos;
    while (parser_peek(p) != -1 && parser_peek(p) != '"' &&
           parser_peek(p) != '\n')
        parser_next(p);
    if (parser_peek(p) != '"')
        return NULL;
    size_t n = p->pos - start;
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, p->data + start, n);
    s[n] = '\0';
    parser_next(p); /* consume the closing quote */
    return s;
}

/* Reads a float; ok=false on failure (bad token or trailing junk). */
static bool parse_float(parser *p, hal_f32 *out) {
    char *word = parse_word(p);
    if (!word)
        return false;
    char *end = NULL;
    errno = 0;
    double v = strtod(word, &end);
    bool ok = end && *end == '\0' && end != word && errno == 0 &&
              v > -1e30 && v < 1e30;
    free(word);
    if (ok)
        *out = (hal_f32)v;
    return ok;
}

static bool parse_u32(parser *p, hal_u32 *out) {
    char *word = parse_word(p);
    if (!word)
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(word, &end, 10);
    bool ok = end && *end == '\0' && end != word && errno == 0 &&
              v <= 0xFFFFFFFFul;
    free(word);
    if (ok)
        *out = (hal_u32)v;
    return ok;
}

/* Consumes the rest of the line (whitespace only allowed). */
static bool expect_end_of_line(parser *p) {
    skip_spaces(p);
    return parser_peek(p) == -1 || parser_peek(p) == '\n';
}

static void consume_line(parser *p) {
    while (parser_peek(p) != -1 && parser_peek(p) != '\n')
        parser_next(p);
    if (parser_peek(p) == '\n')
        parser_next(p);
}

static bool object_type_from_token(const char *token,
                                   hal_object_type *out) {
    if (strcmp(token, "mesh") == 0)
        *out = HAL_OBJECT_MESH;
    else if (strcmp(token, "armature") == 0)
        *out = HAL_OBJECT_ARMATURE;
    else if (strcmp(token, "camera") == 0)
        *out = HAL_OBJECT_CAMERA;
    else if (strcmp(token, "light") == 0)
        *out = HAL_OBJECT_LIGHT;
    else
        return false;
    return true;
}

hal_result hal_project_load(const char *path, hal_scene **out_scene,
                            char *err, size_t err_cap) {
    if (!path || !out_scene) {
        set_err(err, err_cap, "invalid arguments");
        return HAL_ERR_INVALID_ARGUMENT;
    }
    *out_scene = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_cap, "cannot read %s: %s", path, strerror(errno));
        return HAL_ERR_IO;
    }

    char *data = NULL;
    size_t len = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz < 64 * 1024 * 1024) { /* hard cap: 64 MiB */
            rewind(f);
            data = malloc((size_t)sz + 1);
            if (data) {
                len = fread(data, 1, (size_t)sz, f);
                data[len] = '\0';
            }
        }
    }
    fclose(f);

    if (!data) {
        set_err(err, err_cap, "cannot read %s (empty or too large)", path);
        free(data);
        return HAL_ERR_IO;
    }

    parser p = { .data = data, .len = len, .pos = 0, .line = 1 };
    hal_result result = HAL_OK;
    hal_scene *scene = NULL;
    hal_object_id current = HAL_OBJECT_ID_NONE;
    hal_clip *cur_clip = NULL;
    hal_anim_channel *cur_channel = NULL;

    /* --- header --- */
    {
        char *key = parse_word(&p);
        if (!key || strcmp(key, "halcyon") != 0) {
            set_err(err, err_cap, "line 1: not a halcyon project "
                                  "(expected 'halcyon <version>')");
            result = HAL_ERR_INVALID_ARGUMENT;
            free(key);
            goto done;
        }
        free(key);

        hal_u32 version = 0;
        if (!parse_u32(&p, &version) || !expect_end_of_line(&p)) {
            set_err(err, err_cap, "line 1: bad version");
            result = HAL_ERR_INVALID_ARGUMENT;
            goto done;
        }
        if (version == 0 || version > (hal_u32)HAL_PROJECT_FORMAT_VERSION) {
            set_err(err, err_cap,
                    "line 1: format version %u is newer than this "
                    "Halcyon supports (max %d)",
                    (unsigned)version, HAL_PROJECT_FORMAT_VERSION);
            result = HAL_ERR_UNSUPPORTED;
            goto done;
        }
        consume_line(&p);
    }

    /* --- body --- */
    while (result == HAL_OK) {
        skip_spaces(&p);
        if (parser_peek(&p) == -1)
            break; /* clean EOF */
        if (parser_peek(&p) == '\n') { /* blank line: allowed */
            parser_next(&p);
            continue;
        }

        int line_no = p.line;
        char *key = parse_word(&p);
        if (!key) {
            set_err(err, err_cap, "line %d: expected a key", line_no);
            result = HAL_ERR_INVALID_ARGUMENT;
            goto done;
        }

        if (strcmp(key, "scene") == 0) {
            if (scene) {
                set_err(err, err_cap,
                        "line %d: duplicate 'scene'", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            char *name = parse_quoted(&p);
            if (!name || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: scene wants one quoted name", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(name);
                free(key);
                goto done;
            }
            scene = hal_scene_create(name);
            free(name);
            if (!scene) {
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                free(key);
                goto done;
            }
            consume_line(&p);
        } else if (strcmp(key, "object") == 0) {
            if (!scene) {
                scene = hal_scene_create("Scene");
                if (!scene) {
                    set_err(err, err_cap, "out of memory");
                    result = HAL_ERR_OUT_OF_MEMORY;
                    free(key);
                    goto done;
                }
            }
            hal_u32 id = 0;
            char *name = NULL;
            char *type_tok = NULL;
            hal_object_type type = HAL_OBJECT_MESH;
            bool ok = parse_u32(&p, &id) && id != 0;
            if (ok)
                name = parse_quoted(&p);
            ok = ok && name != NULL;
            if (ok) {
                type_tok = parse_word(&p);
                ok = type_tok != NULL &&
                     object_type_from_token(type_tok, &type);
            }
            if (!ok || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: object wants: id \"name\" type", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(name);
                free(type_tok);
                free(key);
                goto done;
            }
            consume_line(&p);

            hal_object_id new_id = hal_scene_object_add(scene, name, type);
            if (type == HAL_OBJECT_ARMATURE) {
                hal_armature *arm = hal_armature_create();
                if (arm)
                    hal_object_set_armature(scene, new_id, arm);
            }
            free(name);
            free(type_tok);
            if (new_id == HAL_OBJECT_ID_NONE) {
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                free(key);
                goto done;
            }
            /* The file states its own ids; adopt them. Objects are
             * dense, so the new one is the last. */
            hal_object *obj = hal_scene_object_at(scene,
                                                  hal_scene_object_count(scene) - 1);
            obj->id = id;
            if (id >= scene->next_id)
                scene->next_id = id + 1;
            current = id;
        } else if (strcmp(key, "parent") == 0 ||
                   strcmp(key, "loc") == 0 || strcmp(key, "rot") == 0 ||
                   strcmp(key, "scale") == 0 || strcmp(key, "flags") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj) {
                set_err(err, err_cap,
                        "line %d: '%s' outside an object block", line_no,
                        key);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }

            if (strcmp(key, "parent") == 0) {
                hal_u32 parent = 0;
                if (!parse_u32(&p, &parent) || !expect_end_of_line(&p)) {
                    set_err(err, err_cap,
                            "line %d: parent wants one id", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                /* Parents must be defined BEFORE their children, so a
                 * forward or self reference is a cycle or a typo. */
                if (parent != 0 &&
                    (parent == obj->id || !hal_scene_object(scene, parent))) {
                    set_err(err, err_cap,
                            "line %d: parent %u is not defined before "
                            "object %u",
                            line_no, (unsigned)parent, (unsigned)obj->id);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                obj->parent = parent;
            } else if (strcmp(key, "loc") == 0) {
                hal_f32 x, y, z;
                if (!parse_float(&p, &x) || !parse_float(&p, &y) ||
                    !parse_float(&p, &z) || !expect_end_of_line(&p)) {
                    set_err(err, err_cap,
                            "line %d: loc wants three numbers", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                obj->location = hal_vec3f_make(x, y, z);
            } else if (strcmp(key, "rot") == 0) {
                hal_f32 x, y, z, w;
                if (!parse_float(&p, &x) || !parse_float(&p, &y) ||
                    !parse_float(&p, &z) || !parse_float(&p, &w) ||
                    !expect_end_of_line(&p)) {
                    set_err(err, err_cap,
                            "line %d: rot wants four numbers", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                obj->rotation = hal_quatf_normalize(hal_quatf_make(x, y, z, w));
            } else if (strcmp(key, "scale") == 0) {
                hal_f32 x, y, z;
                if (!parse_float(&p, &x) || !parse_float(&p, &y) ||
                    !parse_float(&p, &z) || !expect_end_of_line(&p)) {
                    set_err(err, err_cap,
                            "line %d: scale wants three numbers", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                obj->scale = hal_vec3f_make(x, y, z);
            } else { /* flags */
                obj->visible = false;
                obj->selected = false;
                for (;;) {
                    skip_spaces(&p);
                    int c = parser_peek(&p);
                    if (c == -1 || c == '\n')
                        break;
                    char *flag = parse_word(&p);
                    if (!flag)
                        break;
                    if (strcmp(flag, "visible") == 0)
                        obj->visible = true;
                    else if (strcmp(flag, "selected") == 0)
                        obj->selected = true;
                    else if (strcmp(flag, "smooth") == 0)
                        obj->smooth_shading = true;
                    else if (strcmp(flag, "-") == 0) {
                        /* explicit "none" marker written by the saver */
                    } else {
                        set_err(err, err_cap,
                                "line %d: unknown flag '%s'", line_no, flag);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(flag);
                        free(key);
                        goto done;
                    }
                    free(flag);
                }
            }
            consume_line(&p);
        } else if (strcmp(key, "mat") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj) {
                set_err(err, err_cap,
                        "line %d: 'mat' outside an object block", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            hal_u32 mat = 0;
            if (!parse_u32(&p, &mat) || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: mat wants one index", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            obj->material = (int)mat;
            consume_line(&p);
        } else if (strcmp(key, "skin") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj) {
                set_err(err, err_cap,
                        "line %d: 'skin' outside an object block",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            hal_u32 skin = 0;
            if (!parse_u32(&p, &skin) || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: skin wants one id", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            obj->skin_armature = skin;
        } else if (strcmp(key, "bone") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj || !obj->armature) {
                set_err(err, err_cap,
                        "line %d: 'bone' outside an armature object",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            hal_u32 idx = 0;
            int parent = -2;
            char *name = NULL;
            hal_f32 h[3], t[3], q[4];
            bool ok_line = parse_u32(&p, &idx);
            if (ok_line)
                name = parse_quoted(&p);
            if (ok_line) {
                /* The parent is signed (-1 = root); parse_u32 would
                 * reject the wrapped -1. */
                char *tok = parse_word(&p);
                if (tok) {
                    parent = (int)strtol(tok, NULL, 10);
                    free(tok);
                } else {
                    ok_line = false;
                }
            }
            for (int i = 0; ok_line && i < 3; i++)
                ok_line = parse_float(&p, &h[i]);
            for (int i = 0; ok_line && i < 3; i++)
                ok_line = parse_float(&p, &t[i]);
            for (int i = 0; ok_line && i < 4; i++)
                ok_line = parse_float(&p, &q[i]);
            if (!ok_line || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: bone wants: idx \"name\" parent"
                        " head(3) tail(3) pose(4)",
                        line_no);
                free(name);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            consume_line(&p);
            int added = hal_armature_bone_add(
                obj->armature, name, hal_vec3f_make(h[0], h[1], h[2]),
                hal_vec3f_make(t[0], t[1], t[2]), parent);
            free(name);
            if (added < 0 || (size_t)added != idx) {
                set_err(err, err_cap,
                        "line %d: bone index %u out of order", line_no,
                        (unsigned)idx);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            obj->armature->bones[added].pose =
                hal_quatf_normalize(hal_quatf_make(q[0], q[1], q[2],
                                                   q[3]));
        } else if (strcmp(key, "weights") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj || !obj->mesh) {
                set_err(err, err_cap,
                        "line %d: 'weights' outside a mesh object",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            hal_u32 nv = 0;
            if (!parse_u32(&p, &nv) || !expect_end_of_line(&p) ||
                nv != (hal_u32)obj->mesh->vert_count) {
                set_err(err, err_cap,
                        "line %d: weights wants the vert count", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            consume_line(&p);
            hal_f32 *w = calloc((size_t)nv * 4, sizeof(hal_f32));
            hal_i32 *b = malloc((size_t)nv * 4 * sizeof(hal_i32));
            if (!w || !b) {
                free(w);
                free(b);
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                free(key);
                goto done;
            }
            bool w_ok = true;
            for (hal_u32 v = 0; w_ok && v < nv; v++) {
                skip_spaces(&p);
                char *wkey = parse_word(&p);
                if (!wkey || strcmp(wkey, "wv") != 0) {
                    free(wkey);
                    w_ok = false;
                    break;
                }
                free(wkey);
                for (size_t i = 0; w_ok && i < 4; i++) {
                    hal_i32 bi;
                    char *tok = parse_word(&p);
                    if (!tok)
                        w_ok = false;
                    if (w_ok) {
                        bi = (hal_i32)strtol(tok, NULL, 10);
                        free(tok);
                    } else {
                        break;
                    }
                    hal_f32 wt;
                    if (!parse_float(&p, &wt))
                        w_ok = false;
                    if (w_ok) {
                        b[v * 4 + i] = bi;
                        w[v * 4 + i] = wt;
                    }
                }
                if (!expect_end_of_line(&p))
                    w_ok = false;
                consume_line(&p);
            }
            if (!w_ok) {
                free(w);
                free(b);
                set_err(err, err_cap,
                        "line %d: bad weights block", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            free(obj->mesh->skin_weights);
            free(obj->mesh->skin_bones);
            obj->mesh->skin_weights = w;
            obj->mesh->skin_bones = b;
        } else if (strcmp(key, "mesh") == 0 ||
                   strcmp(key, "v") == 0 || strcmp(key, "e") == 0 ||
                   strcmp(key, "f") == 0) {
            hal_object *obj = hal_scene_object(scene, current);
            if (!obj) {
                set_err(err, err_cap,
                        "line %d: '%s' outside an object block", line_no,
                        key);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            if (strcmp(key, "mesh") == 0) {
                /* mesh <verts> <edges> <faces>: counts, verified at
                 * block end. */
                hal_u32 nv = 0, ne = 0, nf = 0;
                if (!parse_u32(&p, &nv) || !parse_u32(&p, &ne) ||
                    !parse_u32(&p, &nf) || !expect_end_of_line(&p)) {
                    set_err(err, err_cap,
                            "line %d: mesh wants three counts", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                if (nv > 10000000u || ne > 40000000u || nf > 10000000u) {
                    set_err(err, err_cap,
                            "line %d: mesh counts out of range", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                obj->mesh = hal_mesh_create();
                if (!obj->mesh) {
                    set_err(err, err_cap, "out of memory");
                    result = HAL_ERR_OUT_OF_MEMORY;
                    free(key);
                    goto done;
                }
            } else if (!obj->mesh) {
                set_err(err, err_cap,
                        "line %d: '%s' before its 'mesh' header", line_no,
                        key);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            } else if (strcmp(key, "v") == 0) {
                hal_f32 x, y, z;
                if (!parse_float(&p, &x) || !parse_float(&p, &y) ||
                    !parse_float(&p, &z) || !expect_end_of_line(&p) ||
                    hal_mesh_vert_add(obj->mesh,
                                      hal_vec3f_make(x, y, z)) < 0) {
                    set_err(err, err_cap,
                            "line %d: v wants three numbers", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
            } else if (strcmp(key, "e") == 0) {
                hal_u32 a, b;
                if (!parse_u32(&p, &a) || !parse_u32(&p, &b) ||
                    !expect_end_of_line(&p) ||
                    hal_mesh_edge_add(obj->mesh, a, b) < 0) {
                    set_err(err, err_cap,
                            "line %d: e wants two vertex ids", line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
            } else { /* f <n> <corners...> */
                hal_u32 n = 0;
                if (!parse_u32(&p, &n) || n < 3 || n > 64) {
                    set_err(err, err_cap,
                            "line %d: f wants a corner count in [3,64]",
                            line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
                hal_u32 corners[64];
                for (hal_u32 i = 0; i < n; i++)
                    if (!parse_u32(&p, &corners[i])) {
                        set_err(err, err_cap,
                                "line %d: f wants %u corners", line_no,
                                (unsigned)n);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(key);
                        goto done;
                    }
                if (!expect_end_of_line(&p) ||
                    hal_mesh_face_add(obj->mesh, corners, n) < 0) {
                    set_err(err, err_cap,
                            "line %d: bad face (indices out of range?)",
                            line_no);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(key);
                    goto done;
                }
            }
            consume_line(&p);
        } else if (strcmp(key, "material") == 0) {
            /* material <index> "<name>" */
            hal_u32 idx = 0;
            bool ok_line = parse_u32(&p, &idx);
            char *name = ok_line ? parse_quoted(&p) : NULL;
            ok_line = ok_line && name != NULL && expect_end_of_line(&p);
            if (!ok_line) {
                set_err(err, err_cap,
                        "line %d: material wants: index \"name\"", line_no);
                free(name);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            consume_line(&p);
            /* Sub-lines until 'end'. */
            hal_material mat = hal_material_default();
            snprintf(mat.name, sizeof(mat.name), "%s", name);
            free(name);
            mat.texture = -1;
            bool done_block = false;
            while (!done_block && result == HAL_OK) {
                skip_spaces(&p);
                if (parser_peek(&p) == -1)
                    break;
                if (parser_peek(&p) == '\n') {
                    parser_next(&p);
                    continue;
                }
                int mline = p.line;
                char *mkey = parse_word(&p);
                if (!mkey) {
                    set_err(err, err_cap, "line %d: expected a key",
                            mline);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    goto done;
                }
                if (strcmp(mkey, "color") == 0) {
                    hal_f32 r, g, b, a;
                    if (!parse_float(&p, &r) || !parse_float(&p, &g) ||
                        !parse_float(&p, &b) || !parse_float(&p, &a) ||
                        !expect_end_of_line(&p)) {
                        set_err(err, err_cap,
                                "line %d: color wants four numbers",
                                mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.base_color = hal_color_rgba(r, g, b, a);
                } else if (strcmp(mkey, "mr") == 0) {
                    hal_f32 met, rou;
                    if (!parse_float(&p, &met) ||
                        !parse_float(&p, &rou) || !expect_end_of_line(&p)) {
                        set_err(err, err_cap,
                                "line %d: mr wants two numbers", mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.metallic = met;
                    mat.roughness = rou;
                } else if (strcmp(mkey, "emissive") == 0) {
                    hal_f32 x, y, z, s;
                    if (!parse_float(&p, &x) || !parse_float(&p, &y) ||
                        !parse_float(&p, &z) || !parse_float(&p, &s) ||
                        !expect_end_of_line(&p)) {
                        set_err(err, err_cap,
                                "line %d: emissive wants four numbers",
                                mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.emissive = hal_vec3f_make(x, y, z);
                    mat.emissive_strength = s;
                } else if (strcmp(mkey, "texture") == 0) {
                    /* Signed: -1 = no texture (the saver writes it). */
                    char *tok = parse_word(&p);
                    bool tex_ok = tok != NULL;
                    long tex = -2;
                    if (tex_ok) {
                        tex = strtol(tok, NULL, 10);
                        free(tok);
                    }
                    if (!tex_ok || !expect_end_of_line(&p) ||
                        tex < -1) {
                        set_err(err, err_cap,
                                "line %d: texture wants an index"
                                " (-1 or more)",
                                mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.texture = (int)tex;
                } else if (strcmp(mkey, "filter") == 0) {
                    hal_u32 f2 = 0;
                    if (!parse_u32(&p, &f2) || !expect_end_of_line(&p)) {
                        set_err(err, err_cap,
                                "line %d: filter wants 0 or 1", mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.filter = (int)f2;
                } else if (strcmp(mkey, "end") == 0) {
                    done_block = true;
                } else {
                    set_err(err, err_cap,
                            "line %d: unknown material key '%s'", mline,
                            mkey);
                    result = HAL_ERR_INVALID_ARGUMENT;
                    free(mkey);
                    goto done;
                }
                free(mkey);
                consume_line(&p);
            }
            if (hal_scene_material_add(scene, &mat) < 0) {
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                goto done;
            }
        } else if (strcmp(key, "image") == 0) {
            char *img_path = parse_quoted(&p);
            if (!img_path || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: image wants one quoted path", line_no);
                free(img_path);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            consume_line(&p);
            if (hal_scene_image_add(scene, img_path) < 0) {
                free(img_path);
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                free(key);
                goto done;
            }
            free(img_path);
        } else if (strcmp(key, "clip") == 0) {
            hal_u32 idx = 0;
            char *name = NULL;
            bool ok_line = parse_u32(&p, &idx);
            if (ok_line)
                name = parse_quoted(&p);
            ok_line = ok_line && name != NULL && expect_end_of_line(&p);
            if (!ok_line) {
                set_err(err, err_cap,
                        "line %d: clip wants: index \"name\"", line_no);
                free(name);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            consume_line(&p);
            int added = hal_scene_clip_add(scene, name);
            free(name);
            cur_clip = added >= 0 ? hal_scene_clip(scene, added) : NULL;
            cur_channel = NULL;
        } else if (strcmp(key, "range") == 0) {
            hal_f32 s, e;
            if (!cur_clip || !parse_float(&p, &s) ||
                !parse_float(&p, &e) || !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: range wants two numbers (in a clip)",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            cur_clip->start = s;
            cur_clip->end = e;
        } else if (strcmp(key, "loop") == 0) {
            hal_u32 v = 0;
            if (!cur_clip || !parse_u32(&p, &v) ||
                !expect_end_of_line(&p)) {
                set_err(err, err_cap,
                        "line %d: loop wants 0 or 1", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            cur_clip->loop = v != 0;
        } else if (strcmp(key, "channel") == 0) {
            hal_u32 type = 0, object = 0;
            int bone = -2;
            char *tok = NULL;
            bool ok_line = cur_clip && parse_u32(&p, &type);
            if (ok_line)
                ok_line = parse_u32(&p, &object);
            if (ok_line) {
                tok = parse_word(&p);
                ok_line = tok != NULL;
            }
            if (ok_line) {
                bone = (int)strtol(tok, NULL, 10);
                free(tok);
            }
            if (!ok_line || !expect_end_of_line(&p) ||
                type >= HAL_CH__COUNT) {
                set_err(err, err_cap,
                        "line %d: channel wants: type object bone",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            cur_channel = hal_clip_channel(cur_clip,
                                           (hal_channel_type)type,
                                           object, bone);
            if (!cur_channel) {
                set_err(err, err_cap, "out of memory");
                result = HAL_ERR_OUT_OF_MEMORY;
                free(key);
                goto done;
            }
        } else if (strcmp(key, "key") == 0) {
            hal_f32 time, v[3], q[4];
            hal_u32 interp = 0;
            bool ok_line = cur_channel && parse_float(&p, &time);
            for (int i = 0; ok_line && i < 3; i++)
                ok_line = parse_float(&p, &v[i]);
            for (int i = 0; ok_line && i < 4; i++)
                ok_line = parse_float(&p, &q[i]);
            if (ok_line)
                ok_line = parse_u32(&p, &interp);
            if (!ok_line || !expect_end_of_line(&p) ||
                interp > HAL_INTERP_STEP) {
                set_err(err, err_cap,
                        "line %d: key wants: time vec(3) quat(4) interp",
                        line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            hal_keyframe kf = {
                .time = time,
                .vec = hal_vec3f_make(v[0], v[1], v[2]),
                .rot = hal_quatf_normalize(
                    hal_quatf_make(q[0], q[1], q[2], q[3])),
                .interp = (hal_interp)interp
            };
            /* Append in file order (the saver writes sorted keys). */
            if (cur_channel->count == cur_channel->cap) {
                size_t cap = cur_channel->cap
                                 ? cur_channel->cap * 2
                                 : 8;
                hal_keyframe *grown =
                    realloc(cur_channel->keys,
                            cap * sizeof(*grown));
                if (!grown) {
                    set_err(err, err_cap, "out of memory");
                    result = HAL_ERR_OUT_OF_MEMORY;
                    free(key);
                    goto done;
                }
                cur_channel->keys = grown;
                cur_channel->cap = cap;
            }
            cur_channel->keys[cur_channel->count++] = kf;
        } else if (strcmp(key, "end") == 0) {
            /* 'end' closes an object block, a material block, or a
             * clip's channel/block nesting — disambiguated by state. */
            if (cur_channel) {
                cur_channel = NULL;
            } else if (cur_clip) {
                cur_clip = NULL;
            } else if (current == HAL_OBJECT_ID_NONE) {
                set_err(err, err_cap,
                        "line %d: 'end' outside an object block", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            } else {
                current = HAL_OBJECT_ID_NONE;
            }
            consume_line(&p);
        } else {
            set_err(err, err_cap, "line %d: unknown key '%s'", line_no, key);
            result = HAL_ERR_INVALID_ARGUMENT;
            free(key);
            goto done;
        }

        free(key);
    }

    if (result == HAL_OK && current != HAL_OBJECT_ID_NONE) {
        set_err(err, err_cap, "unexpected end of file inside an object "
                              "block (missing 'end')");
        result = HAL_ERR_INVALID_ARGUMENT;
    }

    if (result == HAL_OK && !scene) {
        /* A file with no scene and no objects: legal, but make it a
         * real empty scene, not NULL. */
        scene = hal_scene_create("Scene");
        if (!scene) {
            set_err(err, err_cap, "out of memory");
            result = HAL_ERR_OUT_OF_MEMORY;
        }
    }

done:
    free(data);
    if (result == HAL_OK) {
        *out_scene = scene;
    } else {
        hal_scene_destroy(scene);
    }
    return result;
}
