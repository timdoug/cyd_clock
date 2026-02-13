#include "ui_about.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "config.h"
#include "display.h"
#include "touch.h"
#include "ui_common.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "ui_about";

#define URL "github.com/timdoug/cyd_clock"

static uint32_t last_touch_time = 0;

static void draw_screen(void) {
    ui_draw_header("About", true);

    // Content
    int y = 50;

    ui_draw_centered_string(y, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK, false);
    y += 20;
    ui_draw_centered_string(y, "The CYD Clock", COLOR_CYAN, COLOR_BLACK, false);
    y += 20;
    ui_draw_centered_string(y, URL, COLOR_GRAY, COLOR_BLACK, false);
    y += 35;

    display_string(20, y, "Version:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, VERSION_STRING, COLOR_WHITE, COLOR_BLACK);
    y += 20;

    char ip_str[16];
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    display_string(20, y, "IP:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, ip_str, COLOR_WHITE, COLOR_BLACK);
    y += 20;

    char rssi_str[16];
    int8_t rssi = wifi_get_rssi();
    snprintf(rssi_str, sizeof(rssi_str), "%d dBm", rssi);
    display_string(20, y, "RSSI:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, rssi_str, COLOR_WHITE, COLOR_BLACK);
    y += 20;

    char mac_str[18];
    wifi_get_mac_str(mac_str, sizeof(mac_str));
    display_string(20, y, "MAC:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, mac_str, COLOR_WHITE, COLOR_BLACK);
}

void ui_about_init(void) {
    ESP_LOGI(TAG, "Initializing about screen");
    last_touch_time = 0;

    display_fill(COLOR_BLACK);
    draw_screen();
}

about_result_t ui_about_update(void) {
    touch_point_t touch;
    bool touched = ui_read_touch(&touch, &last_touch_time);

    if (touched) {
        // Back button
        if (touch.y < UI_HEADER_HEIGHT && touch.x < UI_BACK_BTN_X + UI_BACK_BTN_W) {
            return ABOUT_RESULT_BACK;
        }
    }

    return ABOUT_RESULT_NONE;
}
