#include "ui_ntp.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_ntp";

#define HEADER_HEIGHT 30

// Interval options in seconds
static const uint32_t intervals[] = {600, 3600, 21600, 86400};
static const char *interval_names[] = {"10 min", "1 hour", "6 hour", "24 hour"};
#define NUM_INTERVALS (sizeof(intervals) / sizeof(intervals[0]))

// Keyboard layout
static const char *keyboard_rows[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-_",
};
#define KEY_WIDTH  29
#define KEY_HEIGHT 22
#define KEY_START_Y 120

// UI state
typedef enum {
    NTP_STATE_MAIN,
    NTP_STATE_KEYBOARD,
} ntp_ui_state_t;

static ntp_ui_state_t ui_state = NTP_STATE_MAIN;
static bool touched_last = false;
static char custom_server[64] = {0};
static int custom_server_len = 0;

static int find_interval_idx(uint32_t interval) {
    for (int i = 0; i < NUM_INTERVALS; i++) {
        if (intervals[i] == interval) return i;
    }
    return 2;  // Default to 6 hour
}

static void draw_keyboard(void) {
    display_fill_rect(0, KEY_START_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - KEY_START_Y, COLOR_BLACK);

    for (int row = 0; row < 4; row++) {
        const char *keys = keyboard_rows[row];
        int num_keys = strlen(keys);
        int start_x = (DISPLAY_WIDTH - num_keys * KEY_WIDTH) / 2;

        for (int i = 0; i < num_keys; i++) {
            int x = start_x + i * KEY_WIDTH;
            int y = KEY_START_Y + row * KEY_HEIGHT;

            display_fill_rect(x + 1, y + 1, KEY_WIDTH - 2, KEY_HEIGHT - 2, COLOR_DARKGRAY);
            char str[2] = {keys[i], '\0'};
            display_string(x + 10, y + 3, str, COLOR_WHITE, COLOR_DARKGRAY);
        }
    }

    // Bottom row: Cancel (left), Del (center), Done (right)
    int y = KEY_START_Y + 4 * KEY_HEIGHT;
    int btn_h = KEY_HEIGHT - 2;

    display_fill_rect(10, y, 80, btn_h, COLOR_RED);
    display_string(26, y + 3, "Cancel", COLOR_WHITE, COLOR_RED);

    display_fill_rect(120, y, 80, btn_h, COLOR_GRAY);
    display_string(144, y + 3, "Del", COLOR_WHITE, COLOR_GRAY);

    display_fill_rect(230, y, 80, btn_h, COLOR_GREEN);
    display_string(254, y + 3, "Done", COLOR_BLACK, COLOR_GREEN);
}

static void draw_server_input(void) {
    display_fill_rect(0, 35, DISPLAY_WIDTH, 30, COLOR_BLACK);
    display_string(10, 38, "Server:", COLOR_GRAY, COLOR_BLACK);

    display_fill_rect(10, 55, DISPLAY_WIDTH - 20, 20, COLOR_DARKGRAY);

    if (custom_server_len > 0) {
        const char *display_str = custom_server;
        if (custom_server_len > 35) {
            display_str = custom_server + custom_server_len - 35;
        }
        display_string(15, 59, display_str, COLOR_WHITE, COLOR_DARKGRAY);
    }

    // Show cursor
    int cursor_x = 15 + (custom_server_len > 35 ? 35 : custom_server_len) * 8;
    if (cursor_x < DISPLAY_WIDTH - 20) {
        display_string(cursor_x, 59, "_", COLOR_CYAN, COLOR_DARKGRAY);
    }
}

static char get_key_at(int16_t x, int16_t y) {
    if (y < KEY_START_Y) return 0;

    int row = (y - KEY_START_Y) / KEY_HEIGHT;

    // Bottom row: Cancel (left), Del (center), Done (right)
    if (row >= 4) {
        if (x >= 10 && x < 90) return '\x1B';      // Cancel
        if (x >= 120 && x < 200) return '\x08';    // Del
        if (x >= 230 && x < 310) return '\x0D';    // Done
        return 0;
    }

    const char *keys = keyboard_rows[row];
    int num_keys = strlen(keys);
    int start_x = (DISPLAY_WIDTH - num_keys * KEY_WIDTH) / 2;

    int col = (x - start_x) / KEY_WIDTH;
    if (col >= 0 && col < num_keys) {
        return keys[col];
    }

    return 0;
}

static void draw_main_screen(void) {
    display_fill(COLOR_BLACK);

    // Header
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_BLUE);
    display_string((DISPLAY_WIDTH - 12 * 8) / 2, 8, "NTP Settings", COLOR_WHITE, COLOR_BLUE);

    // Back button
    display_fill_rect(5, 5, 50, 20, COLOR_DARKGRAY);
    display_string(15, 8, "Back", COLOR_WHITE, COLOR_DARKGRAY);

    int y = 40;

    // Server display
    display_string(10, y, "Server:", COLOR_GRAY, COLOR_BLACK);
    y += 20;

    // Show current server in a tappable box
    display_fill_rect(10, y, DISPLAY_WIDTH - 20, 28, COLOR_DARKGRAY);
    char display_server[38];
    strncpy(display_server, wifi_get_custom_ntp_server(), 37);
    display_server[37] = '\0';
    display_string(15, y + 7, display_server, COLOR_WHITE, COLOR_DARKGRAY);
    display_string(DISPLAY_WIDTH - 30, y + 7, ">", COLOR_WHITE, COLOR_DARKGRAY);
    y += 40;

    // Interval section
    display_string(10, y, "Sync Interval:", COLOR_GRAY, COLOR_BLACK);
    y += 22;

    uint32_t current_interval = wifi_get_ntp_interval();
    int current_interval_idx = find_interval_idx(current_interval);

    for (int i = 0; i < NUM_INTERVALS; i++) {
        uint16_t bg = (i == current_interval_idx) ? COLOR_CYAN : COLOR_DARKGRAY;
        uint16_t fg = (i == current_interval_idx) ? COLOR_BLACK : COLOR_WHITE;

        int btn_w = 72;
        int btn_x = 10 + i * (btn_w + 4);

        display_fill_rect(btn_x, y, btn_w, 24, bg);
        int text_x = btn_x + (btn_w - strlen(interval_names[i]) * 8) / 2;
        display_string(text_x, y + 5, interval_names[i], fg, bg);
    }
    y += 36;

    // Sync Now button
    display_fill_rect(10, y, 80, 28, COLOR_GREEN);
    display_string(18, y + 7, "Sync Now", COLOR_BLACK, COLOR_GREEN);
}

static void draw_keyboard_screen(void) {
    display_fill(COLOR_BLACK);

    // Header
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_BLUE);
    display_string((DISPLAY_WIDTH - 10 * 8) / 2, 8, "NTP Server", COLOR_WHITE, COLOR_BLUE);

    draw_server_input();
    draw_keyboard();
}

void ui_ntp_init(void) {
    ESP_LOGI(TAG, "Initializing NTP settings UI");
    touched_last = false;
    ui_state = NTP_STATE_MAIN;

    // Load current custom server
    strncpy(custom_server, wifi_get_custom_ntp_server(), sizeof(custom_server) - 1);
    custom_server[sizeof(custom_server) - 1] = '\0';
    custom_server_len = strlen(custom_server);

    draw_main_screen();
}

ntp_result_t ui_ntp_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    if (touched && !touched_last) {
        if (ui_state == NTP_STATE_MAIN) {
            // Back button
            if (touch.y < HEADER_HEIGHT && touch.x < 60) {
                touched_last = touched;
                return NTP_RESULT_BACK;
            }

            // Server box (tap to edit)
            if (touch.y >= 60 && touch.y < 88) {
                ui_state = NTP_STATE_KEYBOARD;
                draw_keyboard_screen();
            }

            // Interval buttons
            int interval_y = 40 + 20 + 40 + 22;
            if (touch.y >= interval_y && touch.y < interval_y + 24) {
                int btn_w = 72;
                int col = (touch.x - 10) / (btn_w + 4);
                if (col >= 0 && col < NUM_INTERVALS) {
                    wifi_set_ntp_interval(intervals[col]);
                    nvs_config_set_ntp_interval(intervals[col]);
                    draw_main_screen();
                }
            }

            // Sync Now button
            int sync_y = interval_y + 36;
            if (touch.y >= sync_y && touch.y < sync_y + 28 && touch.x >= 10 && touch.x < 90) {
                wifi_force_ntp_sync();
                return NTP_RESULT_SYNCED;
            }
        } else if (ui_state == NTP_STATE_KEYBOARD) {
            char key = get_key_at(touch.x, touch.y);

            if (key == '\x1B') {  // Cancel
                // Restore from saved
                strncpy(custom_server, wifi_get_custom_ntp_server(), sizeof(custom_server) - 1);
                custom_server[sizeof(custom_server) - 1] = '\0';
                custom_server_len = strlen(custom_server);
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (key == '\x0D') {  // Done
                if (custom_server_len > 0) {
                    wifi_set_custom_ntp_server(custom_server);
                    nvs_config_set_custom_ntp_server(custom_server);
                    wifi_force_ntp_sync();
                }
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (key == '\x08') {  // Delete
                if (custom_server_len > 0) {
                    custom_server_len--;
                    custom_server[custom_server_len] = '\0';
                    draw_server_input();
                }
            } else if (key >= ' ' && key <= '~' && custom_server_len < 63) {
                custom_server[custom_server_len++] = key;
                custom_server[custom_server_len] = '\0';
                draw_server_input();
            }
        }
    }

    touched_last = touched;
    return NTP_RESULT_NONE;
}
