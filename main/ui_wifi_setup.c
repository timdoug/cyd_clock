#include "ui_wifi_setup.h"
#include "ui_common.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_wifi_setup";

// UI States
typedef enum {
    STATE_SCANNING,
    STATE_NETWORK_LIST,
    STATE_PASSWORD_ENTRY,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_FAILED,
} setup_state_t;

// Layout constants
#define KEYBOARD_Y      120
#define KEY_WIDTH       28
#define KEY_HEIGHT      22
#define KEY_SPACING     2

// Colors
#define COLOR_KEYBOARD  COLOR_DARKGRAY
#define COLOR_KEY_FG    COLOR_WHITE
#define COLOR_INPUT     COLOR_GREEN

// Keyboard layouts
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

// State variables
static setup_state_t state = STATE_SCANNING;
static wifi_network_t networks[MAX_SCAN_RESULTS];
static int network_count = 0;
static int selected_network = -1;
static int list_scroll = 0;
static char password[64] = {0};
static int password_len = 0;
static int keyboard_mode = 0;  // 0=lower, 1=upper, 2=symbols
static bool shift_active = false;
static char connected_ssid[33] = {0};
static char connected_password[64] = {0};
static uint32_t last_touch_time = 0;
static bool show_back_button = false;


static void draw_network_list(void) {
    display_fill_rect(0, UI_LIST_START_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - UI_LIST_START_Y, COLOR_BLACK);

    for (int i = 0; i < UI_LIST_VISIBLE && (i + list_scroll) < network_count; i++) {
        int idx = i + list_scroll;
        int y = UI_LIST_START_Y + i * UI_LIST_ITEM_H;

        uint16_t bg = (idx == selected_network) ? UI_COLOR_SELECTED : COLOR_BLACK;
        uint16_t fg = (idx == selected_network) ? COLOR_BLACK : COLOR_WHITE;

        display_fill_rect(0, y, DISPLAY_WIDTH, UI_LIST_ITEM_H - 2, bg);

        // Network name
        display_string(5, y + 6, networks[idx].ssid, fg, bg);

        // Signal strength indicator
        int bars = 0;
        if (networks[idx].rssi > -50) bars = 4;
        else if (networks[idx].rssi > -60) bars = 3;
        else if (networks[idx].rssi > -70) bars = 2;
        else bars = 1;

        for (int b = 0; b < bars; b++) {
            int bh = 4 + b * 3;
            display_fill_rect(DISPLAY_WIDTH - 30 + b * 6, y + UI_LIST_ITEM_H - 4 - bh, 4, bh, fg);
        }

        // Lock icon for secured networks
        if (networks[idx].authmode) {
            display_char(DISPLAY_WIDTH - 50, y + 6, '*', fg, bg);
        }
    }

    // Scroll indicators
    if (list_scroll > 0) {
        display_string(DISPLAY_WIDTH / 2 - 4, UI_LIST_START_Y - 8, "^", COLOR_GRAY, COLOR_BLACK);
    }
    if (list_scroll + UI_LIST_VISIBLE < network_count) {
        display_string(DISPLAY_WIDTH / 2 - 4, UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H, "v", COLOR_GRAY, COLOR_BLACK);
    }
}


static void draw_password_input(void) {
    // Clear password area
    display_fill_rect(0, UI_LIST_START_Y, DISPLAY_WIDTH, KEYBOARD_Y - UI_LIST_START_Y, COLOR_BLACK);

    // Show selected network
    display_string(5, UI_LIST_START_Y + 5, "Network:", COLOR_GRAY, COLOR_BLACK);
    display_string(80, UI_LIST_START_Y + 5, networks[selected_network].ssid, COLOR_WHITE, COLOR_BLACK);

    // Password input field
    display_fill_rect(5, UI_LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_DARKGRAY);
    display_rect(5, UI_LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_WHITE);

    // Show password as dots with last char visible
    char display_pwd[65];
    for (int i = 0; i < password_len; i++) {
        if (i == password_len - 1) {
            display_pwd[i] = password[i];  // Show last character briefly
        } else {
            display_pwd[i] = '*';
        }
    }
    display_pwd[password_len] = '\0';
    display_string(10, UI_LIST_START_Y + 35, display_pwd, COLOR_INPUT, COLOR_DARKGRAY);

    // Cursor
    display_char(10 + password_len * 8, UI_LIST_START_Y + 35, '_', COLOR_INPUT, COLOR_DARKGRAY);
}

static void draw_keyboard(void) {
    const char **layout;
    if (keyboard_mode == 2) layout = keyboard_symbols;
    else if (keyboard_mode == 1 || shift_active) layout = keyboard_upper;
    else layout = keyboard_lower;

    // Clear keyboard area first
    display_fill_rect(0, KEYBOARD_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - KEYBOARD_Y, COLOR_BLACK);

    int y = KEYBOARD_Y;

    for (int row = 0; row < 4; row++) {
        const char *keys = layout[row];
        int row_len = strlen(keys);
        int x = (DISPLAY_WIDTH - row_len * (KEY_WIDTH + KEY_SPACING)) / 2;

        for (int col = 0; col < row_len; col++) {
            display_fill_rect(x, y, KEY_WIDTH, KEY_HEIGHT, COLOR_KEYBOARD);
            display_rect(x, y, KEY_WIDTH, KEY_HEIGHT, COLOR_GRAY);
            display_char(x + 10, y + 3, keys[col], COLOR_KEY_FG, COLOR_KEYBOARD);
            x += KEY_WIDTH + KEY_SPACING;
        }
        y += KEY_HEIGHT + KEY_SPACING;
    }

    // Special keys row
    y = KEYBOARD_Y + 4 * (KEY_HEIGHT + KEY_SPACING);
    int x = 5;

    // Shift key
    display_fill_rect(x, y, 40, KEY_HEIGHT, shift_active ? UI_COLOR_SELECTED : COLOR_KEYBOARD);
    display_string(x + 8, y + 3, "Shf", shift_active ? COLOR_BLACK : COLOR_KEY_FG,
                   shift_active ? UI_COLOR_SELECTED : COLOR_KEYBOARD);
    x += 45;

    // Mode key
    display_fill_rect(x, y, 40, KEY_HEIGHT, COLOR_KEYBOARD);
    const char *mode_label = (keyboard_mode == 0) ? "?#@" : "abc";
    display_string(x + 8, y + 3, mode_label, COLOR_KEY_FG, COLOR_KEYBOARD);
    x += 45;

    // Space bar
    display_fill_rect(x, y, 100, KEY_HEIGHT, COLOR_KEYBOARD);
    display_string(x + 30, y + 3, "Space", COLOR_KEY_FG, COLOR_KEYBOARD);
    x += 105;

    // Backspace
    display_fill_rect(x, y, 40, KEY_HEIGHT, COLOR_KEYBOARD);
    display_string(x + 8, y + 3, "Del", COLOR_KEY_FG, COLOR_KEYBOARD);
    x += 45;

    // Connect button
    display_fill_rect(x, y, 60, KEY_HEIGHT, COLOR_GREEN);
    display_string(x + 18, y + 3, "Go", COLOR_BLACK, COLOR_GREEN);
}

static char get_key_at(int tx, int ty) {
    const char **layout;
    if (keyboard_mode == 2) layout = keyboard_symbols;
    else if (keyboard_mode == 1 || shift_active) layout = keyboard_upper;
    else layout = keyboard_lower;

    if (ty < KEYBOARD_Y) return 0;

    int row = (ty - KEYBOARD_Y) / (KEY_HEIGHT + KEY_SPACING);

    // Special keys row
    if (row >= 4) {
        int y = KEYBOARD_Y + 4 * (KEY_HEIGHT + KEY_SPACING);
        if (ty >= y && ty < y + KEY_HEIGHT) {
            if (tx < 45) return VKEY_SHIFT;
            if (tx < 90) return VKEY_MODE;
            if (tx < 195) return ' ';    // Space
            if (tx < 240) return VKEY_BACKSPACE;
            return VKEY_ENTER;
        }
        return 0;
    }

    if (row >= 4) return 0;

    const char *keys = layout[row];
    int row_len = strlen(keys);
    int row_start = (DISPLAY_WIDTH - row_len * (KEY_WIDTH + KEY_SPACING)) / 2;

    if (tx < row_start) return 0;

    int col = (tx - row_start) / (KEY_WIDTH + KEY_SPACING);
    if (col >= row_len) return 0;

    return keys[col];
}

void ui_wifi_setup_init(bool show_back) {
    ESP_LOGI(TAG, "Initializing WiFi setup UI");
    state = STATE_SCANNING;
    network_count = 0;
    selected_network = -1;
    list_scroll = 0;
    password_len = 0;
    password[0] = '\0';
    keyboard_mode = 0;
    shift_active = false;
    show_back_button = show_back;
}

wifi_setup_result_t ui_wifi_setup_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Debounce touch
    if (touched && ui_should_debounce(last_touch_time)) {
        touched = false;
    }
    if (touched) {
        last_touch_time = xTaskGetTickCount();
    }

    switch (state) {
        case STATE_SCANNING:
            display_fill(COLOR_BLACK);
            ui_draw_header("WiFi Setup", show_back_button);
            display_string((DISPLAY_WIDTH - 11 * 8) / 2, 120, "Scanning...", COLOR_WHITE, COLOR_BLACK);

            wifi_init();
            network_count = wifi_scan(networks, MAX_SCAN_RESULTS);

            if (network_count > 0) {
                state = STATE_NETWORK_LIST;
                ui_draw_header("Select Network", show_back_button);
                draw_network_list();
            } else {
                display_fill_rect(0, 100, DISPLAY_WIDTH, 40, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 17 * 8) / 2, 120, "No networks found", COLOR_RED, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 12 * 8) / 2, 150, "Tap to retry", COLOR_GRAY, COLOR_BLACK);

                if (touched) {
                    // Back button
                    if (show_back_button && touch.y < UI_HEADER_HEIGHT && touch.x < 60) {
                        return WIFI_SETUP_CANCELLED;
                    }
                    state = STATE_SCANNING;
                }
            }
            break;

        case STATE_NETWORK_LIST:
            if (touched) {
                // Back button
                if (show_back_button && touch.y < UI_HEADER_HEIGHT && touch.x < 60) {
                    return WIFI_SETUP_CANCELLED;
                }

                // Check for list item touch - single tap to select
                if (touch.y >= UI_LIST_START_Y && touch.y < UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H) {
                    int item = (touch.y - UI_LIST_START_Y) / UI_LIST_ITEM_H + list_scroll;
                    if (item < network_count) {
                        selected_network = item;
                        state = STATE_PASSWORD_ENTRY;
                        password_len = 0;
                        password[0] = '\0';
                        display_fill(COLOR_BLACK);
                        ui_draw_header("Enter Password", true);
                        draw_password_input();
                        draw_keyboard();
                    }
                }

                // Scroll up
                if (touch.y < UI_LIST_START_Y && list_scroll > 0) {
                    list_scroll--;
                    draw_network_list();
                }

                // Scroll down
                if (touch.y > UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H && list_scroll + UI_LIST_VISIBLE < network_count) {
                    list_scroll++;
                    draw_network_list();
                }
            }
            break;

        case STATE_PASSWORD_ENTRY:
            if (touched) {
                // Header Back button - return to network list
                if (touch.y < UI_HEADER_HEIGHT && touch.x < 60) {
                    state = STATE_NETWORK_LIST;
                    selected_network = -1;
                    display_fill(COLOR_BLACK);
                    ui_draw_header("Select Network", show_back_button);
                    draw_network_list();
                    break;
                }

                char key = get_key_at(touch.x, touch.y);

                if (key == VKEY_SHIFT) {  // Shift
                    shift_active = !shift_active;
                    draw_keyboard();
                } else if (key == VKEY_MODE) {  // Mode - toggle between letters and symbols
                    keyboard_mode = (keyboard_mode == 0) ? 2 : 0;
                    shift_active = false;
                    draw_keyboard();
                } else if (key == VKEY_BACKSPACE) {  // Backspace
                    if (password_len > 0) {
                        password_len--;
                        password[password_len] = '\0';
                        draw_password_input();
                    }
                } else if (key == VKEY_ENTER) {  // Connect
                    state = STATE_CONNECTING;
                    display_fill(COLOR_BLACK);
                    ui_draw_header("Connecting", false);
                    display_string((DISPLAY_WIDTH - 13 * 8) / 2, 100, "Connecting to", COLOR_WHITE, COLOR_BLACK);
                    display_string((DISPLAY_WIDTH - strlen(networks[selected_network].ssid) * 8) / 2,
                                   130, networks[selected_network].ssid, COLOR_CYAN, COLOR_BLACK);
                } else if (key >= ' ' && key <= '~' && password_len < 63) {
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
                strncpy(connected_ssid, networks[selected_network].ssid, sizeof(connected_ssid) - 1);
                strncpy(connected_password, password, sizeof(connected_password) - 1);
                state = STATE_CONNECTED;
                display_fill_rect(0, 160, DISPLAY_WIDTH, 30, COLOR_BLACK);
                display_string(120, 160, "Connected!", COLOR_GREEN, COLOR_BLACK);
                vTaskDelay(pdMS_TO_TICKS(1000));
                return WIFI_SETUP_CONNECTED;
            } else {
                state = STATE_FAILED;
                display_fill_rect(0, 160, DISPLAY_WIDTH, 50, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 17 * 8) / 2, 160, "Connection failed", COLOR_RED, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 12 * 8) / 2, 190, "Tap to retry", COLOR_GRAY, COLOR_BLACK);
            }
            break;

        case STATE_CONNECTED:
            return WIFI_SETUP_CONNECTED;

        case STATE_FAILED:
            if (touched) {
                state = STATE_PASSWORD_ENTRY;
                display_fill(COLOR_BLACK);
                ui_draw_header("Enter Password", true);
                draw_password_input();
                draw_keyboard();
            }
            break;
    }

    return WIFI_SETUP_CONTINUE;
}

void ui_wifi_setup_get_credentials(char *ssid, char *pwd) {
    strncpy(ssid, connected_ssid, 32);
    ssid[32] = '\0';
    strncpy(pwd, connected_password, 63);
    pwd[63] = '\0';
}
