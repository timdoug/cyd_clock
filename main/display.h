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

// Optional 16x16 supplemental glyph table for compact CJK UI strings.
// Strings reference these glyphs with DISPLAY_CJK_ESCAPE followed by
// 0x80 + glyph-index. Keeping payload bytes out of ASCII avoids accidental
// '%' format specifiers in translated snprintf format strings.
#define DISPLAY_CJK_ESCAPE '\x1E'
#define DISPLAY_CJK_GLYPH_WIDTH 16
#define DISPLAY_CJK_GLYPH_HEIGHT 16
#define DISPLAY_CJK_GLYPH_BYTES 32
#define DISPLAY_CJK_GLYPH_2X_WIDTH 32
#define DISPLAY_CJK_GLYPH_2X_HEIGHT 32
#define DISPLAY_CJK_GLYPH_2X_BYTES 128

typedef struct display_cjk_font {
    const uint8_t (*glyphs)[DISPLAY_CJK_GLYPH_BYTES];
    const uint8_t (*glyphs_2x)[DISPLAY_CJK_GLYPH_2X_BYTES];
    uint16_t count;
    uint8_t glyph_width;
} display_cjk_font_t;

void display_init(void);

void display_fill(uint16_t color);

void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

void display_hline(int16_t x, int16_t y, int16_t w, uint16_t color);

void display_vline(int16_t x, int16_t y, int16_t h, uint16_t color);

void display_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

void display_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg);

void display_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);
void display_string_font(int16_t x, int16_t y, const char *str,
                         const display_cjk_font_t *font,
                         uint16_t fg, uint16_t bg);

void display_string_2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);

int display_text_width(const char *str);
int display_text_width_font(const char *str, const display_cjk_font_t *font);
void display_set_cjk_font(const display_cjk_font_t *font);

// size: 1=small (20x40), 2=medium (40x80), 3=large (60x120)
void display_digit_7seg(int16_t x, int16_t y, uint8_t digit, uint8_t size, uint16_t color, uint16_t bg);

void display_colon_7seg(int16_t x, int16_t y, uint8_t size, uint16_t color);

void display_set_backlight(uint8_t brightness);

void display_set_rotation(bool rotated);
bool display_is_rotated(void);

#endif
