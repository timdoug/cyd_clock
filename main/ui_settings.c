#include "ui_settings.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "touch.h"
#include "nvs_config.h"
#include "ui_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_settings";

#define ITEM_START_Y        32
#define ROTATION_TOGGLE_X   260
#define ROTATION_TOGGLE_W   50


static uint8_t brightness = BRIGHTNESS_DEFAULT;
static uint8_t led_brightness = BRIGHTNESS_DEFAULT;
static bool rotation = false;
static uint32_t last_touch_time = 0;

// Handle touch on a slider row. Returns true if value changed.
static bool handle_slider_touch(int touch_x, uint8_t *value, uint8_t min_val) {
    if (touch_x >= UI_SLIDER_BAR_X && touch_x < UI_SLIDER_BAR_X + UI_SLIDER_BAR_W) {
        *value = ((touch_x - UI_SLIDER_BAR_X) * BRIGHTNESS_MAX) / UI_SLIDER_BAR_W;
        if (*value < min_val) *value = min_val;
        return true;
    }
    if (touch_x >= UI_SLIDER_BTN_X1 && touch_x < UI_SLIDER_BTN_X1 + UI_SLIDER_BTN_W) {
        if (*value >= min_val + BRIGHTNESS_STEP) {
            *value -= BRIGHTNESS_STEP;
        } else {
            *value = min_val;
        }
        return true;
    }
    if (touch_x >= UI_SLIDER_BTN_X2 && touch_x < UI_SLIDER_BTN_X2 + UI_SLIDER_BTN_W) {
        if (*value <= BRIGHTNESS_MAX - BRIGHTNESS_STEP) {
            *value += BRIGHTNESS_STEP;
        } else {
            *value = BRIGHTNESS_MAX;
        }
        return true;
    }
    return false;
}

static void draw_menu(void) {
    int y = ITEM_START_Y;

    ui_draw_menu_item(y, "Time zone");
    y += UI_ITEM_HEIGHT;

    ui_draw_menu_item(y, "WiFi");
    y += UI_ITEM_HEIGHT;

    ui_draw_menu_item(y, "NTP");
    y += UI_ITEM_HEIGHT;

    ui_draw_slider(y, "Brightness", brightness, BRIGHTNESS_MAX, UI_COLOR_SELECTED);
    y += UI_ITEM_HEIGHT;

    ui_draw_slider(y, "LED Blink", led_brightness, BRIGHTNESS_MAX, COLOR_RED);
    y += UI_ITEM_HEIGHT;

    // Rotation toggle
    display_fill_rect(0, y, DISPLAY_WIDTH, UI_ITEM_HEIGHT - 3, UI_COLOR_ITEM_BG);
    display_string(10, y + UI_TEXT_Y_OFFSET, "Rotate 180\x7F", UI_COLOR_ITEM_FG, UI_COLOR_ITEM_BG);
    uint16_t rot_bg = rotation ? COLOR_GREEN : COLOR_GRAY;
    display_fill_rect(ROTATION_TOGGLE_X, y + 3, ROTATION_TOGGLE_W, 18, rot_bg);
    display_string(272, y + 4, rotation ? "On" : "Off", rotation ? COLOR_BLACK : COLOR_WHITE, rot_bg);
    y += UI_ITEM_HEIGHT;

    ui_draw_menu_item(y, "About");
    y += UI_ITEM_HEIGHT;

    // Done button (1/3 width, centered)
    int btn_w = DISPLAY_WIDTH / 3;
    int btn_x = (DISPLAY_WIDTH - btn_w) / 2;
    display_fill_rect(btn_x, y, btn_w, UI_ITEM_HEIGHT - 3, COLOR_GREEN);
    display_string(btn_x + (btn_w - 4 * CHAR_WIDTH) / 2, y + UI_TEXT_Y_OFFSET, "Done", COLOR_BLACK, COLOR_GREEN);
}

void ui_settings_init(void) {
    ESP_LOGI(TAG, "Initializing settings UI");

    // Load saved brightness or default
    if (!nvs_config_get_brightness(&brightness) || brightness < BRIGHTNESS_MIN) {
        brightness = BRIGHTNESS_DEFAULT;
    }

    // Load saved rotation
    rotation = display_is_rotated();

    // Load saved LED brightness (default to BRIGHTNESS_DEFAULT if not set)
    if (!nvs_config_get_led_brightness(&led_brightness)) {
        led_brightness = BRIGHTNESS_DEFAULT;
    }

    // Turn off LED when entering settings
    led_set_brightness(0);

    display_fill(COLOR_BLACK);
    ui_draw_header("Settings", false);
    draw_menu();
}

settings_result_t ui_settings_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Debounce
    if (touched && ui_should_debounce(last_touch_time)) {
        touched = false;
    }
    if (touched) {
        last_touch_time = xTaskGetTickCount();
    }

    if (!touched) {
        return SETTINGS_RESULT_NONE;
    }

    int y = ITEM_START_Y;

    // Timezone
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        led_set_brightness(0);
        return SETTINGS_RESULT_TIMEZONE;
    }
    y += UI_ITEM_HEIGHT;

    // WiFi
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        led_set_brightness(0);
        return SETTINGS_RESULT_WIFI;
    }
    y += UI_ITEM_HEIGHT;

    // NTP
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        led_set_brightness(0);
        return SETTINGS_RESULT_NTP;
    }
    y += UI_ITEM_HEIGHT;

    // Brightness controls
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        if (handle_slider_touch(touch.x, &brightness, BRIGHTNESS_MIN)) {
            display_set_backlight(brightness);
            nvs_config_set_brightness(brightness);
            draw_menu();
        }
    }
    y += UI_ITEM_HEIGHT;

    // LED brightness controls
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        if (handle_slider_touch(touch.x, &led_brightness, 0)) {
            led_set_brightness(led_brightness);
            nvs_config_set_led_brightness(led_brightness);
            draw_menu();
        }
    }
    y += UI_ITEM_HEIGHT;

    // Rotation toggle
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT &&
        touch.x >= ROTATION_TOGGLE_X && touch.x < ROTATION_TOGGLE_X + ROTATION_TOGGLE_W) {
        rotation = !rotation;
        display_set_rotation(rotation);
        nvs_config_set_rotation(rotation);
        // Redraw everything after rotation change
        display_fill(COLOR_BLACK);
        ui_draw_header("Settings", false);
        draw_menu();
    }
    y += UI_ITEM_HEIGHT;

    // About
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
        led_set_brightness(0);
        return SETTINGS_RESULT_ABOUT;
    }
    y += UI_ITEM_HEIGHT;

    // Done button (1/3 width, centered)
    int btn_w = DISPLAY_WIDTH / 3;
    int btn_x = (DISPLAY_WIDTH - btn_w) / 2;
    if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT &&
        touch.x >= btn_x && touch.x < btn_x + btn_w) {
        led_set_brightness(0);
        return SETTINGS_RESULT_DONE;
    }

    return SETTINGS_RESULT_NONE;
}
