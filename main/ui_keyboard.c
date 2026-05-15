#include "ui_keyboard.h"
#include <string.h>
#include "display.h"

void ui_keyboard_draw_keys(const char **layout, int num_rows, int start_y,
                           uint16_t key_bg, uint16_t key_fg, uint16_t border_color) {
    int y = start_y;

    for (int row = 0; row < num_rows; row++) {
        const char *keys = layout[row];
        int row_len = strlen(keys);
        int x = (DISPLAY_WIDTH - row_len * (KB_KEY_WIDTH + KB_KEY_SPACING)) / 2;

        for (int col = 0; col < row_len; col++) {
            display_fill_rect(x, y, KB_KEY_WIDTH, KB_KEY_HEIGHT, key_bg);
            display_rect(x, y, KB_KEY_WIDTH, KB_KEY_HEIGHT, border_color);
            display_char(x + 10, y + 3, keys[col], key_fg, key_bg);
            x += KB_KEY_WIDTH + KB_KEY_SPACING;
        }
        y += KB_KEY_HEIGHT + KB_KEY_SPACING;
    }
}

char ui_keyboard_get_key(const char **layout, int num_rows, int start_y,
                         int touch_x, int touch_y) {
    if (touch_y < start_y) return 0;

    int row_pitch = KB_KEY_HEIGHT + KB_KEY_SPACING;
    int row = (touch_y - start_y) / row_pitch;
    if (row >= num_rows) return 0;
    if ((touch_y - start_y) % row_pitch >= KB_KEY_HEIGHT) return 0;

    const char *keys = layout[row];
    int row_len = strlen(keys);
    int row_start = (DISPLAY_WIDTH - row_len * (KB_KEY_WIDTH + KB_KEY_SPACING)) / 2;

    if (touch_x < row_start) return 0;

    int col_pitch = KB_KEY_WIDTH + KB_KEY_SPACING;
    int col = (touch_x - row_start) / col_pitch;
    if (col >= row_len) return 0;
    if ((touch_x - row_start) % col_pitch >= KB_KEY_WIDTH) return 0;

    return keys[col];
}
