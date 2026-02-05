#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <stdint.h>
#include <stdbool.h>

// Common UI layout constants
#define UI_HEADER_HEIGHT    30
#define UI_ITEM_HEIGHT      26
#define UI_ITEM_MARGIN      10
#define UI_TEXT_Y_OFFSET    5
#define UI_HEADER_TEXT_Y    8

// Common UI colors
#define UI_COLOR_HEADER     0x001F  // COLOR_BLUE
#define UI_COLOR_ITEM_BG    0x4208  // COLOR_DARKGRAY
#define UI_COLOR_ITEM_FG    0xFFFF  // COLOR_WHITE
#define UI_COLOR_SELECTED   0x07FF  // COLOR_CYAN

// Draw a standard header bar with centered title
// If show_back is true, shows a "Back" button on the left
void ui_draw_header(const char *title, bool show_back);

// Draw a centered string with full-width background (no pre-clear needed)
// height: 16 for 1x scale, 32 for 2x scale
void ui_draw_centered_string(int16_t y, const char *str, uint16_t fg, uint16_t bg, bool scale_2x);

// Wait for touch release (blocks until finger lifted)
void ui_wait_for_touch_release(void);

// Check if touch should be debounced (returns true if too soon since last_time)
bool ui_should_debounce(uint32_t last_time_ticks);

#endif // UI_COMMON_H
