/*
 * project.c — .halcyon project file save/load
 *
 * Save: serialize the scene graph into the documented line grammar,
 * atomically (temp file + rename). Load: strict fail-closed parse into
 * a fresh scene. See docs/project-format.md for the user-facing
 * grammar description.
 */

#include "project.h"

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
                    hal_u32 tex = 0;
                    if (!parse_u32(&p, &tex) || !expect_end_of_line(&p)) {
                        set_err(err, err_cap,
                                "line %d: texture wants one index",
                                mline);
                        result = HAL_ERR_INVALID_ARGUMENT;
                        free(mkey);
                        goto done;
                    }
                    mat.texture = (int)tex;
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
        } else if (strcmp(key, "end") == 0) {
            if (current == HAL_OBJECT_ID_NONE) {
                set_err(err, err_cap,
                        "line %d: 'end' outside an object block", line_no);
                result = HAL_ERR_INVALID_ARGUMENT;
                free(key);
                goto done;
            }
            current = HAL_OBJECT_ID_NONE;
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
