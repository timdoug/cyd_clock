#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include <stdint.h>

// Keyboard key dimensions
#define KB_KEY_WIDTH    28
#define KB_KEY_HEIGHT   22
#define KB_KEY_SPACING  2

// Draw character key rows from a layout array
// layout: array of strings, one per row (e.g. "qwertyuiop")
// num_rows: number of rows in layout
// start_y: Y coordinate for top of keyboard
void ui_keyboard_draw_keys(const char **layout, int num_rows, int start_y,
                           uint16_t key_bg, uint16_t key_fg, uint16_t border_color);

// Get the character at a touch point within the key grid
// Returns the character, or 0 if touch is outside the grid
char ui_keyboard_get_key(const char **layout, int num_rows, int start_y,
                         int touch_x, int touch_y);

// Get the Y coordinate just below the last key row (for placing special keys)
static inline int ui_keyboard_bottom_y(int num_rows, int start_y) {
    return start_y + num_rows * (KB_KEY_HEIGHT + KB_KEY_SPACING);
}

#endif // UI_KEYBOARD_H
