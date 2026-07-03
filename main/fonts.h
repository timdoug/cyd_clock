#ifndef FONTS_H
#define FONTS_H

#include "display.h"

// Base Menlo font: printable ASCII indexed by codepoint - 0x20, plus a
// sorted supplementary codepoint table for everything else the non-CJK
// languages use (Latin extensions, Cyrillic, Greek, Vietnamese, degree).
extern const uint8_t font_base_ascii[][FONT_BASE_GLYPH_BYTES];
extern const uint8_t font_base_ascii_2x[][FONT_BASE_GLYPH_2X_BYTES];
extern const uint16_t font_base_ext_cp[];
extern const uint8_t font_base_ext[][FONT_BASE_GLYPH_BYTES];
extern const uint8_t font_base_ext_2x[][FONT_BASE_GLYPH_2X_BYTES];
extern const uint16_t font_base_ext_count;

// Per-language glyph fonts: CJK only, so shared Han codepoints render in
// the right regional face.
extern const display_glyph_font_t font_ja;
extern const display_glyph_font_t font_zh;
extern const display_glyph_font_t font_zh_hant;
extern const display_glyph_font_t font_ko;

#endif
