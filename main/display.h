#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_CYAN    0x07FF
#define COLOR_ORANGE  0xFD20
#define COLOR_GRAY    0xAD55
#define COLOR_DARKGRAY 0x4208

// All strings are UTF-8 (NFC). Rendering decodes codepoint by codepoint:
// printable ASCII indexes the base font directly, other codepoints binary
// search the active per-language glyph font (CJK) and then the base font's
// supplementary table (Latin extensions, Cyrillic, Greek, Vietnamese).
// Unknown codepoints and malformed bytes render as '?'. UTF-8 keeps the
// old private encoding's snprintf safety for free: no byte of a multi-byte
// sequence can be '%' or NUL.
//
// Glyph bitmaps are antialiased: 4-bit alpha per pixel, two pixels per
// byte, high nibble = left pixel, rows packed top to bottom. Draw paths
// blend fg toward bg through a 16-entry palette, which is why glyphs
// always repaint their full cell background.
#define FONT_BASE_ASCII_FIRST 0x20
#define FONT_BASE_ASCII_COUNT 95         // 0x20-0x7E
#define FONT_BASE_GLYPH_BYTES 64         // 8x16 px at 4 bits each

#define DISPLAY_GLYPH_WIDTH 16
#define DISPLAY_GLYPH_HEIGHT 16
#define DISPLAY_GLYPH_BYTES (DISPLAY_GLYPH_WIDTH * DISPLAY_GLYPH_HEIGHT / 2)
#define DISPLAY_GLYPH_2X_WIDTH 32
#define DISPLAY_GLYPH_2X_HEIGHT 32
#define DISPLAY_GLYPH_2X_BYTES (DISPLAY_GLYPH_2X_WIDTH * DISPLAY_GLYPH_2X_HEIGHT / 2)

// Glyph bitmaps are PackBits-compressed at both sizes: per font, a blob
// plus count+1 offsets; an empty 2x span (off[i] == off[i+1]) means the
// glyph has no 2x bitmap and is pixel-doubled from 1x instead.
typedef struct display_glyph_font {
    const uint16_t *codepoints;  // sorted ascending, parallel to glyphs
    const uint8_t *glyphs_rle;
    const uint16_t *glyphs_off;
    const uint8_t *glyphs_2x_rle;
    const uint16_t *glyphs_2x_off;
    const uint8_t *widths;       // per-glyph advances; NULL: uniform glyph_width
    uint16_t count;
    uint8_t glyph_width;
} display_glyph_font_t;

void display_init(void);

void display_fill(uint16_t color);

void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

void display_hline(int16_t x, int16_t y, int16_t w, uint16_t color);

void display_vline(int16_t x, int16_t y, int16_t h, uint16_t color);

void display_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

void display_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg);

void display_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);
void display_string_font(int16_t x, int16_t y, const char *str,
                         const display_glyph_font_t *font,
                         uint16_t fg, uint16_t bg);

void display_string_2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);

int display_text_width(const char *str);
int display_text_width_font(const char *str, const display_glyph_font_t *font);
void display_set_glyph_font(const display_glyph_font_t *font);

// One rendering cell of a UTF-8 string: returns the byte length of the
// sequence at s (>= 1, malformed bytes count as 1) and writes its 1x pixel
// width under the active glyph font.
int display_cell_at(const char *s, int *width_px);

// size: 1=small (20x40), 2=medium (40x80), 3=large (60x120)
void display_digit_7seg(int16_t x, int16_t y, uint8_t digit, uint8_t size, uint16_t color, uint16_t bg);

void display_colon_7seg(int16_t x, int16_t y, uint8_t size, uint16_t color);

void display_set_backlight(uint8_t brightness);

void display_set_rotation(bool rotated);
bool display_is_rotated(void);

#endif
