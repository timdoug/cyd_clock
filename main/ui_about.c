#include "ui_about.h"
#include "config.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "ui_common.h"
#include "esp_log.h"
#include "version.h"
#include <string.h>

static const char *TAG = "ui_about";

#define URL "github.com/timdoug/cyd_clock"

static bool touched_last = false;

static void draw_screen(void) {
    ui_draw_header("About", true);

    // Content
    int y = 50;

    display_string((DISPLAY_WIDTH - 15 * CHAR_WIDTH) / 2, y, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK);
    y += 20;
    display_string((DISPLAY_WIDTH - 13 * CHAR_WIDTH) / 2, y, "The CYD Clock", COLOR_CYAN, COLOR_BLACK);
    y += 20;
    display_string((DISPLAY_WIDTH - strlen(URL) * CHAR_WIDTH) / 2, y, URL, COLOR_GRAY, COLOR_BLACK);
    y += 35;

    display_string(20, y, "Version:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, VERSION_STRING, COLOR_WHITE, COLOR_BLACK);
    y += 25;

    char ip_str[16];
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    display_string(20, y, "IP:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, ip_str, COLOR_WHITE, COLOR_BLACK);
}

void ui_about_init(void) {
    ESP_LOGI(TAG, "Initializing about screen");
    touched_last = false;

    display_fill(COLOR_BLACK);
    draw_screen();
}

about_result_t ui_about_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Only respond on touch down
    if (touched && !touched_last) {
        // Back button
        if (touch.y < UI_HEADER_HEIGHT && touch.x < 60) {
            touched_last = touched;
            return ABOUT_RESULT_BACK;
        }
    }

    touched_last = touched;
    return ABOUT_RESULT_NONE;
}
