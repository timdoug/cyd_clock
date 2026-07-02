#include "ui_common.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include "i18n.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "touch.h"

int ui_marquee_window(char *out, int width, const char *s, int scroll) {
    int len = (int)strlen(s);
    if (len <= width) {
        for (int i = 0; i < width; i++) out[i] = i < len ? s[i] : ' ';
        out[width] = '\0';
        return 1;
    }
    int period = len + UI_MARQUEE_GAP;
    for (int i = 0; i < width; i++) {
        int idx = (scroll % period + i) % period;
        out[i] = idx < len ? s[idx] : ' ';
    }
    out[width] = '\0';
    return period;
}

bool ui_marquee_advance(int *scroll, int *dwell, int width, const char *s) {
    int len = (int)strlen(s);
    if (len <= width) {            // fits: nothing to scroll
        *scroll = 0;
        *dwell = 0;
        return false;
    }
    int period = len + UI_MARQUEE_GAP;
    int end = len - width;         // scroll with the last char flush right
    if ((*scroll == 0 || *scroll == end) && *dwell < UI_MARQUEE_DWELL) {
        (*dwell)++;
        return false;
    }
    *dwell = 0;
    *scroll = (*scroll + 1) % period;
    return true;
}

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
        const char *back = tr(STR_BACK);
        int bx = UI_BACK_BTN_X + (UI_BACK_BTN_W - (int)strlen(back) * FONT_CHAR_WIDTH) / 2;
        display_fill_rect(UI_BACK_BTN_X, 5, UI_BACK_BTN_W, 20, UI_COLOR_ITEM_BG);
        display_string(bx, UI_HEADER_TEXT_Y, back, COLOR_WHITE, UI_COLOR_ITEM_BG);
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

    if (x > 0) {
        display_fill_rect(0, y, x, char_height, bg);
    }

    if (scale_2x) {
        display_string_2x(x, y, str, fg, bg);
    } else {
        display_string(x, y, str, fg, bg);
    }

    int right_x = x + text_width;
    if (right_x < DISPLAY_WIDTH) {
        display_fill_rect(right_x, y, DISPLAY_WIDTH - right_x, char_height, bg);
    }
}

void ui_draw_list(const char **labels, int count, int scroll_offset, int selected) {
    int rows = 0;
    for (int i = 0; i < UI_LIST_VISIBLE && (i + scroll_offset) < count; i++, rows++) {
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

    int list_bottom = UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H;
    if (rows < UI_LIST_VISIBLE) {
        int clear_y = UI_LIST_START_Y + rows * UI_LIST_ITEM_H;
        display_fill_rect(0, clear_y, DISPLAY_WIDTH, list_bottom - clear_y, COLOR_BLACK);
    }
    if (list_bottom < DISPLAY_HEIGHT) {
        display_fill_rect(0, list_bottom, DISPLAY_WIDTH, DISPLAY_HEIGHT - list_bottom, COLOR_BLACK);
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
    state->redraw_pending = false;
    state->last_redraw_ticks = 0;
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
        state->redraw_pending = false;
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
            state->redraw_pending = true;

            uint32_t now = xTaskGetTickCount();
            if (state->last_redraw_ticks == 0
                || now - state->last_redraw_ticks >= pdMS_TO_TICKS(UI_LIST_SCROLL_REDRAW_MS)) {
                state->last_redraw_ticks = now;
                state->redraw_pending = false;
                return UI_LIST_TOUCH_SCROLLED;
            }
        }
        return UI_LIST_TOUCH_PRESSED;
    }

    if (!state->was_pressed) return UI_LIST_TOUCH_NONE;

    state->was_pressed = false;
    state->drag_tracking = false;
    if (state->drag_moved) {
        bool needs_redraw = state->redraw_pending;
        state->redraw_pending = false;
        return needs_redraw ? UI_LIST_TOUCH_SCROLLED : UI_LIST_TOUCH_NONE;
    }
    return UI_LIST_TOUCH_TAPPED;
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
    ui_draw_slider_value(y, value, max_value, fill_color);

    display_fill_rect(UI_SLIDER_BTN_X1, y + 3, UI_SLIDER_BTN_W, UI_SLIDER_BTN_H, COLOR_GRAY);
    display_string(UI_SLIDER_BTN_X1 + 6, y + 4, "-", COLOR_WHITE, COLOR_GRAY);

    display_fill_rect(UI_SLIDER_BTN_X2, y + 3, UI_SLIDER_BTN_W, UI_SLIDER_BTN_H, COLOR_GRAY);
    display_string(UI_SLIDER_BTN_X2 + 6, y + 4, "+", COLOR_WHITE, COLOR_GRAY);
}

void ui_draw_slider_value(int y, uint8_t value, uint8_t max_value, uint16_t fill_color) {
    int bar_y = y + UI_TEXT_Y_OFFSET;
    int inner_x = UI_SLIDER_BAR_X + 2;
    int inner_y = bar_y + 2;
    int inner_w = UI_SLIDER_BAR_W - 4;
    int inner_h = UI_SLIDER_BAR_H - 4;
    int fill_w = max_value ? (value * inner_w) / max_value : 0;

    display_fill_rect(inner_x, inner_y, inner_w, inner_h, COLOR_BLACK);
    if (fill_w > 0) {
        display_fill_rect(inner_x, inner_y, fill_w, inner_h, fill_color);
    }
}

void ui_draw_slider_value_delta(int y, uint8_t old_value, uint8_t new_value,
                                uint8_t max_value, uint16_t fill_color) {
    if (old_value == new_value) return;

    int bar_y = y + UI_TEXT_Y_OFFSET;
    int inner_x = UI_SLIDER_BAR_X + 2;
    int inner_y = bar_y + 2;
    int inner_w = UI_SLIDER_BAR_W - 4;
    int inner_h = UI_SLIDER_BAR_H - 4;
    int old_fill = max_value ? (old_value * inner_w) / max_value : 0;
    int new_fill = max_value ? (new_value * inner_w) / max_value : 0;

    if (new_fill > old_fill) {
        display_fill_rect(inner_x + old_fill, inner_y,
                          new_fill - old_fill, inner_h, fill_color);
    } else if (new_fill < old_fill) {
        display_fill_rect(inner_x + new_fill, inner_y,
                          old_fill - new_fill, inner_h, COLOR_BLACK);
    }
}

void ui_fmt_offset_us(char *buf, size_t len, int64_t us) {
    char sign = (us < 0) ? '-' : '+';
    int64_t av = us < 0 ? -us : us;

    // Each bucket is chosen AFTER rounding to its precision, so a value that
    // rounds up to the next width falls into the next bucket instead of
    // overflowing the 7-char cap (9999 us used to render as "+10.00ms").
    // Sub-millisecond values render in ms too ("+0.99ms") so the column
    // reads in a single unit.
    int64_t hund_ms = (av + 5) / 10;
    if (hund_ms < 1000) {
        snprintf(buf, len, "%c%lld.%02lldms", sign,
                 (long long)(hund_ms / 100), (long long)(hund_ms % 100));
        return;
    }
    int64_t tenth_ms = (av + 50) / 100;
    if (tenth_ms < 1000) {
        snprintf(buf, len, "%c%lld.%lldms", sign,
                 (long long)(tenth_ms / 10), (long long)(tenth_ms % 10));
        return;
    }
    int64_t ms = (av + 500) / 1000;
    if (ms < 10000) {
        snprintf(buf, len, "%c%lldms", sign, (long long)ms);
        return;
    }
    int64_t tenth_s = (av + 50000) / 100000;
    if (tenth_s < 1000) {
        snprintf(buf, len, "%c%lld.%llds", sign,
                 (long long)(tenth_s / 10), (long long)(tenth_s % 10));
        return;
    }
    snprintf(buf, len, "%c%llds", sign, (long long)((av + 500000) / 1000000));
}

void ui_fmt_signed_x1000(char *buf, size_t len, int32_t val_x1000, const char *unit) {
    char sign = (val_x1000 < 0) ? '-' : '+';
    uint32_t av = val_x1000 < 0 ? (uint32_t)-val_x1000 : (uint32_t)val_x1000;

    uint32_t hund = (av + 5) / 10;
    if (hund < 1000) {
        snprintf(buf, len, "%c%lu.%02lu%s", sign,
                 (unsigned long)(hund / 100), (unsigned long)(hund % 100), unit);
        return;
    }
    uint32_t tenth = (av + 50) / 100;
    if (tenth < 1000) {
        snprintf(buf, len, "%c%lu.%lu%s", sign,
                 (unsigned long)(tenth / 10), (unsigned long)(tenth % 10), unit);
        return;
    }
    snprintf(buf, len, "%c%lu%s", sign, (unsigned long)((av + 500) / 1000), unit);
}

void ui_fmt_duration_full(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60) {
        snprintf(buf, len, "%lus", (unsigned long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, len, "%lum %lus",
                 (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    } else if (seconds < 86400) {
        snprintf(buf, len, "%luh %lum",
                 (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds % 3600) / 60));
    } else {
        snprintf(buf, len, "%lud %luh",
                 (unsigned long)(seconds / 86400),
                 (unsigned long)((seconds % 86400) / 3600));
    }
}
