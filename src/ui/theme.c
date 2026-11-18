/*
 * theme.c — Halcyon themes: the built-in Faded Dream face + the strict
 * .haltheme parser.
 */

#include "theme.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Token names (shared by parser and saver) ---- */

typedef struct color_name {
    const char *name;
    hal_theme_color_token token;
} color_name;

static const color_name COLOR_NAMES[] = {
    { "window", HAL_THEME_WINDOW },
    { "panel", HAL_THEME_PANEL },
    { "panel_title", HAL_THEME_PANEL_TITLE },
    { "border", HAL_THEME_BORDER },
    { "text", HAL_THEME_TEXT },
    { "text_dim", HAL_THEME_TEXT_DIM },
    { "accent", HAL_THEME_ACCENT },
    { "accent_text", HAL_THEME_ACCENT_TEXT },
    { "widget", HAL_THEME_WIDGET },
    { "widget_hover", HAL_THEME_WIDGET_HOVER },
    { "widget_active", HAL_THEME_WIDGET_ACTIVE },
    { "select", HAL_THEME_SELECT },
    { "viewport_bg", HAL_THEME_VIEWPORT_BG },
    { "viewport_grid", HAL_THEME_VIEWPORT_GRID },
    { "success", HAL_THEME_SUCCESS },
    { "warning", HAL_THEME_WARNING },
    { "danger", HAL_THEME_DANGER },
};

static const char *metric_names[] = {
    "radius", "padding", "gap", "font_size", "font_size_small",
};
enum { METRIC_RADIUS, METRIC_PADDING, METRIC_GAP, METRIC_FONT,
       METRIC_FONT_SMALL, METRIC__COUNT };

/* ---- The built-in face: Faded Dream ----
 * Soft purples and deep blues — the project's stated look. Every
 * value is also what the .haltheme file spells. */

hal_theme *hal_theme_default(void) {
    hal_theme *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->name = malloc(strlen("Faded Dream") + 1);
    if (!t->name) {
        free(t);
        return NULL;
    }
    strcpy(t->name, "Faded Dream");

    /* The night-sky family: deep indigo surfaces, violet accents. */
    t->colors[HAL_THEME_WINDOW] = hal_color_hex(0x141a3cu);
    t->colors[HAL_THEME_PANEL] = hal_color_hex(0x1c244fu);
    t->colors[HAL_THEME_PANEL_TITLE] = hal_color_hex(0x232c5eu);
    t->colors[HAL_THEME_BORDER] = hal_color_rgb8(0x38, 0x40, 0x74);
    t->colors[HAL_THEME_TEXT] = hal_color_hex(0xe8e4f8u);
    t->colors[HAL_THEME_TEXT_DIM] = hal_color_hex(0x9d97c2u);
    t->colors[HAL_THEME_ACCENT] = hal_color_hex(0x8b7ec8u);
    t->colors[HAL_THEME_ACCENT_TEXT] = hal_color_hex(0x151228u);
    t->colors[HAL_THEME_WIDGET] = hal_color_hex(0x2a3364u);
    t->colors[HAL_THEME_WIDGET_HOVER] = hal_color_hex(0x37417bu);
    t->colors[HAL_THEME_WIDGET_ACTIVE] = hal_color_hex(0x45508eu);
    t->colors[HAL_THEME_SELECT] = hal_color_rgba8(0x8b, 0x7e, 0xc8, 110);
    t->colors[HAL_THEME_VIEWPORT_BG] = hal_color_hex(0x10142eu);
    t->colors[HAL_THEME_VIEWPORT_GRID] = hal_color_rgb8(0x3a, 0x42, 0x70);
    t->colors[HAL_THEME_SUCCESS] = hal_color_hex(0x7ec8a4u);
    t->colors[HAL_THEME_WARNING] = hal_color_hex(0xc8b07eu);
    t->colors[HAL_THEME_DANGER] = hal_color_hex(0xc87e8bu);

    t->radius = 6.0f;
    t->padding = 10.0f;
    t->gap = 6.0f;
    t->font_size = 16;
    t->font_size_small = 13;
    return t;
}

hal_theme *hal_theme_clone(const hal_theme *theme) {
    if (!theme)
        return NULL;
    hal_theme *t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    *t = *theme;
    t->name = malloc(strlen(theme->name) + 1);
    if (!t->name) {
        free(t);
        return NULL;
    }
    strcpy(t->name, theme->name);
    return t;
}

void hal_theme_destroy(hal_theme *theme) {
    if (!theme)
        return;
    free(theme->name);
    free(theme);
}

/* ---- .haltheme: strict parser ----
 *
 * Grammar (see docs/themes.md):
 *   halcyon-theme <version>
 *   name "Display Name"
 *   color <token> #RRGGBB[AA]
 *   metric <token> <number>
 */

static void theme_set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Parses #RRGGBB or #RRGGBBAA into a color. */
static bool parse_hex_color(const char *word, hal_color *out) {
    size_t n = strlen(word);
    if (n != 7 && n != 9)
        return false;
    if (word[0] != '#')
        return false;
    hal_u32 v = 0;
    for (size_t i = 1; i < n; i++) {
        char c = word[i];
        hal_u32 d;
        if (c >= '0' && c <= '9')
            d = (hal_u32)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (hal_u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (hal_u32)(c - 'A' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    if (n == 7)
        *out = hal_color_hex(v);
    else
        *out = hal_color_rgba8((hal_u8)((v >> 24) & 0xFFu),
                               (hal_u8)((v >> 16) & 0xFFu),
                               (hal_u8)((v >> 8) & 0xFFu),
                               (hal_u8)(v & 0xFFu));
    return true;
}

/* Duplicate-token tracking is per load call (thread-of-control local,
 * no static state). */
static bool s_token_seen[HAL_THEME__COLOR_COUNT];

hal_theme *hal_theme_load(const char *path, char *err, size_t err_cap) {
    if (!path) {
        theme_set_err(err, err_cap, "invalid arguments");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        theme_set_err(err, err_cap, "cannot read %s: %s", path,
                      strerror(errno));
        return NULL;
    }

    char line[512];
    int line_no = 0;
    hal_theme *theme = NULL;
    bool seen_metrics[METRIC__COUNT] = { false };

    while (fgets(line, sizeof(line), f)) {
        line_no++;

        /* Strip the trailing newline and any carriage return. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            continue; /* blank line */

        /* First token is the key; the value stays in the remainder (a
         * quoted name may contain spaces — strtok would split it). */
        char *save = NULL;
        char *key = strtok_r(p, " \t", &save);
        if (!key)
            continue;

        if (line_no == 1) {
            /* Header: 'halcyon-theme <version>'. */
            char *version_tok = strtok_r(NULL, " \t", &save);
            if (!version_tok ||
                strcmp(key, "halcyon-theme") != 0 ||
                strtok_r(NULL, " \t", &save) != NULL) {
                theme_set_err(err, err_cap,
                              "line 1: not a halcyon theme (expected "
                              "'halcyon-theme <version>')");
                goto fail;
            }
            int version = atoi(version_tok);
            if (version != HAL_THEME_FORMAT_VERSION) {
                theme_set_err(err, err_cap,
                              "line 1: theme version %s not supported "
                              "(this Halcyon reads version %d)",
                              version_tok, HAL_THEME_FORMAT_VERSION);
                goto fail;
            }
            /* Start from the default so a partial theme only overrides
             * what it spells. */
            theme = hal_theme_default();
            if (!theme) {
                theme_set_err(err, err_cap, "out of memory");
                goto fail;
            }
            free(theme->name);
            theme->name = NULL;
            continue;
        }

        if (!theme) {
            theme_set_err(err, err_cap, "line %d: missing theme header",
                          line_no);
            goto fail;
        }

        if (strcmp(key, "name") == 0) {
            /* One quoted string; internal spaces allowed. */
            char *rest = save;
            while (rest && (*rest == ' ' || *rest == '\t'))
                rest++;
            size_t rlen = rest ? strlen(rest) : 0;
            if (!rest || rlen < 2 || rest[0] != '"' ||
                rest[rlen - 1] != '"') {
                theme_set_err(err, err_cap,
                              "line %d: name wants one \"quoted\" string",
                              line_no);
                goto fail;
            }
            rest[rlen - 1] = '\0';
            char *name = malloc(strlen(rest + 1) + 1);
            if (!name) {
                theme_set_err(err, err_cap, "out of memory");
                goto fail;
            }
            strcpy(name, rest + 1);
            free(theme->name);
            theme->name = name;
            continue;
        }

        /* Everything else: key + space-separated fields. */
        char *argv_[2];
        int argc = 0;
        for (char *tok = strtok_r(NULL, " \t", &save); tok && argc < 2;
             tok = strtok_r(NULL, " \t", &save))
            argv_[argc++] = tok;
        if (strtok_r(NULL, " \t", &save))
            argc = 3; /* trailing junk marker */

        if (strcmp(key, "color") == 0) {
            if (argc != 2) {
                theme_set_err(err, err_cap,
                              "line %d: color wants: color <token> "
                              "#rrggbb[aa]",
                              line_no);
                goto fail;
            }
            bool found = false;
            for (size_t i = 0;
                 i < sizeof(COLOR_NAMES) / sizeof(COLOR_NAMES[0]); i++) {
                if (strcmp(COLOR_NAMES[i].name, argv_[0]) == 0) {
                    if (s_token_seen[COLOR_NAMES[i].token]) {
                        theme_set_err(err, err_cap,
                                      "line %d: duplicate color '%s'",
                                      line_no, argv_[0]);
                        goto fail;
                    }
                    if (!parse_hex_color(
                            argv_[1],
                            &theme->colors[COLOR_NAMES[i].token])) {
                        theme_set_err(err, err_cap,
                                      "line %d: '%s' is not #rrggbb[aa]",
                                      line_no, argv_[1]);
                        goto fail;
                    }
                    s_token_seen[COLOR_NAMES[i].token] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                theme_set_err(err, err_cap,
                              "line %d: unknown color '%s'", line_no,
                              argv_[0]);
                goto fail;
            }
        } else if (strcmp(key, "metric") == 0) {
            if (argc != 2) {
                theme_set_err(err, err_cap,
                              "line %d: metric wants: metric <token> "
                              "<number>",
                              line_no);
                goto fail;
            }
            int which = -1;
            for (int i = 0; i < METRIC__COUNT; i++)
                if (strcmp(metric_names[i], argv_[0]) == 0)
                    which = i;
            if (which < 0) {
                theme_set_err(err, err_cap,
                              "line %d: unknown metric '%s'", line_no,
                              argv_[0]);
                goto fail;
            }
            if (seen_metrics[which]) {
                theme_set_err(err, err_cap,
                              "line %d: duplicate metric '%s'", line_no,
                              argv_[0]);
                goto fail;
            }
            char *end = NULL;
            double v = strtod(argv_[1], &end);
            if (!end || *end != '\0' || end == argv_[1]) {
                theme_set_err(err, err_cap,
                              "line %d: '%s' is not a number", line_no,
                              argv_[1]);
                goto fail;
            }
            switch (which) {
            case METRIC_RADIUS: theme->radius = (hal_f32)v; break;
            case METRIC_PADDING: theme->padding = (hal_f32)v; break;
            case METRIC_GAP: theme->gap = (hal_f32)v; break;
            case METRIC_FONT: theme->font_size = (hal_i32)v; break;
            case METRIC_FONT_SMALL:
                theme->font_size_small = (hal_i32)v;
                break;
            }
            seen_metrics[which] = true;
        } else {
            theme_set_err(err, err_cap, "line %d: unknown key '%s'",
                          line_no, key);
            goto fail;
        }
    }

    fclose(f);

    if (!theme) {
        theme_set_err(err, err_cap, "empty theme file");
        return NULL;
    }
    if (!theme->name) {
        theme->name = malloc(strlen("Unnamed") + 1);
        if (!theme->name) {
            hal_theme_destroy(theme);
            theme_set_err(err, err_cap, "out of memory");
            return NULL;
        }
        strcpy(theme->name, "Unnamed");
    }

    /* Reset the duplicate-tracking for the next load. */
    for (int i = 0; i < HAL_THEME__COLOR_COUNT; i++)
        s_token_seen[i] = false;

    return theme;

fail:
    fclose(f);
    hal_theme_destroy(theme);
    for (int i = 0; i < HAL_THEME__COLOR_COUNT; i++)
        s_token_seen[i] = false;
    return NULL;
}

bool hal_theme_save(const hal_theme *theme, const char *path) {
    if (!theme || !path)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    bool ok = fprintf(f, "halcyon-theme %d\n", HAL_THEME_FORMAT_VERSION) > 0;
    ok = ok && fprintf(f, "name \"%s\"\n", theme->name) > 0;

    for (size_t i = 0;
         ok && i < sizeof(COLOR_NAMES) / sizeof(COLOR_NAMES[0]); i++) {
        hal_color c = theme->colors[COLOR_NAMES[i].token];
        ok = ok &&
             fprintf(f, "color %s #%02x%02x%02x%02x\n", COLOR_NAMES[i].name,
                     (unsigned)(c.r * 255.0f + 0.5f),
                     (unsigned)(c.g * 255.0f + 0.5f),
                     (unsigned)(c.b * 255.0f + 0.5f),
                     (unsigned)(c.a * 255.0f + 0.5f)) > 0;
    }

    ok = ok && fprintf(f, "metric radius %.9g\n", (double)theme->radius) > 0;
    ok = ok && fprintf(f, "metric padding %.9g\n", (double)theme->padding) > 0;
    ok = ok && fprintf(f, "metric gap %.9g\n", (double)theme->gap) > 0;
    ok = ok && fprintf(f, "metric font_size %d\n", theme->font_size) > 0;
    ok = ok && fprintf(f, "metric font_size_small %d\n",
                       theme->font_size_small) > 0;

    return fclose(f) == 0 && ok;
}
