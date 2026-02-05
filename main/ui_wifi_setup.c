#include "ui_wifi_setup.h"
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
#define HEADER_HEIGHT   30
#define LIST_ITEM_H     28
#define LIST_START_Y    35
#define LIST_VISIBLE    6
#define KEYBOARD_Y      120
#define KEY_WIDTH       28
#define KEY_HEIGHT      22
#define KEY_SPACING     2

// Colors
#define COLOR_HEADER    COLOR_BLUE
#define COLOR_SELECTED  COLOR_CYAN
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

static void draw_header(const char *title, bool show_back) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_HEADER);
    int x = (DISPLAY_WIDTH - strlen(title) * 8) / 2;
    display_string(x, 8, title, COLOR_WHITE, COLOR_HEADER);

    if (show_back) {
        display_fill_rect(5, 5, 50, 20, COLOR_DARKGRAY);
        display_string(15, 8, "Back", COLOR_WHITE, COLOR_DARKGRAY);
    }
}

static void draw_network_list(void) {
    display_fill_rect(0, LIST_START_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - LIST_START_Y, COLOR_BLACK);

    for (int i = 0; i < LIST_VISIBLE && (i + list_scroll) < network_count; i++) {
        int idx = i + list_scroll;
        int y = LIST_START_Y + i * LIST_ITEM_H;

        uint16_t bg = (idx == selected_network) ? COLOR_SELECTED : COLOR_BLACK;
        uint16_t fg = (idx == selected_network) ? COLOR_BLACK : COLOR_WHITE;

        display_fill_rect(0, y, DISPLAY_WIDTH, LIST_ITEM_H - 2, bg);

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
            display_fill_rect(DISPLAY_WIDTH - 30 + b * 6, y + LIST_ITEM_H - 4 - bh, 4, bh, fg);
        }

        // Lock icon for secured networks
        if (networks[idx].authmode) {
            display_char(DISPLAY_WIDTH - 50, y + 6, '*', fg, bg);
        }
    }

    // Scroll indicators
    if (list_scroll > 0) {
        display_string(DISPLAY_WIDTH / 2 - 4, LIST_START_Y - 8, "^", COLOR_GRAY, COLOR_BLACK);
    }
    if (list_scroll + LIST_VISIBLE < network_count) {
        display_string(DISPLAY_WIDTH / 2 - 4, LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H, "v", COLOR_GRAY, COLOR_BLACK);
    }
}

static void draw_back_button(void) {
    display_fill_rect(5, 5, 50, 20, COLOR_RED);
    display_string(10, 8, "Back", COLOR_WHITE, COLOR_RED);
}

static void draw_password_input(void) {
    // Clear password area
    display_fill_rect(0, LIST_START_Y, DISPLAY_WIDTH, KEYBOARD_Y - LIST_START_Y, COLOR_BLACK);

    // Show selected network
    display_string(5, LIST_START_Y + 5, "Network:", COLOR_GRAY, COLOR_BLACK);
    display_string(80, LIST_START_Y + 5, networks[selected_network].ssid, COLOR_WHITE, COLOR_BLACK);

    // Password input field
    display_fill_rect(5, LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_DARKGRAY);
    display_rect(5, LIST_START_Y + 30, DISPLAY_WIDTH - 10, 24, COLOR_WHITE);

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
    display_string(10, LIST_START_Y + 35, display_pwd, COLOR_INPUT, COLOR_DARKGRAY);

    // Cursor
    display_char(10 + password_len * 8, LIST_START_Y + 35, '_', COLOR_INPUT, COLOR_DARKGRAY);
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
    display_fill_rect(x, y, 40, KEY_HEIGHT, shift_active ? COLOR_SELECTED : COLOR_KEYBOARD);
    display_string(x + 8, y + 3, "Shf", shift_active ? COLOR_BLACK : COLOR_KEY_FG,
                   shift_active ? COLOR_SELECTED : COLOR_KEYBOARD);
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
            if (tx < 45) return '\x01';  // Shift
            if (tx < 90) return '\x02';  // Mode
            if (tx < 195) return ' ';    // Space
            if (tx < 240) return '\x08'; // Backspace
            return '\x0D';               // Connect (Enter)
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

void ui_wifi_setup_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi setup UI");
    state = STATE_SCANNING;
    network_count = 0;
    selected_network = -1;
    list_scroll = 0;
    password_len = 0;
    password[0] = '\0';
    keyboard_mode = 0;
    shift_active = false;
}

wifi_setup_result_t ui_wifi_setup_update(void) {
    touch_point_t touch;
    bool touched = touch_read(&touch);

    // Debounce touch
    uint32_t now = xTaskGetTickCount();
    if (touched && (now - last_touch_time) < pdMS_TO_TICKS(200)) {
        touched = false;
    }
    if (touched) {
        last_touch_time = now;
    }

    switch (state) {
        case STATE_SCANNING:
            display_fill(COLOR_BLACK);
            draw_header("WiFi Setup", true);
            display_string((DISPLAY_WIDTH - 11 * 8) / 2, 120, "Scanning...", COLOR_WHITE, COLOR_BLACK);

            wifi_init();
            network_count = wifi_scan(networks, MAX_SCAN_RESULTS);

            if (network_count > 0) {
                state = STATE_NETWORK_LIST;
                draw_header("Select Network", true);
                draw_network_list();
            } else {
                display_fill_rect(0, 100, DISPLAY_WIDTH, 40, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 17 * 8) / 2, 120, "No networks found", COLOR_RED, COLOR_BLACK);
                display_string((DISPLAY_WIDTH - 12 * 8) / 2, 150, "Tap to retry", COLOR_GRAY, COLOR_BLACK);

                if (touched) {
                    // Back button
                    if (touch.y < HEADER_HEIGHT && touch.x < 60) {
                        return WIFI_SETUP_CANCELLED;
                    }
                    state = STATE_SCANNING;
                }
            }
            break;

        case STATE_NETWORK_LIST:
            if (touched) {
                // Back button
                if (touch.y < HEADER_HEIGHT && touch.x < 60) {
                    return WIFI_SETUP_CANCELLED;
                }

                // Check for list item touch - single tap to select
                if (touch.y >= LIST_START_Y && touch.y < LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H) {
                    int item = (touch.y - LIST_START_Y) / LIST_ITEM_H + list_scroll;
                    if (item < network_count) {
                        selected_network = item;
                        state = STATE_PASSWORD_ENTRY;
                        password_len = 0;
                        password[0] = '\0';
                        display_fill(COLOR_BLACK);
                        draw_header("Enter Password", true);
                        draw_back_button();
                        draw_password_input();
                        draw_keyboard();
                    }
                }

                // Scroll up
                if (touch.y < LIST_START_Y && list_scroll > 0) {
                    list_scroll--;
                    draw_network_list();
                }

                // Scroll down
                if (touch.y > LIST_START_Y + LIST_VISIBLE * LIST_ITEM_H && list_scroll + LIST_VISIBLE < network_count) {
                    list_scroll++;
                    draw_network_list();
                }
            }
            break;

        case STATE_PASSWORD_ENTRY:
            if (touched) {
                // Header Back button - return to settings
                if (touch.y < HEADER_HEIGHT && touch.x < 60) {
                    return WIFI_SETUP_CANCELLED;
                }

                char key = get_key_at(touch.x, touch.y);

                if (key == '\x01') {  // Shift
                    shift_active = !shift_active;
                    draw_keyboard();
                } else if (key == '\x02') {  // Mode - toggle between letters and symbols
                    keyboard_mode = (keyboard_mode == 0) ? 2 : 0;
                    shift_active = false;
                    draw_keyboard();
                } else if (key == '\x08') {  // Backspace
                    if (password_len > 0) {
                        password_len--;
                        password[password_len] = '\0';
                        draw_password_input();
                    }
                } else if (key == '\x0D') {  // Connect
                    state = STATE_CONNECTING;
                    display_fill(COLOR_BLACK);
                    draw_header("Connecting", false);
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
                draw_header("Enter Password", true);
                draw_back_button();
                draw_password_input();
                draw_keyboard();
            }
            break;
    }

    return WIFI_SETUP_CONTINUE;
}

void ui_wifi_setup_get_credentials(char *ssid, char *pwd) {
    strncpy(ssid, connected_ssid, 32);
    strncpy(pwd, connected_password, 63);
}
