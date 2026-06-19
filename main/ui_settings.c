#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_common.h"

static const char *TAG = "ui_settings";

#define ITEM_START_Y        32
#define ROTATION_TOGGLE_X   260
#define ROTATION_TOGGLE_W   50

#define TZ_ROW_Y            (ITEM_START_Y)
#define WIFI_ROW_Y          (TZ_ROW_Y + UI_ITEM_HEIGHT)
#define NTP_ROW_Y           (WIFI_ROW_Y + UI_ITEM_HEIGHT)
#define BRIGHTNESS_ROW_Y    (NTP_ROW_Y + UI_ITEM_HEIGHT)
#define LED_ROW_Y           (BRIGHTNESS_ROW_Y + UI_ITEM_HEIGHT)
#define ROTATION_ROW_Y      (LED_ROW_Y + UI_ITEM_HEIGHT)
#define ABOUT_ROW_Y         (ROTATION_ROW_Y + UI_ITEM_HEIGHT)


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
    ui_draw_menu_item(TZ_ROW_Y, "Time zone");
    ui_draw_menu_item(WIFI_ROW_Y, "WiFi");
    ui_draw_menu_item(NTP_ROW_Y, "NTP");
    ui_draw_slider(BRIGHTNESS_ROW_Y, "Brightness", brightness, BRIGHTNESS_MAX, UI_COLOR_SELECTED);
    ui_draw_slider(LED_ROW_Y, "LED Blink", led_brightness, BRIGHTNESS_MAX, COLOR_RED);

    display_fill_rect(0, ROTATION_ROW_Y, DISPLAY_WIDTH, UI_ITEM_HEIGHT - 3, UI_COLOR_ITEM_BG);
    display_string(10, ROTATION_ROW_Y + UI_TEXT_Y_OFFSET, "Rotate 180\x7F", UI_COLOR_ITEM_FG, UI_COLOR_ITEM_BG);
    uint16_t rot_bg = rotation ? COLOR_GREEN : COLOR_GRAY;
    display_fill_rect(ROTATION_TOGGLE_X, ROTATION_ROW_Y + 3, ROTATION_TOGGLE_W, 18, rot_bg);
    const char *rot_label = rotation ? "On" : "Off";
    int rot_text_x = ROTATION_TOGGLE_X + (ROTATION_TOGGLE_W - strlen(rot_label) * FONT_CHAR_WIDTH) / 2;
    display_string(rot_text_x, ROTATION_ROW_Y + 4, rot_label, rotation ? COLOR_BLACK : COLOR_WHITE, rot_bg);

    ui_draw_menu_item(ABOUT_ROW_Y, "About");
}

static void draw_header(void) {
    ui_draw_header("Settings", false);
    display_fill_rect(UI_BACK_BTN_X, 5, UI_BACK_BTN_W, 20, UI_COLOR_ITEM_BG);
    display_string(UI_BACK_BTN_X + 10, UI_HEADER_TEXT_Y, "Done", COLOR_WHITE, UI_COLOR_ITEM_BG);
}

void ui_settings_init(void) {
    ESP_LOGI(TAG, "Initializing settings UI");

    if (!nvs_config_get_brightness(&brightness) || brightness < BRIGHTNESS_MIN) {
        brightness = BRIGHTNESS_DEFAULT;
    }

    rotation = display_is_rotated();

    if (!nvs_config_get_led_brightness(&led_brightness)) {
        led_brightness = BRIGHTNESS_DEFAULT;
    }

    display_fill(COLOR_BLACK);
    draw_header();
    draw_menu();
}

settings_result_t ui_settings_update(void) {
    touch_point_t touch;
    bool touched = ui_read_touch(&touch, &last_touch_time);

    if (!touched) {
        return SETTINGS_RESULT_NONE;
    }

    if (ui_back_button_hit(&touch)) {
        return SETTINGS_RESULT_DONE;
    }

    if (touch.y >= TZ_ROW_Y && touch.y < TZ_ROW_Y + UI_ITEM_HEIGHT) {
        return SETTINGS_RESULT_TIMEZONE;
    }

    if (touch.y >= WIFI_ROW_Y && touch.y < WIFI_ROW_Y + UI_ITEM_HEIGHT) {
        return SETTINGS_RESULT_WIFI;
    }

    if (touch.y >= NTP_ROW_Y && touch.y < NTP_ROW_Y + UI_ITEM_HEIGHT) {
        return SETTINGS_RESULT_NTP;
    }

    if (touch.y >= BRIGHTNESS_ROW_Y && touch.y < BRIGHTNESS_ROW_Y + UI_ITEM_HEIGHT) {
        uint8_t old_brightness = brightness;
        if (handle_slider_touch(touch.x, &brightness, BRIGHTNESS_MIN)) {
            display_set_backlight(brightness);
            nvs_config_set_brightness(brightness);
            ui_draw_slider_value_delta(BRIGHTNESS_ROW_Y, old_brightness, brightness,
                                       BRIGHTNESS_MAX, UI_COLOR_SELECTED);
        }
    }

    if (touch.y >= LED_ROW_Y && touch.y < LED_ROW_Y + UI_ITEM_HEIGHT) {
        uint8_t old_led_brightness = led_brightness;
        if (handle_slider_touch(touch.x, &led_brightness, 0)) {
            led_set_brightness(led_brightness);
            nvs_config_set_led_brightness(led_brightness);
            ui_draw_slider_value_delta(LED_ROW_Y, old_led_brightness, led_brightness,
                                       BRIGHTNESS_MAX, COLOR_RED);
        }
    }

    if (touch.y >= ROTATION_ROW_Y && touch.y < ROTATION_ROW_Y + UI_ITEM_HEIGHT &&
        touch.x >= ROTATION_TOGGLE_X && touch.x < ROTATION_TOGGLE_X + ROTATION_TOGGLE_W) {
        rotation = !rotation;
        display_set_rotation(rotation);
        nvs_config_set_rotation(rotation);
        display_fill(COLOR_BLACK);
        draw_header();
        draw_menu();
    }

    if (touch.y >= ABOUT_ROW_Y && touch.y < ABOUT_ROW_Y + UI_ITEM_HEIGHT) {
        return SETTINGS_RESULT_ABOUT;
    }

    return SETTINGS_RESULT_NONE;
}
