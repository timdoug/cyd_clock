#include "ui_settings.h"
#include "display.h"
#include "touch.h"
#include "nvs_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_settings";

#define HEADER_HEIGHT   30
#define ITEM_HEIGHT     28
#define ITEM_START_Y    32

#define COLOR_HEADER    COLOR_BLUE
#define COLOR_ITEM_BG   COLOR_DARKGRAY
#define COLOR_ITEM_FG   COLOR_WHITE
#define COLOR_SELECTED  COLOR_CYAN

static uint8_t brightness = 128;
static bool rotation = false;
static bool touched_last = false;

static void draw_header(void) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_HEADER);
    display_string((DISPLAY_WIDTH - 8 * 8) / 2, 8, "Settings", COLOR_WHITE, COLOR_HEADER);
}

static void draw_menu(void) {
    int y = ITEM_START_Y;

    // Timezone button
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "Timezone", COLOR_ITEM_FG, COLOR_ITEM_BG);
    display_string(DISPLAY_WIDTH - 30, y + 6, ">", COLOR_ITEM_FG, COLOR_ITEM_BG);
    y += ITEM_HEIGHT;

    // WiFi button
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "WiFi", COLOR_ITEM_FG, COLOR_ITEM_BG);
    display_string(DISPLAY_WIDTH - 30, y + 6, ">", COLOR_ITEM_FG, COLOR_ITEM_BG);
    y += ITEM_HEIGHT;

    // NTP button
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "NTP", COLOR_ITEM_FG, COLOR_ITEM_BG);
    display_string(DISPLAY_WIDTH - 30, y + 6, ">", COLOR_ITEM_FG, COLOR_ITEM_BG);
    y += ITEM_HEIGHT;

    // Brightness control
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "Brightness", COLOR_ITEM_FG, COLOR_ITEM_BG);

    // Brightness bar
    int bar_x = 120;
    int bar_w = 100;
    int bar_h = 14;
    int bar_y = y + 6;
    display_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BLACK);
    display_rect(bar_x, bar_y, bar_w, bar_h, COLOR_GRAY);
    int fill_w = (brightness * (bar_w - 4)) / 255;
    display_fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_SELECTED);

    // - and + buttons (aligned to right edge, ending at x=300)
    display_fill_rect(250, y + 2, 22, 22, COLOR_GRAY);
    display_string(256, y + 5, "-", COLOR_WHITE, COLOR_GRAY);
    display_fill_rect(278, y + 2, 22, 22, COLOR_GRAY);
    display_string(284, y + 5, "+", COLOR_WHITE, COLOR_GRAY);
    y += ITEM_HEIGHT;

    // Rotation toggle (aligned to right edge, ending at x=300, vertically centered)
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "Rotate 180", COLOR_ITEM_FG, COLOR_ITEM_BG);
    uint16_t toggle_bg = rotation ? COLOR_GREEN : COLOR_GRAY;
    display_fill_rect(250, y + 4, 50, 18, toggle_bg);
    display_string(262, y + 5, rotation ? "On" : "Off", rotation ? COLOR_BLACK : COLOR_WHITE, toggle_bg);
    y += ITEM_HEIGHT;

    // About button
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_ITEM_BG);
    display_string(20, y + 6, "About", COLOR_ITEM_FG, COLOR_ITEM_BG);
    display_string(DISPLAY_WIDTH - 30, y + 6, ">", COLOR_ITEM_FG, COLOR_ITEM_BG);
    y += ITEM_HEIGHT;

    // Done button
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, ITEM_HEIGHT - 3, COLOR_GREEN);
    display_string((DISPLAY_WIDTH - 4 * 8) / 2, y + 6, "Done", COLOR_BLACK, COLOR_GREEN);
}

void ui_settings_init(void) {
    ESP_LOGI(TAG, "Initializing settings UI");

    // Load saved brightness or default (minimum 32)
    if (!nvs_config_get_brightness(&brightness) || brightness < 32) {
        brightness = 128;
    }

    // Load saved rotation
    rotation = display_is_rotated();

    display_fill(COLOR_BLACK);
    draw_header();
    draw_menu();
}

settings_result_t ui_settings_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Only respond on touch down
    if (touched && !touched_last) {
        int y = ITEM_START_Y;

        // Timezone
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            touched_last = touched;
            return SETTINGS_RESULT_TIMEZONE;
        }
        y += ITEM_HEIGHT;

        // WiFi
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            touched_last = touched;
            return SETTINGS_RESULT_WIFI;
        }
        y += ITEM_HEIGHT;

        // NTP
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            touched_last = touched;
            return SETTINGS_RESULT_NTP;
        }
        y += ITEM_HEIGHT;

        // Brightness controls
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            // Minus button (minimum 32 so screen stays visible)
            if (touch.x >= 250 && touch.x < 272) {
                if (brightness > 32) {
                    brightness -= 16;
                    display_set_backlight(brightness);
                    nvs_config_set_brightness(brightness);
                    draw_menu();
                }
            }
            // Plus button
            else if (touch.x >= 278 && touch.x < 300) {
                if (brightness < 255) {
                    if (brightness <= 239) {
                        brightness += 16;
                    } else {
                        brightness = 255;
                    }
                    display_set_backlight(brightness);
                    nvs_config_set_brightness(brightness);
                    draw_menu();
                }
            }
        }
        y += ITEM_HEIGHT;

        // Rotation toggle (only on the button, x=250 to 300)
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT && touch.x >= 250 && touch.x < 300) {
            rotation = !rotation;
            display_set_rotation(rotation);
            nvs_config_set_rotation(rotation);
            // Redraw everything after rotation change
            display_fill(COLOR_BLACK);
            draw_header();
            draw_menu();
        }
        y += ITEM_HEIGHT;

        // About
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            touched_last = touched;
            return SETTINGS_RESULT_ABOUT;
        }
        y += ITEM_HEIGHT;

        // Done button
        if (touch.y >= y && touch.y < y + ITEM_HEIGHT) {
            touched_last = touched;
            return SETTINGS_RESULT_DONE;
        }
    }

    touched_last = touched;
    return SETTINGS_RESULT_NONE;
}
