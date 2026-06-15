#include "ui_about.h"
#include <string.h>
#include <stdio.h>
#include "esp_idf_version.h"
#include "esp_log.h"
#include "config.h"
#include "display.h"
#include "nvs_config.h"
#include "ota_update.h"
#include "touch.h"
#include "ui_common.h"
#include "ui_keyboard.h"
#include "util.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "ui_about";

#define URL "github.com/timdoug/cyd_clock"
#define OTA_BTN_X 260
#define OTA_BTN_Y 5
#define OTA_BTN_W 55
#define OTA_BTN_H 20
#define OTA_URL_BOX_Y 58
#define OTA_URL_BOX_H 60
#define OTA_UPDATE_BTN_Y 134
#define OTA_UPDATE_BTN_H 28
#define OTA_KEYBOARD_Y 84

static uint32_t last_touch_time = 0;
static char ota_url[MAX_OTA_URL_LEN];
static int ota_url_len = 0;

typedef enum {
    ABOUT_STATE_MAIN,
    ABOUT_STATE_OTA,
    ABOUT_STATE_OTA_KEYBOARD,
} about_ui_state_t;

static about_ui_state_t ui_state = ABOUT_STATE_MAIN;
static ota_update_status_t last_status;
static bool last_update_button_running = false;
static bool update_button_drawn = false;
static bool ota_status_frame_drawn = false;
static char ota_status_message_drawn[sizeof(last_status.message)] = {0};
static uint32_t ota_status_read_bucket_drawn = UINT32_MAX;
static bool ota_status_read_label_drawn = false;
static char ota_status_progress_drawn[40] = {0};
static int ota_status_bar_fill_drawn = -1;

static void draw_changed_text(int x,
                              int y,
                              const char *text,
                              char *drawn,
                              size_t drawn_len,
                              uint16_t fg,
                              uint16_t bg) {
    size_t text_len = strlen(text);
    size_t old_len = strlen(drawn);
    size_t max_len = text_len > old_len ? text_len : old_len;
    if (max_len >= drawn_len) max_len = drawn_len - 1;

    for (size_t i = 0; i < max_len; i++) {
        char next = i < text_len ? text[i] : ' ';
        char prev = i < old_len ? drawn[i] : ' ';
        if (next != prev) {
            display_char(x + (int)i * FONT_CHAR_WIDTH, y, next, fg, bg);
        }
    }

    str_copy(drawn, drawn_len, text);
}

static const char *url_keyboard_rows[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-_/",
    ":?&=%+#~",
};

static void draw_ota_header_button(void) {
    display_fill_rect(OTA_BTN_X, OTA_BTN_Y, OTA_BTN_W, OTA_BTN_H, UI_COLOR_ITEM_BG);
    display_string(OTA_BTN_X + 15, OTA_BTN_Y + 3, "OTA", COLOR_WHITE, UI_COLOR_ITEM_BG);
}

static void draw_screen(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("About", true);
    draw_ota_header_button();

    // Content
    int y = 48;

    ui_draw_centered_string(y, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK, false);
    y += 18;
    ui_draw_centered_string(y, "The CYD Clock", COLOR_CYAN, COLOR_BLACK, false);
    y += 18;
    ui_draw_centered_string(y, URL, COLOR_GRAY, COLOR_BLACK, false);
    y += 34;

    display_string(20, y, "Version:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, VERSION_STRING, COLOR_WHITE, COLOR_BLACK);
    y += 18;

    display_string(20, y, "ESP-IDF:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, esp_get_idf_version(), COLOR_WHITE, COLOR_BLACK);
    y += 18;

    char ip_str[16];
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    display_string(20, y, "IPv4:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, ip_str, COLOR_WHITE, COLOR_BLACK);
    y += 18;

    char ip6_str[46];
    wifi_get_ip6_str(ip6_str, sizeof(ip6_str));
    display_string(20, y, "IPv6:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, ip6_str[0] ? ip6_str : "none", COLOR_WHITE, COLOR_BLACK);
    y += 18;

    char rssi_str[16];
    int8_t rssi = wifi_get_rssi();
    snprintf(rssi_str, sizeof(rssi_str), "%d dBm", rssi);
    display_string(20, y, "RSSI:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, rssi_str, COLOR_WHITE, COLOR_BLACK);
    y += 18;

    char mac_str[18];
    wifi_get_mac_str(mac_str, sizeof(mac_str));
    display_string(20, y, "MAC:", COLOR_GRAY, COLOR_BLACK);
    display_string(90, y, mac_str, COLOR_WHITE, COLOR_BLACK);
}

static void draw_url_box(void) {
    display_string(10, 40, "Firmware URL:", COLOR_GRAY, COLOR_BLACK);
    display_fill_rect(10, OTA_URL_BOX_Y, DISPLAY_WIDTH - 20, OTA_URL_BOX_H, COLOR_DARKGRAY);

    const int chars_per_line = 36;
    const int max_lines = 3;
    for (int line_no = 0; line_no < max_lines; line_no++) {
        int offset = line_no * chars_per_line;
        if (offset >= ota_url_len) break;

        char line[chars_per_line + 1];
        int copy_len = ota_url_len - offset;
        if (copy_len > chars_per_line) copy_len = chars_per_line;
        memcpy(line, ota_url + offset, copy_len);
        line[copy_len] = '\0';

        if (line_no == max_lines - 1 && offset + copy_len < ota_url_len && copy_len > 3) {
            memcpy(line + copy_len - 3, "...", 3);
        }
        display_string(15, OTA_URL_BOX_Y + 6 + line_no * FONT_CHAR_HEIGHT,
                       line, COLOR_WHITE, COLOR_DARKGRAY);
    }
    display_string(DISPLAY_WIDTH - 30, OTA_URL_BOX_Y + 6, ">", COLOR_WHITE, COLOR_DARKGRAY);
}

static void draw_update_button(void) {
    bool running = ota_update_is_running();
    if (update_button_drawn && running == last_update_button_running) {
        return;
    }
    last_update_button_running = running;
    update_button_drawn = true;

    uint16_t bg = running ? COLOR_GRAY : COLOR_GREEN;
    uint16_t fg = running ? COLOR_WHITE : COLOR_BLACK;
    const char *label = running ? "Running" : "Update";
    display_fill_rect(10, OTA_UPDATE_BTN_Y, 100, OTA_UPDATE_BTN_H, bg);
    int x = 10 + (100 - (int)strlen(label) * FONT_CHAR_WIDTH) / 2;
    display_string(x, OTA_UPDATE_BTN_Y + 7, label, fg, bg);
}

static void draw_ota_status(bool force) {
    ota_update_status_t status;
    ota_update_get_status(&status);
    uint32_t progress_bucket = UINT32_MAX;
    int bar_fill = 0;

    if (status.total_bytes > 0) {
        if (status.bytes_read >= status.total_bytes) {
            progress_bucket = 100;
        } else {
            progress_bucket = (uint32_t)(((uint64_t)status.bytes_read * 100) /
                                         status.total_bytes);
        }
        bar_fill = (int)(((uint64_t)status.bytes_read * 220) /
                         status.total_bytes);
        if (bar_fill > 220) bar_fill = 220;
    } else if (status.bytes_read > 0) {
        progress_bucket = status.bytes_read / (32 * 1024);
    }

    if (force || !ota_status_frame_drawn) {
        display_fill_rect(0, 174, DISPLAY_WIDTH, 65, COLOR_BLACK);
        display_string(10, 176, "Status:", COLOR_GRAY, COLOR_BLACK);
        ota_status_frame_drawn = true;
        ota_status_message_drawn[0] = '\0';
        ota_status_read_bucket_drawn = UINT32_MAX;
        ota_status_read_label_drawn = false;
        ota_status_progress_drawn[0] = '\0';
        ota_status_bar_fill_drawn = -1;
    }

    if (force || strcmp(status.message, ota_status_message_drawn) != 0) {
        display_fill_rect(75, 176, DISPLAY_WIDTH - 75, FONT_CHAR_HEIGHT, COLOR_BLACK);
        display_string(75, 176, status.message, COLOR_WHITE, COLOR_BLACK);
        str_copy(ota_status_message_drawn, sizeof(ota_status_message_drawn), status.message);
    }

    if (force || progress_bucket != ota_status_read_bucket_drawn) {
        if (status.bytes_read > 0) {
            if (force || !ota_status_read_label_drawn) {
                display_string(10, 198, "Read:", COLOR_GRAY, COLOR_BLACK);
                display_rect(10, 218, 222, 8, COLOR_GRAY);
                ota_status_read_label_drawn = true;
            }
            char progress[sizeof(ota_status_progress_drawn)];
            if (status.total_bytes > 0) {
                snprintf(progress, sizeof(progress), "%lu / %lu KB %lu%%",
                         (unsigned long)(status.bytes_read / 1024),
                         (unsigned long)(status.total_bytes / 1024),
                         (unsigned long)progress_bucket);
            } else {
                snprintf(progress, sizeof(progress), "%lu KB",
                         (unsigned long)(status.bytes_read / 1024));
            }

            if (force || strcmp(progress, ota_status_progress_drawn) != 0) {
                draw_changed_text(75, 198, progress, ota_status_progress_drawn,
                                  sizeof(ota_status_progress_drawn),
                                  COLOR_WHITE, COLOR_BLACK);
            }
            if (force || bar_fill != ota_status_bar_fill_drawn) {
                if (force || bar_fill < ota_status_bar_fill_drawn ||
                    ota_status_bar_fill_drawn < 0) {
                    display_fill_rect(11, 219, 220, 6, COLOR_BLACK);
                    display_fill_rect(11, 219, bar_fill, 6, COLOR_CYAN);
                } else if (bar_fill > ota_status_bar_fill_drawn) {
                    display_fill_rect(11 + ota_status_bar_fill_drawn, 219,
                                      bar_fill - ota_status_bar_fill_drawn,
                                      6, COLOR_CYAN);
                }
                ota_status_bar_fill_drawn = bar_fill;
            }
        } else if (ota_status_read_label_drawn) {
            display_fill_rect(10, 198, DISPLAY_WIDTH - 10, 31, COLOR_BLACK);
            ota_status_read_label_drawn = false;
            ota_status_progress_drawn[0] = '\0';
            ota_status_bar_fill_drawn = -1;
        }
        ota_status_read_bucket_drawn = progress_bucket;
    }

    last_status = status;
}

static void draw_ota_error(const char *err) {
    display_fill_rect(0, 174, DISPLAY_WIDTH, 55, COLOR_BLACK);
    display_string(10, 176, "Status:", COLOR_GRAY, COLOR_BLACK);
    display_string(75, 176, err, COLOR_RED, COLOR_BLACK);
    ota_status_frame_drawn = true;
    str_copy(ota_status_message_drawn, sizeof(ota_status_message_drawn), err);
    ota_status_read_bucket_drawn = UINT32_MAX;
    ota_status_read_label_drawn = false;
    ota_status_progress_drawn[0] = '\0';
    ota_status_bar_fill_drawn = -1;
}

static void reset_ota_status_draw_state(void) {
    ota_status_frame_drawn = false;
    ota_status_message_drawn[0] = '\0';
    ota_status_read_bucket_drawn = UINT32_MAX;
    ota_status_read_label_drawn = false;
    ota_status_progress_drawn[0] = '\0';
    ota_status_bar_fill_drawn = -1;
}

static void draw_ota_screen(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("OTA Update", true);
    update_button_drawn = false;
    reset_ota_status_draw_state();
    draw_url_box();
    draw_update_button();
    draw_ota_status(true);
}

static void draw_keyboard_input(void) {
    display_fill_rect(0, 35, DISPLAY_WIDTH, 45, COLOR_BLACK);
    display_string(10, 38, "Firmware URL:", COLOR_GRAY, COLOR_BLACK);
    display_fill_rect(10, 55, DISPLAY_WIDTH - 20, 22, COLOR_DARKGRAY);

    const int max_chars = 35;
    const char *display_url = ota_url;
    if (ota_url_len > max_chars) {
        display_url = ota_url + ota_url_len - max_chars;
    }
    display_string(15, 59, display_url, COLOR_WHITE, COLOR_DARKGRAY);

    int cursor_x = 15 + (ota_url_len > max_chars ? max_chars : ota_url_len) * FONT_CHAR_WIDTH;
    if (cursor_x < DISPLAY_WIDTH - 20) {
        display_string(cursor_x, 59, "_", COLOR_CYAN, COLOR_DARKGRAY);
    }
}

static void draw_keyboard(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("OTA URL", false);
    draw_keyboard_input();
    ui_keyboard_draw_keys(url_keyboard_rows, 5, OTA_KEYBOARD_Y,
                          COLOR_DARKGRAY, COLOR_WHITE, COLOR_GRAY);

    int y = ui_keyboard_bottom_y(5, OTA_KEYBOARD_Y);
    int btn_h = KB_KEY_HEIGHT - 2;

    display_fill_rect(10, y, 80, btn_h, COLOR_RED);
    display_string(26, y + 3, "Cancel", COLOR_WHITE, COLOR_RED);

    display_fill_rect(120, y, 80, btn_h, COLOR_GRAY);
    display_string(144, y + 3, "Del", COLOR_WHITE, COLOR_GRAY);

    display_fill_rect(230, y, 80, btn_h, COLOR_GREEN);
    display_string(254, y + 3, "Done", COLOR_BLACK, COLOR_GREEN);
}

static char get_keyboard_key(int16_t x, int16_t y) {
    char key = ui_keyboard_get_key(url_keyboard_rows, 5, OTA_KEYBOARD_Y, x, y);
    if (key) return key;

    int btn_y = ui_keyboard_bottom_y(5, OTA_KEYBOARD_Y);
    if (y >= btn_y && y < btn_y + KB_KEY_HEIGHT) {
        if (x >= 10 && x < 90) return VKEY_ESCAPE;
        if (x >= 120 && x < 200) return VKEY_BACKSPACE;
        if (x >= 230 && x < 310) return VKEY_ENTER;
    }
    return 0;
}

static void load_ota_url(void) {
    if (!nvs_config_get_ota_url(ota_url) || ota_url[0] == '\0') {
        str_copy(ota_url, sizeof(ota_url), DEFAULT_OTA_URL);
    }
    ota_url_len = strlen(ota_url);
}

void ui_about_init(void) {
    ESP_LOGI(TAG, "Initializing about screen");
    last_touch_time = 0;
    ui_state = ABOUT_STATE_MAIN;
    load_ota_url();
    memset(&last_status, 0, sizeof(last_status));

    display_fill(COLOR_BLACK);
    draw_screen();
}

about_result_t ui_about_update(void) {
    touch_point_t touch;
    bool touched = ui_read_touch(&touch, &last_touch_time);

    if (touched) {
        if (ui_state == ABOUT_STATE_MAIN) {
            if (ui_back_button_hit(&touch)) {
                return ABOUT_RESULT_BACK;
            }
            if (touch.x >= OTA_BTN_X && touch.x < OTA_BTN_X + OTA_BTN_W &&
                touch.y >= OTA_BTN_Y && touch.y < OTA_BTN_Y + OTA_BTN_H) {
                ui_state = ABOUT_STATE_OTA;
                draw_ota_screen();
            }
        } else if (ui_state == ABOUT_STATE_OTA) {
            if (ui_back_button_hit(&touch)) {
                ui_state = ABOUT_STATE_MAIN;
                draw_screen();
            } else if (touch.y >= OTA_URL_BOX_Y && touch.y < OTA_URL_BOX_Y + OTA_URL_BOX_H) {
                ui_state = ABOUT_STATE_OTA_KEYBOARD;
                draw_keyboard();
            } else if (touch.x >= 10 && touch.x < 110 &&
                       touch.y >= OTA_UPDATE_BTN_Y &&
                       touch.y < OTA_UPDATE_BTN_Y + OTA_UPDATE_BTN_H) {
                char err[64];
                if (!ota_update_start(ota_url, err, sizeof(err))) {
                    ota_update_status_t status = {
                        .state = OTA_UPDATE_FAILED,
                        .bytes_read = 0,
                    };
                    str_copy(status.message, sizeof(status.message), err);
                    last_status = status;
                    draw_ota_error(err);
                }
                update_button_drawn = false;
                draw_update_button();
                draw_ota_status(true);
            }
        } else if (ui_state == ABOUT_STATE_OTA_KEYBOARD) {
            char key = get_keyboard_key(touch.x, touch.y);
            if (key == VKEY_ESCAPE) {
                load_ota_url();
                ui_state = ABOUT_STATE_OTA;
                draw_ota_screen();
            } else if (key == VKEY_ENTER) {
                if (ota_url_len == 0) {
                    str_copy(ota_url, sizeof(ota_url), DEFAULT_OTA_URL);
                    ota_url_len = strlen(ota_url);
                }
                nvs_config_set_ota_url(ota_url);
                ui_state = ABOUT_STATE_OTA;
                draw_ota_screen();
            } else if (key == VKEY_BACKSPACE) {
                if (ota_url_len > 0) {
                    ota_url[--ota_url_len] = '\0';
                    draw_keyboard_input();
                }
            } else if (key >= ' ' && key <= '~' && ota_url_len < (int)(sizeof(ota_url) - 1)) {
                ota_url[ota_url_len++] = key;
                ota_url[ota_url_len] = '\0';
                draw_keyboard_input();
            }
        }
    }

    if (ui_state == ABOUT_STATE_OTA) {
        draw_update_button();
        draw_ota_status(false);
    }

    return ABOUT_RESULT_NONE;
}
