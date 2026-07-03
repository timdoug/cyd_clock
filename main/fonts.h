#ifndef FONTS_H
#define FONTS_H

#include "display.h"
#include "i18n.h"  // str_id_t for the pre-shaped string tables

// Base Menlo font: printable ASCII indexed by codepoint - 0x20, plus a
// sorted supplementary codepoint table for everything else the non-CJK
// languages use (Latin extensions, Cyrillic, Greek, Vietnamese, degree).
extern const uint8_t font_base_ascii_rle[];
extern const uint16_t font_base_ascii_off[];
extern const uint8_t font_base_ascii_2x_rle[];
extern const uint16_t font_base_ascii_2x_off[];
extern const uint16_t font_base_ext_cp[];
extern const uint8_t font_base_ext_rle[];
extern const uint16_t font_base_ext_off[];
extern const uint8_t font_base_ext_2x_rle[];
extern const uint16_t font_base_ext_2x_off[];
extern const uint16_t font_base_ext_count;

// Per-language glyph fonts: CJK (so shared Han codepoints render in the
// right regional face) plus scripts the base Menlo face does not cover.
extern const display_glyph_font_t font_ja;
extern const display_glyph_font_t font_zh;
extern const display_glyph_font_t font_zh_hant;
extern const display_glyph_font_t font_ko;
extern const display_glyph_font_t font_ka;
extern const display_glyph_font_t font_hy;

// Pre-shaped scripts: CoreText shapes these languages' strings at
// generation time (joining, ligatures, conjuncts, mark stacking, RTL
// reordering) into cluster glyphs with per-glyph widths; the generated
// tables below hold the shaped strings as private-use codepoint runs in
// visual order. The raw editable Unicode lives in i18n_data.inc under
// the shaped-source markers.
extern const display_glyph_font_t font_ar;
extern const display_glyph_font_t font_fa;
extern const display_glyph_font_t font_he;
extern const display_glyph_font_t font_hi;
extern const display_glyph_font_t font_bn;
extern const display_glyph_font_t font_th;
extern const display_glyph_font_t font_ur;
extern const display_glyph_font_t font_mr;
extern const display_glyph_font_t font_ta;
extern const display_glyph_font_t font_te;
extern const display_glyph_font_t font_pa;
extern const display_glyph_font_t font_gu;

extern const char *const lang_ar_shaped[];
extern const char *const weekdays_ar_shaped[7];
extern const char *const months_ar_shaped[12];
extern const char lang_name_ar_shaped[];
extern const char *const lang_fa_shaped[];
extern const char *const weekdays_fa_shaped[7];
extern const char *const months_fa_shaped[12];
extern const char lang_name_fa_shaped[];
extern const char *const lang_he_shaped[];
extern const char *const weekdays_he_shaped[7];
extern const char *const months_he_shaped[12];
extern const char lang_name_he_shaped[];
extern const char *const lang_hi_shaped[];
extern const char *const weekdays_hi_shaped[7];
extern const char *const months_hi_shaped[12];
extern const char lang_name_hi_shaped[];
extern const char *const lang_bn_shaped[];
extern const char *const weekdays_bn_shaped[7];
extern const char *const months_bn_shaped[12];
extern const char lang_name_bn_shaped[];
extern const char *const lang_th_shaped[];
extern const char *const weekdays_th_shaped[7];
extern const char *const months_th_shaped[12];
extern const char lang_name_th_shaped[];
extern const char *const lang_ur_shaped[];
extern const char *const weekdays_ur_shaped[7];
extern const char *const months_ur_shaped[12];
extern const char lang_name_ur_shaped[];
extern const char *const lang_mr_shaped[];
extern const char *const weekdays_mr_shaped[7];
extern const char *const months_mr_shaped[12];
extern const char lang_name_mr_shaped[];
extern const char *const lang_ta_shaped[];
extern const char *const weekdays_ta_shaped[7];
extern const char *const months_ta_shaped[12];
extern const char lang_name_ta_shaped[];
extern const char *const lang_te_shaped[];
extern const char *const weekdays_te_shaped[7];
extern const char *const months_te_shaped[12];
extern const char lang_name_te_shaped[];
extern const char *const lang_pa_shaped[];
extern const char *const weekdays_pa_shaped[7];
extern const char *const months_pa_shaped[12];
extern const char lang_name_pa_shaped[];
extern const char *const lang_gu_shaped[];
extern const char *const weekdays_gu_shaped[7];
extern const char *const months_gu_shaped[12];
extern const char lang_name_gu_shaped[];

#endif
