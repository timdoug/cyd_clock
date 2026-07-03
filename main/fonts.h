#ifndef FONTS_H
#define FONTS_H

#include "display.h"

extern const uint8_t font_builtin[FONT_BUILTIN_COUNT][FONT_BUILTIN_GLYPH_BYTES];
extern const uint8_t font_builtin_2x[FONT_BUILTIN_COUNT][FONT_BUILTIN_GLYPH_2X_BYTES];

extern const display_glyph_font_t font_ja;
extern const display_glyph_font_t font_zh;
extern const display_glyph_font_t font_zh_hant;
extern const display_glyph_font_t font_ko;
extern const display_glyph_font_t font_el;
extern const display_glyph_font_t font_vi;

#endif
