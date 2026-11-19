# Halcyon themes (.haltheme, format version 1)

Halcyon's look is data. A theme file is a strict, line-oriented text
file; the parser is fail-closed (unknown keys, duplicates, malformed
values and wrong versions are line-numbered errors — a theme never
applies half-read). A file only needs to spell what it CHANGES: a
partial theme starts from the built-in default and overrides the
tokens it lists.

## Grammar

```
file    := header name? (color | metric)*
header  := "halcyon-theme" WS version LF        (version is 1)
name    := "name" WS quoted-string LF           (may contain spaces)
color   := "color" WS token WS hexcolor LF
metric  := "metric" WS token WS number LF
hexcolor:= "#" rrggbb | "#" rrggbbaa            (lower/upper hex both fine)
```

Blank lines are allowed. `#rrggbbaa` sets the alpha too.

## Color tokens

| Token | Paints |
|---|---|
| `window` | the window's base color |
| `panel` | panel surfaces |
| `panel_title` | a panel's title band |
| `border` | separators and outlines |
| `text` | primary text |
| `text_dim` | secondary text (hints, shortcuts, panel titles) |
| `accent` | the highlight (slider fills, active states) |
| `accent_text` | text drawn ON accent |
| `widget` | button/field background |
| `widget_hover` | hovered widget |
| `widget_active` | pressed widget |
| `select` | selection bands (tree rows, menu items) |
| `viewport_bg` | the 3D viewport's background |
| `viewport_grid` | the viewport grid |
| `success` / `warning` / `danger` | status semantics |

## Metric tokens

`radius` (widget corner radius), `padding`, `gap` (spacing between
widgets), `font_size` (base UI text), `font_size_small` (dense text:
tree rows, hints) — all logical pixels.

## Example

```
halcyon-theme 1
name "Deep Dusk"
color window #0d1126
color accent #a78bfa
metric radius 8
metric font_size 15
```

## The built-in face: Faded Dream

Soft purples over deep blues, the Faded Dream palette — the default
every unoverridden token inherits. `hal_theme_save` writes it out (the
full file, all tokens), and any saved theme loads back byte-equal
(tested by test_ui's round trip).

## Loading

Themes load at run time (`hal_theme_load`, then `hal_ui_set_theme` —
the UI repaints from the new table immediately). A preferences-driven
theme picker arrives with M11.
