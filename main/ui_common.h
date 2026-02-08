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

// Common list layout
#define UI_LIST_ITEM_H      28
#define UI_LIST_START_Y     35
#define UI_LIST_VISIBLE     6

// Common UI colors
#define UI_COLOR_HEADER     0x001F  // COLOR_BLUE
#define UI_COLOR_ITEM_BG    0x4208  // COLOR_DARKGRAY
#define UI_COLOR_ITEM_FG    0xFFFF  // COLOR_WHITE
#define UI_COLOR_SELECTED   0x07FF  // COLOR_CYAN

// Virtual key codes for on-screen keyboards
#define VKEY_SHIFT      '\x01'
#define VKEY_MODE       '\x02'
#define VKEY_BACKSPACE  '\x08'
#define VKEY_ENTER      '\x0D'
#define VKEY_ESCAPE     '\x1B'

// Slider layout constants (for hit-testing in callers)
#define UI_SLIDER_BAR_X     100
#define UI_SLIDER_BAR_W     150
#define UI_SLIDER_BAR_H     14
#define UI_SLIDER_BTN_X1    260   // Minus button x
#define UI_SLIDER_BTN_X2    288   // Plus button x
#define UI_SLIDER_BTN_W     22
#define UI_SLIDER_BTN_H     18

// Draw a standard header bar with centered title
// If show_back is true, shows a "Back" button on the left
void ui_draw_header(const char *title, bool show_back);

// Draw a menu item with label and ">" arrow
void ui_draw_menu_item(int y, const char *label);

// Draw a labeled slider with +/- buttons
void ui_draw_slider(int y, const char *label, uint8_t value, uint8_t max_value, uint16_t fill_color);

// Draw a centered string with full-width background (no pre-clear needed)
// height: 16 for 1x scale, 32 for 2x scale
void ui_draw_centered_string(int16_t y, const char *str, uint16_t fg, uint16_t bg, bool scale_2x);

// Draw a scrollable list with selection highlight and scroll indicators
void ui_draw_list(const char **labels, int count, int scroll_offset, int selected);

// Wait for touch release (blocks until finger lifted)
void ui_wait_for_touch_release(void);

// Check if touch should be debounced (returns true if too soon since last_time)
bool ui_should_debounce(uint32_t last_time_ticks);

#endif // UI_COMMON_H
