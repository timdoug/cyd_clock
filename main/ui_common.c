#include "ui_common.h"
#include "display.h"
#include "touch.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

void ui_draw_header(const char *title, bool show_back) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, UI_HEADER_HEIGHT, UI_COLOR_HEADER);

    int len = strlen(title);
    int x = (DISPLAY_WIDTH - len * CHAR_WIDTH) / 2;
    display_string(x, UI_HEADER_TEXT_Y, title, COLOR_WHITE, UI_COLOR_HEADER);

    if (show_back) {
        display_fill_rect(5, 5, 50, 20, UI_COLOR_ITEM_BG);
        display_string(15, UI_HEADER_TEXT_Y, "Back", COLOR_WHITE, UI_COLOR_ITEM_BG);
    }
}

void ui_draw_centered_string(int16_t y, const char *str, uint16_t fg, uint16_t bg, bool scale_2x) {
    int len = strlen(str);
    int char_width = scale_2x ? CHAR_WIDTH_2X : CHAR_WIDTH;
    int char_height = scale_2x ? CHAR_HEIGHT_2X : CHAR_HEIGHT;
    int text_width = len * char_width;
    int x = (DISPLAY_WIDTH - text_width) / 2;

    // Fill left padding
    if (x > 0) {
        display_fill_rect(0, y, x, char_height, bg);
    }

    // Draw text
    if (scale_2x) {
        display_string_2x(x, y, str, fg, bg);
    } else {
        display_string(x, y, str, fg, bg);
    }

    // Fill right padding
    int right_x = x + text_width;
    if (right_x < DISPLAY_WIDTH) {
        display_fill_rect(right_x, y, DISPLAY_WIDTH - right_x, char_height, bg);
    }
}

void ui_wait_for_touch_release(void) {
    while (touch_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
    }
}

bool ui_should_debounce(uint32_t last_time_ticks) {
    uint32_t now = xTaskGetTickCount();
    return (now - last_time_ticks) < pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS);
}
