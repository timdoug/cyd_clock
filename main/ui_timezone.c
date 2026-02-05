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
    // UTC
    {"UTC (UTC+0)",                 "UTC0"},
    // Pacific / North America West
    {"Honolulu (UTC-10)",           "HST10"},
    {"Anchorage (UTC-9)",           "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Los Angeles (UTC-8)",         "PST8PDT,M3.2.0,M11.1.0"},
    {"Phoenix (UTC-7)",             "MST7"},
    {"Denver (UTC-7)",              "MST7MDT,M3.2.0,M11.1.0"},
    {"Mexico City (UTC-6)",         "CST6CDT,M4.1.0,M10.5.0"},
    {"Chicago (UTC-6)",             "CST6CDT,M3.2.0,M11.1.0"},
    {"New York (UTC-5)",            "EST5EDT,M3.2.0,M11.1.0"},
    {"Panama (UTC-5)",              "EST5"},
    {"Bogota (UTC-5)",              "COT5"},
    {"Lima (UTC-5)",                "PET5"},
    {"Halifax (UTC-4)",             "AST4ADT,M3.2.0,M11.1.0"},
    {"Santiago (UTC-4)",            "CLT4CLST,M9.1.0,M4.1.0"},
    {"St. John's (UTC-3:30)",       "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Sao Paulo (UTC-3)",           "BRT3"},
    {"Buenos Aires (UTC-3)",        "ART3"},
    // Atlantic / Europe / Africa
    {"Reykjavik (UTC+0)",           "GMT0"},
    {"London (UTC+0)",              "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Dublin (UTC+0)",              "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Lisbon (UTC+0)",              "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Casablanca (UTC+0)",          "WET0WEST,M3.5.0,M10.5.0"},
    {"Lagos (UTC+1)",               "WAT-1"},
    {"Paris (UTC+1)",               "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Berlin (UTC+1)",              "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Rome (UTC+1)",                "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Johannesburg (UTC+2)",        "SAST-2"},
    {"Cairo (UTC+2)",               "EET-2"},
    {"Athens (UTC+2)",              "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Jerusalem (UTC+2)",           "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Helsinki (UTC+2)",            "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Istanbul (UTC+3)",            "TRT-3"},
    {"Moscow (UTC+3)",              "MSK-3"},
    {"Nairobi (UTC+3)",             "EAT-3"},
    {"Riyadh (UTC+3)",              "AST-3"},
    {"Tehran (UTC+3:30)",           "IRST-3:30IRDT,J80/0,J264/0"},
    {"Dubai (UTC+4)",               "GST-4"},
    {"Karachi (UTC+5)",             "PKT-5"},
    {"Mumbai (UTC+5:30)",           "IST-5:30"},
    {"Kolkata (UTC+5:30)",          "IST-5:30"},
    {"Kathmandu (UTC+5:45)",        "NPT-5:45"},
    {"Dhaka (UTC+6)",               "BST-6"},
    {"Bangkok (UTC+7)",             "ICT-7"},
    {"Ho Chi Minh (UTC+7)",         "ICT-7"},
    {"Jakarta (UTC+7)",             "WIB-7"},
    {"Singapore (UTC+8)",           "SGT-8"},
    {"Kuala Lumpur (UTC+8)",        "MYT-8"},
    {"Hong Kong (UTC+8)",           "HKT-8"},
    {"Shanghai (UTC+8)",            "CST-8"},
    {"Taipei (UTC+8)",              "CST-8"},
    {"Manila (UTC+8)",              "PHT-8"},
    {"Perth (UTC+8)",               "AWST-8"},
    {"Seoul (UTC+9)",               "KST-9"},
    {"Tokyo (UTC+9)",               "JST-9"},
    {"Adelaide (UTC+9:30)",         "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Sydney (UTC+10)",             "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Melbourne (UTC+10)",          "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland (UTC+12)",           "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Fiji (UTC+12)",               "FJT-12"},
    {"Samoa (UTC-11)",              "SST11"},
};

#define NUM_TIMEZONES (sizeof(timezones) / sizeof(timezones[0]))

// Layout
#define HEADER_HEIGHT 30
#define LIST_ITEM_H   28
#define LIST_START_Y  35
#define LIST_VISIBLE  6
#define SCROLL_ZONE_H 30

// State
static int selected_tz = 0;
static int scroll_offset = 0;
static bool selection_made = false;
static uint32_t last_touch_time = 0;

static void draw_header(void) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_BLUE);
    display_string((DISPLAY_WIDTH - 15 * 8) / 2, 8, "Select Timezone", COLOR_WHITE, COLOR_BLUE);

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

    // List item touch - single tap to select
    if (touch.y >= LIST_START_Y && touch.y < LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H) {
        int item = (touch.y - LIST_START_Y) / LIST_ITEM_H + scroll_offset;
        if (item < NUM_TIMEZONES) {
            selected_tz = item;
            selection_made = true;
            return TZ_SELECT_DONE;
        }
    }

    // Scroll up
    if (touch.y < LIST_START_Y && scroll_offset > 0) {
        scroll_offset--;
        draw_list();
    }

    // Scroll down (bottom area of screen)
    if (touch.y >= LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H) {
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
