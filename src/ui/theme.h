/*
 * theme.h — Halcyon's theme system
 *
 * Themes are data: a token table of colors and metrics, with the
 * built-in default ("Faded Dream" — soft purples over deep blues) and
 * runtime-loadable `.haltheme` files. The parser is strict and
 * fail-closed like the project parser: unknown tokens, duplicates and
 * malformed values are line-numbered errors; a theme never applies
 * half-read.
 */

#ifndef HAL_THEME_H
#define HAL_THEME_H

#include "../platform/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_THEME_FORMAT_VERSION 1

/* Color tokens. Every widget resolves its paint through these — no
 * hardcoded colors anywhere in src/ui. */
typedef enum hal_theme_color_token {
    HAL_THEME_WINDOW = 0,      /* the window's base               */
    HAL_THEME_PANEL,           /* panel background                */
    HAL_THEME_PANEL_TITLE,     /* panel title band                */
    HAL_THEME_BORDER,          /* separators, outlines            */
    HAL_THEME_TEXT,            /* primary text                    */
    HAL_THEME_TEXT_DIM,        /* secondary text, labels          */
    HAL_THEME_ACCENT,          /* the dream-violet highlight      */
    HAL_THEME_ACCENT_TEXT,     /* text on accent                  */
    HAL_THEME_WIDGET,          /* button/field background         */
    HAL_THEME_WIDGET_HOVER,    /* hovered widget                  */
    HAL_THEME_WIDGET_ACTIVE,   /* pressed widget                  */
    HAL_THEME_SELECT,          /* selection band (lists, entries) */
    HAL_THEME_VIEWPORT_BG,     /* 3D viewport background          */
    HAL_THEME_VIEWPORT_GRID,   /* viewport grid lines             */
    HAL_THEME_SUCCESS,
    HAL_THEME_WARNING,
    HAL_THEME_DANGER,
    HAL_THEME__COLOR_COUNT,
} hal_theme_color_token;

typedef struct hal_theme {
    char *name;               /* owned; "Faded Dream" for the default */
    hal_color colors[HAL_THEME__COLOR_COUNT];

    /* Metrics (logical pixels). */
    hal_f32 radius;           /* widget corner radius               */
    hal_f32 padding;          /* inner padding                      */
    hal_f32 gap;              /* spacing between widgets            */
    hal_i32 font_size;        /* base UI font size                  */
    hal_i32 font_size_small;  /* dense text (tree rows, hints)       */
} hal_theme;

/* The built-in default (a copy the caller owns and must
 * hal_theme_destroy). */
hal_theme *hal_theme_default(void);

/* Deep copy. */
hal_theme *hal_theme_clone(const hal_theme *theme);

void hal_theme_destroy(hal_theme *theme);

/* Loads a .haltheme file. On failure returns NULL and (when err is
 * non-NULL) writes a line-numbered reason. */
hal_theme *hal_theme_load(const char *path, char *err, size_t err_cap);

/* Saves a theme as a .haltheme file (round-trippable). */
bool hal_theme_save(const hal_theme *theme, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HAL_THEME_H */
