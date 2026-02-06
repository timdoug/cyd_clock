#include "ui_settings.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "touch.h"
#include "nvs_config.h"
#include "ui_common.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_settings";

#define ITEM_START_Y    32


static uint8_t brightness = BRIGHTNESS_DEFAULT;
static uint8_t led_brightness = BRIGHTNESS_DEFAULT;
static bool rotation = false;
static bool touched_last = false;

static void draw_header(void) {
    ui_draw_header("Settings", false);
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
    display_fill_rect(260, y + 3, 50, 18, rot_bg);
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
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            touched_last = touched;
            led_set_brightness(0);
            return SETTINGS_RESULT_TIMEZONE;
        }
        y += UI_ITEM_HEIGHT;

        // WiFi
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            touched_last = touched;
            led_set_brightness(0);
            return SETTINGS_RESULT_WIFI;
        }
        y += UI_ITEM_HEIGHT;

        // NTP
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            touched_last = touched;
            led_set_brightness(0);
            return SETTINGS_RESULT_NTP;
        }
        y += UI_ITEM_HEIGHT;

        // Brightness controls
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            // Tap on bar to set value directly
            if (touch.x >= UI_SLIDER_BAR_X && touch.x < UI_SLIDER_BAR_X + UI_SLIDER_BAR_W) {
                brightness = ((touch.x - UI_SLIDER_BAR_X) * BRIGHTNESS_MAX) / UI_SLIDER_BAR_W;
                if (brightness < BRIGHTNESS_MIN) brightness = BRIGHTNESS_MIN;
                display_set_backlight(brightness);
                nvs_config_set_brightness(brightness);
                draw_menu();
            }
            // Minus button
            else if (touch.x >= UI_SLIDER_BTN_X1 && touch.x < UI_SLIDER_BTN_X1 + UI_SLIDER_BTN_W) {
                if (brightness > BRIGHTNESS_MIN) {
                    brightness -= BRIGHTNESS_STEP;
                    display_set_backlight(brightness);
                    nvs_config_set_brightness(brightness);
                    draw_menu();
                }
            }
            // Plus button
            else if (touch.x >= UI_SLIDER_BTN_X2 && touch.x < UI_SLIDER_BTN_X2 + UI_SLIDER_BTN_W) {
                if (brightness < BRIGHTNESS_MAX) {
                    if (brightness <= BRIGHTNESS_MAX - BRIGHTNESS_STEP) {
                        brightness += BRIGHTNESS_STEP;
                    } else {
                        brightness = BRIGHTNESS_MAX;
                    }
                    display_set_backlight(brightness);
                    nvs_config_set_brightness(brightness);
                    draw_menu();
                }
            }
        }
        y += UI_ITEM_HEIGHT;

        // LED brightness controls
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            // Tap on bar to set value directly (can go to 0)
            if (touch.x >= UI_SLIDER_BAR_X && touch.x < UI_SLIDER_BAR_X + UI_SLIDER_BAR_W) {
                led_brightness = ((touch.x - UI_SLIDER_BAR_X) * BRIGHTNESS_MAX) / UI_SLIDER_BAR_W;
                led_set_brightness(led_brightness);
                nvs_config_set_led_brightness(led_brightness);
                draw_menu();
            }
            // Minus button (can go to 0)
            else if (touch.x >= UI_SLIDER_BTN_X1 && touch.x < UI_SLIDER_BTN_X1 + UI_SLIDER_BTN_W) {
                if (led_brightness >= BRIGHTNESS_STEP) {
                    led_brightness -= BRIGHTNESS_STEP;
                } else {
                    led_brightness = 0;
                }
                led_set_brightness(led_brightness);
                nvs_config_set_led_brightness(led_brightness);
                draw_menu();
            }
            // Plus button
            else if (touch.x >= UI_SLIDER_BTN_X2 && touch.x < UI_SLIDER_BTN_X2 + UI_SLIDER_BTN_W) {
                if (led_brightness <= BRIGHTNESS_MAX - BRIGHTNESS_STEP) {
                    led_brightness += BRIGHTNESS_STEP;
                } else {
                    led_brightness = BRIGHTNESS_MAX;
                }
                led_set_brightness(led_brightness);
                nvs_config_set_led_brightness(led_brightness);
                draw_menu();
            }
        }
        y += UI_ITEM_HEIGHT;

        // Rotation toggle (only on the button, x=260 to 310)
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT && touch.x >= 260 && touch.x < 310) {
            rotation = !rotation;
            display_set_rotation(rotation);
            nvs_config_set_rotation(rotation);
            // Redraw everything after rotation change
            display_fill(COLOR_BLACK);
            draw_header();
            draw_menu();
        }
        y += UI_ITEM_HEIGHT;

        // About
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT) {
            touched_last = touched;
            led_set_brightness(0);
            return SETTINGS_RESULT_ABOUT;
        }
        y += UI_ITEM_HEIGHT;

        // Done button (1/3 width, centered)
        if (touch.y >= y && touch.y < y + UI_ITEM_HEIGHT &&
            touch.x >= 106 && touch.x < 214) {
            touched_last = touched;
            led_set_brightness(0);
            return SETTINGS_RESULT_DONE;
        }
    }

    touched_last = touched;
    return SETTINGS_RESULT_NONE;
}
