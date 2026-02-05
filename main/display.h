#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

// Display dimensions
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

// Common colors (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_CYAN    0x07FF
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20
#define COLOR_GRAY    0x8410
#define COLOR_DARKGRAY 0x4208

// Initialize the ILI9341 display
void display_init(void);

// Fill entire screen with color
void display_fill(uint16_t color);

// Fill rectangle
void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Draw single pixel
void display_pixel(int16_t x, int16_t y, uint16_t color);

// Draw horizontal line
void display_hline(int16_t x, int16_t y, int16_t w, uint16_t color);

// Draw vertical line
void display_vline(int16_t x, int16_t y, int16_t h, uint16_t color);

// Draw rectangle outline
void display_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Draw character (8x16 font)
void display_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg);

// Draw string
void display_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);

// Draw large 7-segment digit (for clock)
// size: 1=small (20x40), 2=medium (40x80), 3=large (60x120)
void display_digit_7seg(int16_t x, int16_t y, uint8_t digit, uint8_t size, uint16_t color, uint16_t bg);

// Draw colon for clock display
void display_colon_7seg(int16_t x, int16_t y, uint8_t size, uint16_t color, uint16_t bg);

// Set backlight (0-255)
void display_set_backlight(uint8_t brightness);

#endif // DISPLAY_H
