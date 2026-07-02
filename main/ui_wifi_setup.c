#include "ui_wifi_setup.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "nvs_config.h"
#include "touch.h"
#include "i18n.h"
#include "ui_common.h"
#include "ui_keyboard.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_wifi_setup";

typedef enum {
    STATE_SCANNING,
    STATE_SCAN_EMPTY,
    STATE_NETWORK_LIST,
    STATE_PASSWORD_ENTRY,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_FAILED,
} setup_state_t;

static const char *keyboard_lower[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};

static const char *keyboard_upper[] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
};

static const char *keyboard_symbols[] = {
    "!@#$%^&*()",
    "-_=+[]{}\\|",
    ";:'\"<>,.",
    "`~?/",
};

static setup_state_t state = STATE_SCANNING;
static wifi_network_t networks[MAX_SCAN_RESULTS];
static int network_count = 0;
static int selected_network = -1;
static int highlighted_network = -1;
static int list_scroll = 0;
static char password[MAX_PASSWORD_LEN] = {0};
static int password_len = 0;
static int keyboard_mode = 0;
static bool shift_active = false;
static char connected_ssid[WIFI_SSID_BUF_LEN] = {0};
static char connected_password[MAX_PASSWORD_LEN] = {0};
static char saved_ssid[WIFI_SSID_BUF_LEN] = {0};
static bool saved_ssid_known = false;
static uint32_t last_touch_time = 0;
static bool show_back_button = false;
static bool scan_started = false;
static ui_list_touch_t list_touch;


static void reset_list_touch(void) {
    ui_list_touch_reset(&list_touch);
}

static void refresh_highlighted_network(void) {
    highlighted_network = -1;
    if (!saved_ssid_known) return;

    for (int i = 0; i < network_count; i++) {
        if (strcmp(networks[i].ssid, saved_ssid) == 0) {
            highlighted_network = i;
            return;
        }
    }
}

static void scroll_to_highlighted_network(void) {
    if (highlighted_network < 0) return;
    list_scroll = ui_list_scroll_to_item(highlighted_network, network_count);
}

static void draw_network_list(void) {
    const char *labels[MAX_SCAN_RESULTS];
    for (int i = 0; i < network_count; i++) {
        labels[i] = networks[i].ssid;
    }
    ui_draw_list(labels, network_count, list_scroll, highlighted_network);

    for (int i = 0; i < UI_LIST_VISIBLE && (i + list_scroll) < network_count; i++) {
        int idx = i + list_scroll;
        int y = UI_LIST_START_Y + i * UI_LIST_ITEM_H;

        uint16_t bg = (idx == highlighted_network) ? UI_COLOR_SELECTED : COLOR_BLACK;
        uint16_t fg = (idx == highlighted_network) ? COLOR_BLACK : COLOR_WHITE;

        int bars = 0;
        if (networks[idx].rssi > -50) bars = 4;
        else if (networks[idx].rssi > -60) bars = 3;
        else if (networks[idx].rssi > -70) bars = 2;
        else bars = 1;

        // Keep the bars clear of the scroll-chevron gutter (ui_draw_list draws
        // the down chevron around x = DISPLAY_WIDTH - 9, and the bars are
        // painted afterwards): the rightmost bar ends at DISPLAY_WIDTH - 16,
        // leaving the gutter untouched on the last visible row.
        for (int b = 0; b < bars; b++) {
            int bh = 4 + b * 3;
            display_fill_rect(DISPLAY_WIDTH - 38 + b * 6, y + UI_LIST_ITEM_H - 4 - bh, 4, bh, fg);
        }

        if (networks[idx].authmode) {
            display_char(DISPLAY_WIDTH - 50, y + 6, '*', fg, bg);
        }
    }
}


static void draw_password_input(void) {
    display_fill_rect(0, UI_LIST_START_Y, DISPLAY_WIDTH, KEYBOARD_Y - UI_LIST_START_Y, COLOR_BLACK);

    display_string(5, UI_LIST_START_Y + 5, tr(STR_NETWORK_LABEL), COLOR_GRAY, COLOR_BLACK);
    display_string(80, UI_LIST_START_Y + 5, networks[selected_network].ssid, COLOR_WHITE, COLOR_BLACK);

    display_fill_rect(5, UI_LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_DARKGRAY);
    display_rect(5, UI_LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_WHITE);

    // Show password as dots with last char visible. Long passwords scroll:
    // only the tail that fits the field (with one cell left for the cursor)
    // is drawn.
    const int max_visible = (DISPLAY_WIDTH - 20) / FONT_CHAR_WIDTH - 1;
    int start = password_len > max_visible ? password_len - max_visible : 0;
    int shown = password_len - start;
    char display_pwd[MAX_PASSWORD_LEN];
    for (int i = 0; i < shown; i++) {
        if (start + i == password_len - 1) {
            display_pwd[i] = password[start + i];
        } else {
            display_pwd[i] = '*';
        }
    }
    display_pwd[shown] = '\0';
    display_string(10, UI_LIST_START_Y + 35, display_pwd, COLOR_GREEN, COLOR_DARKGRAY);

    display_char(10 + shown * FONT_CHAR_WIDTH, UI_LIST_START_Y + 35, '_', COLOR_GREEN, COLOR_DARKGRAY);
}

// Bottom keyboard row: draw geometry and hit testing share this table so
// the two can never drift apart.
static const struct { int x; int w; char key; } kb_bottom_row[] = {
    { 5,   40,  VKEY_SHIFT     },
    { 50,  40,  VKEY_MODE      },
    { 95,  100, ' '            },
    { 200, 40,  VKEY_BACKSPACE },
    { 245, 60,  VKEY_ENTER     },
};
#define KB_BOTTOM_KEYS ((int)(sizeof(kb_bottom_row) / sizeof(kb_bottom_row[0])))

static void draw_keyboard(void) {
    const char **layout;
    if (keyboard_mode == 2) layout = keyboard_symbols;
    else if (keyboard_mode == 1 || shift_active) layout = keyboard_upper;
    else layout = keyboard_lower;

    display_fill_rect(0, KEYBOARD_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - KEYBOARD_Y, COLOR_BLACK);

    ui_keyboard_draw_keys(layout, 4, KEYBOARD_Y, COLOR_DARKGRAY, COLOR_WHITE, COLOR_GRAY);

    int y = ui_keyboard_bottom_y(4, KEYBOARD_Y);
    for (int i = 0; i < KB_BOTTOM_KEYS; i++) {
        const char *label;
        uint16_t fg = COLOR_WHITE, bg = COLOR_DARKGRAY;
        switch (kb_bottom_row[i].key) {
        case VKEY_SHIFT:
            label = tr(STR_KB_SHIFT);
            if (shift_active) { fg = COLOR_BLACK; bg = UI_COLOR_SELECTED; }
            break;
        case VKEY_MODE:
            label = (keyboard_mode == 0) ? "?#@" : "abc";
            break;
        case ' ':
            label = tr(STR_KB_SPACE);
            break;
        case VKEY_BACKSPACE:
            label = tr(STR_DEL);
            break;
        default:
            label = tr(STR_KB_GO);
            fg = COLOR_BLACK;
            bg = COLOR_GREEN;
            break;
        }
        ui_draw_button(kb_bottom_row[i].x, y, kb_bottom_row[i].w,
                       KB_KEY_HEIGHT, label, fg, bg);
    }
}

static char get_key_at(int tx, int ty) {
    const char **layout;
    if (keyboard_mode == 2) layout = keyboard_symbols;
    else if (keyboard_mode == 1 || shift_active) layout = keyboard_upper;
    else layout = keyboard_lower;

    char key = ui_keyboard_get_key(layout, 4, KEYBOARD_Y, tx, ty);
    if (key) return key;

    int y = ui_keyboard_bottom_y(4, KEYBOARD_Y);
    if (ty >= y && ty < y + KB_KEY_HEIGHT) {
        for (int i = 0; i < KB_BOTTOM_KEYS; i++) {
            if (tx >= kb_bottom_row[i].x &&
                tx < kb_bottom_row[i].x + kb_bottom_row[i].w) {
                return kb_bottom_row[i].key;
            }
        }
    }

    return 0;
}

static void enter_connecting_state(void) {
    state = STATE_CONNECTING;
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_CONNECTING), false);
    ui_draw_centered_string(100, tr(STR_CONNECTING_TO), COLOR_WHITE, COLOR_BLACK, false);
    ui_draw_centered_string(130, networks[selected_network].ssid, COLOR_CYAN, COLOR_BLACK, false);
}

static void enter_password_state(void) {
    state = STATE_PASSWORD_ENTRY;
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_ENTER_PASSWORD), true);
    draw_password_input();
    draw_keyboard();
}

void ui_wifi_setup_init(bool show_back) {
    ESP_LOGI(TAG, "Initializing WiFi setup UI");
    state = STATE_SCANNING;
    network_count = 0;
    selected_network = -1;
    highlighted_network = -1;
    list_scroll = 0;
    password_len = 0;
    password[0] = '\0';
    keyboard_mode = 0;
    shift_active = false;
    show_back_button = show_back;
    scan_started = false;
    reset_list_touch();

    char saved_password[MAX_PASSWORD_LEN];
    saved_ssid_known = nvs_config_get_wifi(saved_ssid, saved_password);
    if (!saved_ssid_known) {
        saved_ssid[0] = '\0';
    }
}

wifi_setup_result_t ui_wifi_setup_update(void) {
    touch_point_t touch;
    bool pressed;
    bool touched = ui_read_touch_ex(&touch, &last_touch_time, &pressed);

    switch (state) {
        case STATE_SCANNING:
            if (!scan_started) {
                display_fill(COLOR_BLACK);
                ui_draw_header(tr(STR_WIFI_SETUP), show_back_button);
                ui_draw_centered_string(120, tr(STR_SCANNING), COLOR_WHITE, COLOR_BLACK, false);

                scan_started = true;
                wifi_init();
                if (!wifi_scan_start_async()) {
                    scan_started = false;
                    state = STATE_SCAN_EMPTY;
                    display_fill_rect(0, 100, DISPLAY_WIDTH, 60, COLOR_BLACK);
                    ui_draw_centered_string(120, tr(STR_SCAN_FAILED), COLOR_RED, COLOR_BLACK, false);
                    ui_draw_centered_string(150, tr(STR_TAP_RETRY), COLOR_GRAY, COLOR_BLACK, false);
                    break;
                }
            }

            if (touched && show_back_button && ui_back_button_hit(&touch)) {
                wifi_scan_cancel();
                scan_started = false;
                return WIFI_SETUP_CANCELLED;
            }

            if (!wifi_scan_poll(networks, MAX_SCAN_RESULTS, &network_count)) {
                break;
            }

            scan_started = false;
            if (network_count > 0) {
                state = STATE_NETWORK_LIST;
                refresh_highlighted_network();
                scroll_to_highlighted_network();
                reset_list_touch();
                ui_draw_header(tr(STR_SELECT_NETWORK), show_back_button);
                draw_network_list();
            } else {
                state = STATE_SCAN_EMPTY;
                display_fill_rect(0, 100, DISPLAY_WIDTH, 40, COLOR_BLACK);
                ui_draw_centered_string(120, tr(STR_NO_NETWORKS), COLOR_RED, COLOR_BLACK, false);
                ui_draw_centered_string(150, tr(STR_TAP_RETRY), COLOR_GRAY, COLOR_BLACK, false);
            }
            break;

        case STATE_SCAN_EMPTY:
            if (touched) {
                if (show_back_button && ui_back_button_hit(&touch)) {
                    return WIFI_SETUP_CANCELLED;
                }
                state = STATE_SCANNING;
            }
            break;

        case STATE_NETWORK_LIST:
            ui_list_touch_result_t touch_result =
                ui_list_touch_update(&list_touch, &touch, pressed, network_count, &list_scroll);
            if (touch_result == UI_LIST_TOUCH_SCROLLED) {
                draw_network_list();
                break;
            }

            if (touch_result != UI_LIST_TOUCH_TAPPED) {
                break;
            }

            const touch_point_t *tap_start = &list_touch.tap_start;
            if (show_back_button && ui_back_button_hit(tap_start)) {
                return WIFI_SETUP_CANCELLED;
            }

            int item = ui_list_tap_to_item(tap_start, list_scroll, network_count);
            if (item >= 0) {
                selected_network = item;
                password_len = 0;
                password[0] = '\0';
                reset_list_touch();
                if (networks[selected_network].authmode == 0) {
                    enter_connecting_state();
                } else {
                    enter_password_state();
                }
            }
            break;

        case STATE_PASSWORD_ENTRY:
            if (touched) {
                if (ui_back_button_hit(&touch)) {
                    // Wait for the finger to lift before re-arming the list;
                    // otherwise the held Back press is re-registered as a fresh
                    // list tap and cancels the whole WiFi setup.
                    ui_wait_for_touch_release();
                    state = STATE_NETWORK_LIST;
                    selected_network = -1;
                    reset_list_touch();
                    display_fill(COLOR_BLACK);
                    ui_draw_header(tr(STR_SELECT_NETWORK), show_back_button);
                    draw_network_list();
                    break;
                }

                char key = get_key_at(touch.x, touch.y);

                if (key == VKEY_SHIFT) {
                    shift_active = !shift_active;
                    draw_keyboard();
                } else if (key == VKEY_MODE) {
                    keyboard_mode = (keyboard_mode == 0) ? 2 : 0;
                    shift_active = false;
                    draw_keyboard();
                } else if (key == VKEY_BACKSPACE) {
                    if (password_len > 0) {
                        password_len--;
                        password[password_len] = '\0';
                        draw_password_input();
                    }
                } else if (key == VKEY_ENTER) {
                    enter_connecting_state();
                } else if (key >= ' ' && key <= '~' && password_len < MAX_PASSWORD_LEN - 1) {
                    password[password_len++] = key;
                    password[password_len] = '\0';
                    if (shift_active) {
                        shift_active = false;
                        draw_keyboard();
                    }
                    draw_password_input();
                }
            }
            break;

        case STATE_CONNECTING:
            if (wifi_connect(networks[selected_network].ssid, password)) {
                str_copy(connected_ssid, sizeof(connected_ssid), networks[selected_network].ssid);
                str_copy(connected_password, sizeof(connected_password), password);
                state = STATE_CONNECTED;
                display_fill_rect(0, 160, DISPLAY_WIDTH, 30, COLOR_BLACK);
                ui_draw_centered_string(160, tr(STR_CONNECTED), COLOR_GREEN, COLOR_BLACK, false);
                vTaskDelay(pdMS_TO_TICKS(1000));
                return WIFI_SETUP_CONNECTED;
            } else {
                state = STATE_FAILED;
                display_fill_rect(0, 160, DISPLAY_WIDTH, 50, COLOR_BLACK);
                ui_draw_centered_string(160, tr(STR_CONNECTION_FAILED), COLOR_RED, COLOR_BLACK, false);
                ui_draw_centered_string(190, tr(STR_TAP_RETRY), COLOR_GRAY, COLOR_BLACK, false);
            }
            break;

        case STATE_CONNECTED:
            return WIFI_SETUP_CONNECTED;

        case STATE_FAILED:
            if (touched) {
                // Open networks skipped password entry on the way in; retry
                // them directly rather than landing on a password keyboard.
                if (networks[selected_network].authmode == 0) {
                    enter_connecting_state();
                } else {
                    enter_password_state();
                }
            }
            break;
    }

    return WIFI_SETUP_CONTINUE;
}

void ui_wifi_setup_get_credentials(char *ssid, char *pwd) {
    str_copy(ssid, WIFI_SSID_BUF_LEN, connected_ssid);
    str_copy(pwd, MAX_PASSWORD_LEN, connected_password);
}
