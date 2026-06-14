#include "ui_common.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "touch.h"

static void draw_scroll_chevron(int cx, int y, bool up, uint16_t color) {
    for (int i = 0; i < 5; i++) {
        int row = up ? y + i : y + 4 - i;
        display_hline(cx - i, row, i * 2 + 1, color);
    }
}

void ui_draw_header(const char *title, bool show_back) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, UI_HEADER_HEIGHT, UI_COLOR_HEADER);

    int len = strlen(title);
    int x = (DISPLAY_WIDTH - len * FONT_CHAR_WIDTH) / 2;
    display_string(x, UI_HEADER_TEXT_Y, title, COLOR_WHITE, UI_COLOR_HEADER);

    if (show_back) {
        display_fill_rect(UI_BACK_BTN_X, 5, UI_BACK_BTN_W, 20, UI_COLOR_ITEM_BG);
        display_string(UI_BACK_BTN_X + 10, UI_HEADER_TEXT_Y, "Back", COLOR_WHITE, UI_COLOR_ITEM_BG);
    }
}

void ui_draw_centered_string(int16_t y, const char *str, uint16_t fg, uint16_t bg, bool scale_2x) {
    int char_width = scale_2x ? FONT_CHAR_WIDTH_2X : FONT_CHAR_WIDTH;
    int char_height = scale_2x ? FONT_CHAR_HEIGHT_2X : FONT_CHAR_HEIGHT;
    int max_chars = DISPLAY_WIDTH / char_width;
    char clipped[DISPLAY_WIDTH / FONT_CHAR_WIDTH + 1];

    int len = (int)strlen(str);
    if (len > max_chars) {
        if (max_chars > 3) {
            memcpy(clipped, str, (size_t)(max_chars - 3));
            memcpy(clipped + max_chars - 3, "...", 3);
        } else {
            memcpy(clipped, str, (size_t)max_chars);
        }
        clipped[max_chars] = '\0';
        str = clipped;
        len = max_chars;
    }

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

void ui_draw_list(const char **labels, int count, int scroll_offset, int selected) {
    display_fill_rect(0, UI_LIST_START_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - UI_LIST_START_Y, COLOR_BLACK);

    for (int i = 0; i < UI_LIST_VISIBLE && (i + scroll_offset) < count; i++) {
        int idx = i + scroll_offset;
        int y = UI_LIST_START_Y + i * UI_LIST_ITEM_H;

        uint16_t bg = (idx == selected) ? UI_COLOR_SELECTED : COLOR_BLACK;
        uint16_t fg = (idx == selected) ? COLOR_BLACK : COLOR_WHITE;

        display_fill_rect(0, y, DISPLAY_WIDTH, UI_LIST_ITEM_H - 2, bg);
        display_string(10, y + 6, labels[idx], fg, bg);
    }

    // Scroll indicators: keep them inside the list gutter so they never
    // paint over the header or into the tap zones above/below the list.
    const int arrow_x = DISPLAY_WIDTH - 9;
    if (scroll_offset > 0) {
        draw_scroll_chevron(arrow_x, UI_LIST_START_Y + 2, true, COLOR_GRAY);
    }
    if (scroll_offset + UI_LIST_VISIBLE < count) {
        draw_scroll_chevron(arrow_x,
                            UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H - 7,
                            false, COLOR_GRAY);
    }
}

int ui_list_clamp_scroll(int scroll, int count) {
    int max_scroll = count - UI_LIST_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll < 0) return 0;
    if (scroll > max_scroll) return max_scroll;
    return scroll;
}

int ui_list_scroll_to_item(int item, int count) {
    if (item < 0) return 0;
    return ui_list_clamp_scroll(item - UI_LIST_VISIBLE / 2, count);
}

void ui_list_touch_reset(ui_list_touch_t *state) {
    if (!state) return;
    state->was_pressed = false;
    state->drag_tracking = false;
    state->drag_moved = false;
}

static int list_drag_scroll_delta(int delta_y) {
    if (delta_y >= 0) return delta_y / UI_LIST_ITEM_H;
    return -((-delta_y) / UI_LIST_ITEM_H);
}

ui_list_touch_result_t ui_list_touch_update(ui_list_touch_t *state,
                                            const touch_point_t *touch,
                                            bool pressed,
                                            int count,
                                            int *scroll_offset) {
    if (!state || !scroll_offset) return UI_LIST_TOUCH_NONE;

    if (pressed && !state->was_pressed) {
        state->was_pressed = true;
        state->drag_tracking = touch && touch->y >= UI_LIST_START_Y
                               && touch->y < UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H;
        state->drag_moved = false;
        state->drag_start_y = touch ? touch->y : 0;
        state->drag_start_scroll = *scroll_offset;
        if (touch) state->tap_start = *touch;
        return UI_LIST_TOUCH_PRESSED;
    }

    if (pressed) {
        if (!touch || !state->drag_tracking) return UI_LIST_TOUCH_PRESSED;

        int delta_y = touch->y - state->drag_start_y;
        if (delta_y < -8 || delta_y > 8) {
            state->drag_moved = true;
        }

        int new_scroll = state->drag_start_scroll - list_drag_scroll_delta(delta_y);
        new_scroll = ui_list_clamp_scroll(new_scroll, count);
        if (new_scroll != *scroll_offset) {
            *scroll_offset = new_scroll;
            return UI_LIST_TOUCH_SCROLLED;
        }
        return UI_LIST_TOUCH_PRESSED;
    }

    if (!state->was_pressed) return UI_LIST_TOUCH_NONE;

    state->was_pressed = false;
    state->drag_tracking = false;
    return state->drag_moved ? UI_LIST_TOUCH_SCROLLED : UI_LIST_TOUCH_TAPPED;
}

bool ui_back_button_hit(const touch_point_t *touch) {
    return touch->y < UI_HEADER_HEIGHT && touch->x < UI_BACK_BTN_X + UI_BACK_BTN_W;
}

void ui_wait_for_touch_release(void) {
    // Feeds the task watchdog: an object resting on the touchscreen must
    // not look like a wedged main task.
    while (touch_is_pressed()) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
    }
}

bool ui_should_debounce(uint32_t last_time_ticks) {
    uint32_t now = xTaskGetTickCount();
    return (now - last_time_ticks) < pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS);
}

bool ui_read_touch(touch_point_t *touch, uint32_t *last_time_ticks) {
    bool touched = touch_read(touch);
    if (touched && ui_should_debounce(*last_time_ticks)) {
        return false;
    }
    if (touched) {
        *last_time_ticks = xTaskGetTickCount();
    }
    return touched;
}

void ui_draw_menu_item(int y, const char *label) {
    display_fill_rect(0, y, DISPLAY_WIDTH, UI_ITEM_HEIGHT - 3, UI_COLOR_ITEM_BG);
    display_string(10, y + UI_TEXT_Y_OFFSET, label, UI_COLOR_ITEM_FG, UI_COLOR_ITEM_BG);
    display_string(DISPLAY_WIDTH - 18, y + UI_TEXT_Y_OFFSET, ">", UI_COLOR_ITEM_FG, UI_COLOR_ITEM_BG);
}

void ui_draw_slider(int y, const char *label, uint8_t value, uint8_t max_value, uint16_t fill_color) {
    display_fill_rect(0, y, DISPLAY_WIDTH, UI_ITEM_HEIGHT - 3, UI_COLOR_ITEM_BG);
    display_string(10, y + UI_TEXT_Y_OFFSET, label, UI_COLOR_ITEM_FG, UI_COLOR_ITEM_BG);

    int bar_y = y + UI_TEXT_Y_OFFSET;
    display_fill_rect(UI_SLIDER_BAR_X, bar_y, UI_SLIDER_BAR_W, UI_SLIDER_BAR_H, COLOR_BLACK);
    display_rect(UI_SLIDER_BAR_X, bar_y, UI_SLIDER_BAR_W, UI_SLIDER_BAR_H, COLOR_GRAY);
    int fill_w = (value * (UI_SLIDER_BAR_W - 4)) / max_value;
    display_fill_rect(UI_SLIDER_BAR_X + 2, bar_y + 2, fill_w, UI_SLIDER_BAR_H - 4, fill_color);

    // Minus button
    display_fill_rect(UI_SLIDER_BTN_X1, y + 3, UI_SLIDER_BTN_W, UI_SLIDER_BTN_H, COLOR_GRAY);
    display_string(UI_SLIDER_BTN_X1 + 6, y + 4, "-", COLOR_WHITE, COLOR_GRAY);

    // Plus button
    display_fill_rect(UI_SLIDER_BTN_X2, y + 3, UI_SLIDER_BTN_W, UI_SLIDER_BTN_H, COLOR_GRAY);
    display_string(UI_SLIDER_BTN_X2 + 6, y + 4, "+", COLOR_WHITE, COLOR_GRAY);
}

void ui_fmt_duration(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60)         snprintf(buf, len, "%lus", (unsigned long)seconds);
    else if (seconds < 3600)  snprintf(buf, len, "%lum", (unsigned long)(seconds / 60));
    else if (seconds < 86400) snprintf(buf, len, "%luh", (unsigned long)(seconds / 3600));
    else                      snprintf(buf, len, "%lud", (unsigned long)(seconds / 86400));
}

void ui_fmt_duration_full(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60) {
        snprintf(buf, len, "%lus", (unsigned long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, len, "%lum %lus",
                 (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    } else if (seconds < 86400) {
        snprintf(buf, len, "%luh %lum %lus",
                 (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds % 3600) / 60),
                 (unsigned long)(seconds % 60));
    } else {
        snprintf(buf, len, "%lud %luh %lum",
                 (unsigned long)(seconds / 86400),
                 (unsigned long)((seconds % 86400) / 3600),
                 (unsigned long)((seconds % 3600) / 60));
    }
}
