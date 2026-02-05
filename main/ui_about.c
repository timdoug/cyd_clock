#include "ui_about.h"
#include "config.h"
#include "display.h"
#include "touch.h"
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

    display_string(20, y, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK);
    y += 20;
    display_string(20, y, "The CYD Clock", COLOR_CYAN, COLOR_BLACK);
    y += 30;

    display_string(20, y, "Version:", COLOR_GRAY, COLOR_BLACK);
    y += 20;
    display_string(30, y, VERSION_STRING, COLOR_WHITE, COLOR_BLACK);
    y += 35;

    display_string(20, y, "Website:", COLOR_GRAY, COLOR_BLACK);
    y += 20;
    display_string(30, y, URL, COLOR_CYAN, COLOR_BLACK);
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
