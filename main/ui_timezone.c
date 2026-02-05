#include "ui_timezone.h"
#include "display.h"
#include "touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_timezone";

// Timezone definitions: {display_name, POSIX_TZ_string}
typedef struct {
    const char *name;
    const char *tz;
} timezone_entry_t;

static const timezone_entry_t timezones[] = {
    {"UTC",                     "UTC0"},
    {"US Pacific (LA)",         "PST8PDT,M3.2.0,M11.1.0"},
    {"US Mountain (Denver)",    "MST7MDT,M3.2.0,M11.1.0"},
    {"US Central (Chicago)",    "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern (NYC)",        "EST5EDT,M3.2.0,M11.1.0"},
    {"US Hawaii",               "HST10"},
    {"US Alaska",               "AKST9AKDT,M3.2.0,M11.1.0"},
    {"UK (London)",             "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe Central",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe Eastern",          "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Japan (Tokyo)",           "JST-9"},
    {"China (Beijing)",         "CST-8"},
    {"India",                   "IST-5:30"},
    {"Australia Eastern",       "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia Central",       "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Western",       "AWST-8"},
    {"New Zealand",             "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

#define NUM_TIMEZONES (sizeof(timezones) / sizeof(timezones[0]))

// Layout
#define HEADER_HEIGHT 30
#define LIST_ITEM_H   28
#define LIST_START_Y  35
#define LIST_VISIBLE  7

// State
static int selected_tz = 0;
static int scroll_offset = 0;
static bool selection_made = false;
static uint32_t last_touch_time = 0;

static void draw_header(void) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_BLUE);
    display_string(80, 8, "Select Timezone", COLOR_WHITE, COLOR_BLUE);

    // Back button
    display_fill_rect(5, 5, 50, 20, COLOR_DARKGRAY);
    display_string(15, 8, "Back", COLOR_WHITE, COLOR_DARKGRAY);
}

static void draw_list(void) {
    display_fill_rect(0, LIST_START_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - LIST_START_Y, COLOR_BLACK);

    for (int i = 0; i < LIST_VISIBLE && (i + scroll_offset) < NUM_TIMEZONES; i++) {
        int idx = i + scroll_offset;
        int y = LIST_START_Y + i * LIST_ITEM_H;

        uint16_t bg = (idx == selected_tz) ? COLOR_CYAN : COLOR_BLACK;
        uint16_t fg = (idx == selected_tz) ? COLOR_BLACK : COLOR_WHITE;

        display_fill_rect(0, y, DISPLAY_WIDTH, LIST_ITEM_H - 2, bg);
        display_string(10, y + 6, timezones[idx].name, fg, bg);
    }

    // Scroll indicators
    if (scroll_offset > 0) {
        display_string(DISPLAY_WIDTH / 2 - 4, LIST_START_Y - 8, "^", COLOR_GRAY, COLOR_BLACK);
    }
    if (scroll_offset + LIST_VISIBLE < NUM_TIMEZONES) {
        display_string(DISPLAY_WIDTH / 2 - 4, LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H, "v", COLOR_GRAY, COLOR_BLACK);
    }
}

void ui_timezone_init(void) {
    ESP_LOGI(TAG, "Initializing timezone selector");
    selection_made = false;

    display_fill(COLOR_BLACK);
    draw_header();
    draw_list();
}

tz_select_result_t ui_timezone_update(void) {
    if (selection_made) {
        return TZ_SELECT_DONE;
    }

    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Debounce
    uint32_t now = xTaskGetTickCount();
    if (touched && (now - last_touch_time) < pdMS_TO_TICKS(200)) {
        touched = false;
    }
    if (touched) {
        last_touch_time = now;
    }

    if (!touched) {
        return TZ_SELECT_CONTINUE;
    }

    // Back button
    if (touch.y < HEADER_HEIGHT && touch.x < 60) {
        return TZ_SELECT_CANCELLED;
    }

    // List item touch
    if (touch.y >= LIST_START_Y && touch.y < LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H) {
        int item = (touch.y - LIST_START_Y) / LIST_ITEM_H + scroll_offset;
        if (item < NUM_TIMEZONES) {
            if (selected_tz == item) {
                // Double tap to select
                selection_made = true;
                return TZ_SELECT_DONE;
            } else {
                selected_tz = item;
                draw_list();
            }
        }
    }

    // Scroll up
    if (touch.y < LIST_START_Y && scroll_offset > 0) {
        scroll_offset--;
        draw_list();
    }

    // Scroll down
    if (touch.y > LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H) {
        if (scroll_offset + LIST_VISIBLE < NUM_TIMEZONES) {
            scroll_offset++;
            draw_list();
        }
    }

    return TZ_SELECT_CONTINUE;
}

const char *ui_timezone_get_selected(void) {
    return timezones[selected_tz].tz;
}

const char *ui_timezone_get_name(void) {
    return timezones[selected_tz].name;
}
