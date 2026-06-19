#include "ui_ntp.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_common.h"
#include "ui_keyboard.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_ntp";

#define SERVER_LABEL_Y  40
#define SERVER_BOX_Y    (SERVER_LABEL_Y + 20)
#define SERVER_BOX_H    28
#define ROW_GAP         8
#define SECTION_GAP     18   // extra gap before the toggle row
// Presets button, below the hostname box.
#define PRESETS_BTN_X   10
#define PRESETS_BTN_Y   (SERVER_BOX_Y + SERVER_BOX_H + ROW_GAP)
#define PRESETS_BTN_W   (DISPLAY_WIDTH - 20)
#define PRESETS_BTN_H   28
// IPv6 / NTS toggles: two equal-width buttons + gap, spanning the box width.
#define TOGGLE_GAP      10
#define IPV6_TOGGLE_X   10
#define IPV6_TOGGLE_Y   (PRESETS_BTN_Y + PRESETS_BTN_H + SECTION_GAP)
#define IPV6_TOGGLE_W   145
#define IPV6_TOGGLE_H   28
#define NTS_TOGGLE_X    (IPV6_TOGGLE_X + IPV6_TOGGLE_W + TOGGLE_GAP)
#define NTS_TOGGLE_Y    IPV6_TOGGLE_Y
#define NTS_TOGGLE_W    IPV6_TOGGLE_W
#define NTS_TOGGLE_H    IPV6_TOGGLE_H

static const char *keyboard_rows[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-_",
};

typedef enum {
    NTP_STATE_MAIN,
    NTP_STATE_KEYBOARD,
    NTP_STATE_PRESETS,
} ntp_ui_state_t;

static ntp_ui_state_t ui_state = NTP_STATE_MAIN;
static uint32_t last_touch_time = 0;
// Edits stay pending in these locals and are applied only on Back, if changed.
static char custom_server[64] = {0};
static int custom_server_len = 0;
static char kb_server_backup[64] = {0};
static bool ui_prefer_ipv6 = false;
static nts_mode_t ui_nts_mode = NTS_MODE_OPPORTUNISTIC;

// Server presets, shown by hostname; the nts flag is only a [NTS] display hint.
typedef struct { const char *host; bool nts; } ntp_preset_t;
static const ntp_preset_t presets[] = {
    { "time.apple.com",             false },
    { "time.cloudflare.com",        true  },
    { "time.facebook.com",          false },
    { "time.google.com",            false },
    { "ntp1.inrim.it",              false },  // INRIM, Italy
    { "time.kriss.re.kr",           false },  // KRISS, South Korea
    { "ntp.metas.ch",               false },  // METAS, Switzerland
    { "time.metrologie.at",         false },  // BEV, Austria
    { "tick.usno.navy.mil",         false },  // USNO, USA
    { "nts.netnod.se",              true  },  // Netnod, Sweden
    { "ntp.nict.jp",                false },  // NICT, Japan
    { "time.nist.gov",              false },  // NIST, USA
    { "ntp1.npl.co.uk",             false },  // NPL, UK
    { "time.nplindia.org",          false },  // NPL, India
    { "time.nrc.ca",                false },  // NRC, Canada
    { "pool.ntp.org",               false },
    { "ntp.obspm.fr",               false },  // Observatoire de Paris, France
    { "ptbtime1.ptb.de",            true  },  // PTB, Germany
    { "nts1.ri.se",                 true  },  // RISE, Sweden
    { "brazil.time.system76.com",   true  },
    { "ohio.time.system76.com",     true  },
    { "oregon.time.system76.com",   true  },
    { "paris.time.system76.com",    true  },
    { "virginia.time.system76.com", true  },
    { "ntp.vsl.nl",                 false },  // VSL, Netherlands
    { "time.windows.com",           false },
};
#define N_PRESETS ((int)(sizeof(presets) / sizeof(presets[0])))
static char            preset_labels[N_PRESETS][40];
static const char     *preset_label_ptrs[N_PRESETS];
static ui_list_touch_t list_touch;
static int             list_scroll = 0;

static void draw_keyboard(void) {
    display_fill_rect(0, KEYBOARD_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - KEYBOARD_Y, COLOR_BLACK);

    ui_keyboard_draw_keys(keyboard_rows, 4, KEYBOARD_Y, COLOR_DARKGRAY, COLOR_WHITE, COLOR_GRAY);

    int y = ui_keyboard_bottom_y(4, KEYBOARD_Y);
    int btn_h = KB_KEY_HEIGHT - 2;

    display_fill_rect(10, y, 80, btn_h, COLOR_RED);
    display_string(26, y + 3, "Cancel", COLOR_WHITE, COLOR_RED);

    display_fill_rect(120, y, 80, btn_h, COLOR_GRAY);
    display_string(144, y + 3, "Del", COLOR_WHITE, COLOR_GRAY);

    display_fill_rect(230, y, 80, btn_h, COLOR_GREEN);
    display_string(254, y + 3, "Done", COLOR_BLACK, COLOR_GREEN);
}

static void draw_server_input(void) {
    display_fill_rect(0, 35, DISPLAY_WIDTH, 30, COLOR_BLACK);
    display_string(10, 38, "Server:", COLOR_GRAY, COLOR_BLACK);

    display_fill_rect(10, 55, DISPLAY_WIDTH - 20, 20, COLOR_DARKGRAY);

    if (custom_server_len > 0) {
        const char *display_str = custom_server;
        if (custom_server_len > 35) {
            display_str = custom_server + custom_server_len - 35;
        }
        display_string(15, 59, display_str, COLOR_WHITE, COLOR_DARKGRAY);
    }

    int cursor_x = 15 + (custom_server_len > 35 ? 35 : custom_server_len) * FONT_CHAR_WIDTH;
    if (cursor_x < DISPLAY_WIDTH - 20) {
        display_string(cursor_x, 59, "_", COLOR_CYAN, COLOR_DARKGRAY);
    }
}

static void draw_ipv6_toggle(void) {
    bool ipv6 = ui_prefer_ipv6;
    uint16_t ipv6_bg = ipv6 ? COLOR_CYAN : COLOR_DARKGRAY;
    uint16_t ipv6_fg = ipv6 ? COLOR_BLACK : COLOR_WHITE;
    const char *ipv6_label = ipv6 ? "IPv6: On" : "IPv6: Off";
    display_fill_rect(IPV6_TOGGLE_X, IPV6_TOGGLE_Y, IPV6_TOGGLE_W, IPV6_TOGGLE_H, ipv6_bg);
    int label_x = IPV6_TOGGLE_X +
        (IPV6_TOGGLE_W - (int)strlen(ipv6_label) * FONT_CHAR_WIDTH) / 2;
    display_string(label_x, IPV6_TOGGLE_Y + 7, ipv6_label, ipv6_fg, ipv6_bg);
}

static void draw_nts_toggle(void) {
    const char *label;
    uint16_t bg, fg;
    switch (ui_nts_mode) {
    case NTS_MODE_REQUIRE:       label = "NTS: Req"; bg = COLOR_GREEN;    fg = COLOR_BLACK; break;
    case NTS_MODE_OPPORTUNISTIC: label = "NTS: On";  bg = COLOR_CYAN;     fg = COLOR_BLACK; break;
    default:                     label = "NTS: Off"; bg = COLOR_DARKGRAY; fg = COLOR_WHITE; break;
    }
    display_fill_rect(NTS_TOGGLE_X, NTS_TOGGLE_Y, NTS_TOGGLE_W, NTS_TOGGLE_H, bg);
    int label_x = NTS_TOGGLE_X +
        (NTS_TOGGLE_W - (int)strlen(label) * FONT_CHAR_WIDTH) / 2;
    display_string(label_x, NTS_TOGGLE_Y + 7, label, fg, bg);
}

static char get_key_at(int16_t x, int16_t y) {
    char key = ui_keyboard_get_key(keyboard_rows, 4, KEYBOARD_Y, x, y);
    if (key) return key;

    int btn_y = ui_keyboard_bottom_y(4, KEYBOARD_Y);
    if (y >= btn_y && y < btn_y + KB_KEY_HEIGHT) {
        if (x >= 10 && x < 90) return VKEY_ESCAPE;
        if (x >= 120 && x < 200) return VKEY_BACKSPACE;
        if (x >= 230 && x < 310) return VKEY_ENTER;
    }

    return 0;
}

static void draw_main_screen(void) {
    display_fill(COLOR_BLACK);

    ui_draw_header("NTP Settings", true);

    display_string(10, SERVER_LABEL_Y, "Server:", COLOR_GRAY, COLOR_BLACK);

    display_fill_rect(10, SERVER_BOX_Y, DISPLAY_WIDTH - 20, SERVER_BOX_H, COLOR_DARKGRAY);
    char display_server[38];
    str_copy(display_server, sizeof(display_server), custom_server);
    display_string(15, SERVER_BOX_Y + 7, display_server, COLOR_WHITE, COLOR_DARKGRAY);
    display_string(DISPLAY_WIDTH - 30, SERVER_BOX_Y + 7, ">", COLOR_WHITE, COLOR_DARKGRAY);

    display_fill_rect(PRESETS_BTN_X, PRESETS_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H, UI_COLOR_ITEM_BG);
    int px = PRESETS_BTN_X + (PRESETS_BTN_W - 7 * FONT_CHAR_WIDTH) / 2;  // "Presets"
    display_string(px, PRESETS_BTN_Y + 7, "Presets", COLOR_WHITE, UI_COLOR_ITEM_BG);

    draw_ipv6_toggle();
    draw_nts_toggle();
}

static void draw_keyboard_screen(void) {
    display_fill(COLOR_BLACK);

    ui_draw_header("NTP Server", false);

    draw_server_input();
    draw_keyboard();
}

static int preset_highlight(void) {
    for (int i = 0; i < N_PRESETS; i++) {
        if (strcmp(custom_server, presets[i].host) == 0) return i;
    }
    return -1;
}

static void draw_presets_list(void) {
    ui_draw_list(preset_label_ptrs, N_PRESETS, list_scroll, preset_highlight());
}

static void draw_presets_screen(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("NTP Presets", true);
    draw_presets_list();
}

void ui_ntp_init(void) {
    ESP_LOGI(TAG, "Initializing NTP settings UI");
    last_touch_time = 0;
    ui_state = NTP_STATE_MAIN;

    str_copy(custom_server, sizeof(custom_server), wifi_get_custom_ntp_server());
    custom_server_len = strlen(custom_server);
    ui_prefer_ipv6 = wifi_get_ntp_prefer_ipv6();
    ui_nts_mode = wifi_get_nts_mode();

    for (int i = 0; i < N_PRESETS; i++) {
        snprintf(preset_labels[i], sizeof(preset_labels[i]), "%s%s",
                 presets[i].host, presets[i].nts ? " [NTS]" : "");
        preset_label_ptrs[i] = preset_labels[i];
    }

    draw_main_screen();
}

// Apply (and persist) only the settings that changed - applying one is what
// triggers the engine resync, so unchanged settings leave the clock alone.
static void apply_pending_settings(void) {
    if (custom_server_len > 0 &&
        strcmp(custom_server, wifi_get_custom_ntp_server()) != 0) {
        wifi_set_custom_ntp_server(custom_server);
        nvs_config_set_custom_ntp_server(custom_server);
    }
    if (ui_prefer_ipv6 != wifi_get_ntp_prefer_ipv6()) {
        wifi_set_ntp_prefer_ipv6(ui_prefer_ipv6);
        nvs_config_set_ntp_ipv6(ui_prefer_ipv6);
    }
    if (ui_nts_mode != wifi_get_nts_mode()) {
        wifi_set_nts_mode(ui_nts_mode);
        nvs_config_set_nts_mode((uint8_t)ui_nts_mode);
    }
}

ntp_result_t ui_ntp_update(void) {
    touch_point_t touch;
    bool pressed = touch_read(&touch);
    bool touched = false;
    if (pressed && !ui_should_debounce(last_touch_time)) {
        last_touch_time = xTaskGetTickCount();
        touched = true;
    }

    if (ui_state == NTP_STATE_PRESETS) {
        ui_list_touch_result_t r =
            ui_list_touch_update(&list_touch, &touch, pressed, N_PRESETS, &list_scroll);
        if (r == UI_LIST_TOUCH_SCROLLED) {
            draw_presets_list();
        } else if (r == UI_LIST_TOUCH_TAPPED) {
            const touch_point_t *t = &list_touch.tap_start;
            if (ui_back_button_hit(t)) {
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (t->y >= UI_LIST_START_Y &&
                       t->y < UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H) {
                int item = (t->y - UI_LIST_START_Y) / UI_LIST_ITEM_H + list_scroll;
                if (item < N_PRESETS) {
                    str_copy(custom_server, sizeof(custom_server), presets[item].host);
                    custom_server_len = strlen(custom_server);
                    ui_state = NTP_STATE_MAIN;
                    draw_main_screen();
                }
            }
        }
        return NTP_RESULT_NONE;
    }

    if (touched) {
        if (ui_state == NTP_STATE_MAIN) {
            if (ui_back_button_hit(&touch)) {
                apply_pending_settings();
                return NTP_RESULT_BACK;
            }

            if (touch.y >= SERVER_BOX_Y && touch.y < SERVER_BOX_Y + SERVER_BOX_H) {
                str_copy(kb_server_backup, sizeof(kb_server_backup), custom_server);
                ui_state = NTP_STATE_KEYBOARD;
                draw_keyboard_screen();
            }

            if (touch.y >= IPV6_TOGGLE_Y && touch.y < IPV6_TOGGLE_Y + IPV6_TOGGLE_H &&
                touch.x >= IPV6_TOGGLE_X && touch.x < IPV6_TOGGLE_X + IPV6_TOGGLE_W) {
                ui_prefer_ipv6 = !ui_prefer_ipv6;
                draw_ipv6_toggle();
            }

            // Cycle NTS mode: Off -> On (opportunistic) -> Req (required) -> Off.
            if (touch.y >= NTS_TOGGLE_Y && touch.y < NTS_TOGGLE_Y + NTS_TOGGLE_H &&
                touch.x >= NTS_TOGGLE_X && touch.x < NTS_TOGGLE_X + NTS_TOGGLE_W) {
                ui_nts_mode = (nts_mode_t)((ui_nts_mode + 1) % 3);
                draw_nts_toggle();
            }

            if (touch.y >= PRESETS_BTN_Y && touch.y < PRESETS_BTN_Y + PRESETS_BTN_H &&
                touch.x >= PRESETS_BTN_X && touch.x < PRESETS_BTN_X + PRESETS_BTN_W) {
                int hl = preset_highlight();
                list_scroll = ui_list_scroll_to_item(hl < 0 ? 0 : hl, N_PRESETS);
                ui_list_touch_reset(&list_touch);
                ui_state = NTP_STATE_PRESETS;
                draw_presets_screen();
                ui_wait_for_touch_release();  // don't carry this tap into the list
            }
        } else if (ui_state == NTP_STATE_KEYBOARD) {
            char key = get_key_at(touch.x, touch.y);

            if (key == VKEY_ESCAPE) {
                str_copy(custom_server, sizeof(custom_server), kb_server_backup);
                custom_server_len = strlen(custom_server);
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (key == VKEY_ENTER) {
                // Empty entry keeps the prior value; otherwise it stays pending.
                if (custom_server_len == 0) {
                    str_copy(custom_server, sizeof(custom_server), kb_server_backup);
                    custom_server_len = strlen(custom_server);
                }
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (key == VKEY_BACKSPACE) {
                if (custom_server_len > 0) {
                    custom_server_len--;
                    custom_server[custom_server_len] = '\0';
                    draw_server_input();
                }
            } else if (key >= ' ' && key <= '~' && custom_server_len < (int)(sizeof(custom_server) - 1)) {
                custom_server[custom_server_len++] = key;
                custom_server[custom_server_len] = '\0';
                draw_server_input();
            }
        }
    }

    return NTP_RESULT_NONE;
}
