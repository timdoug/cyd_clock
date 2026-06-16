#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_Y      120

#define KB_KEY_WIDTH    28
#define KB_KEY_HEIGHT   22
#define KB_KEY_SPACING  2

void ui_keyboard_draw_keys(const char **layout, int num_rows, int start_y,
                           uint16_t key_bg, uint16_t key_fg, uint16_t border_color);

char ui_keyboard_get_key(const char **layout, int num_rows, int start_y,
                         int touch_x, int touch_y);

static inline int ui_keyboard_bottom_y(int num_rows, int start_y) {
    return start_y + num_rows * (KB_KEY_HEIGHT + KB_KEY_SPACING);
}

#endif
