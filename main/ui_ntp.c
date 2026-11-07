#include "ui_ntp.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "config.h"
#include "display.h"
#include "ntp.h"
#include "nvs_config.h"
#include "touch.h"
#include "i18n.h"
#include "ui_common.h"
#include "ui_keyboard.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_ntp";

#define SERVER_LABEL_Y  40
#define SERVER_BOX_X    10
#define SERVER_BOX_Y    (SERVER_LABEL_Y + 20)
#define SERVER_BOX_W    (DISPLAY_WIDTH - 20)
#define SERVER_BOX_H    28
#define SERVER_TEXT_X   (SERVER_BOX_X + 5)
#define SERVER_TEXT_Y   (SERVER_BOX_Y + 5)
#define SERVER_TEXT_CHARS 35
#define ROW_GAP         8
#define SECTION_GAP     18   // extra gap before the toggle row
// Presets button, below the hostname box.
#define PRESETS_BTN_X   10
#define PRESETS_BTN_Y   (SERVER_BOX_Y + SERVER_BOX_H + ROW_GAP)
#define PRESETS_BTN_W   (DISPLAY_WIDTH - 20)
#define PRESETS_BTN_H   28
// Address family / NTS toggles (chrony's "ipv4"/"ipv6" and "nts" source
// options): two equal-width buttons + gap, spanning the box width.
#define TOGGLE_GAP      10
#define FAMILY_TOGGLE_X 10
#define FAMILY_TOGGLE_Y (PRESETS_BTN_Y + PRESETS_BTN_H + SECTION_GAP)
#define FAMILY_TOGGLE_W 145
#define FAMILY_TOGGLE_H 28
#define NTS_TOGGLE_X    (FAMILY_TOGGLE_X + FAMILY_TOGGLE_W + TOGGLE_GAP)
#define NTS_TOGGLE_Y    FAMILY_TOGGLE_Y
#define NTS_TOGGLE_W    FAMILY_TOGGLE_W
#define NTS_TOGGLE_H    FAMILY_TOGGLE_H
#define EDIT_DONE_BTN_X UI_HEADER_RBTN_X
#define EDIT_DEL_BTN_W  50
#define EDIT_DEL_BTN_H  20
#define EDIT_DEL_BTN_X  (DISPLAY_WIDTH - UI_BACK_BTN_X - EDIT_DEL_BTN_W)
#define EDIT_DEL_BTN_Y  (KEYBOARD_Y - EDIT_DEL_BTN_H - KB_KEY_SPACING)

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
static ntp_family_t ui_family = NTP_FAMILY_ANY;
static bool ui_nts = false;

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
    { "time.metrologie.at",         true  },  // BEV, Austria
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
// The longest preset host and " [NTS]"; rows draw from x=10 and clip at the
// panel edge.
static char            preset_labels[N_PRESETS][40];
static const char     *preset_label_ptrs[N_PRESETS];
static ui_list_touch_t list_touch;
static int             list_scroll = 0;

static void rebuild_preset_labels(void) {
    for (int i = 0; i < N_PRESETS; i++) {
        snprintf(preset_labels[i], sizeof(preset_labels[i]), "%s%s",
                 presets[i].host, presets[i].nts ? " [NTS]" : "");
        preset_label_ptrs[i] = preset_labels[i];
    }
}

static void draw_keyboard(void) {
    display_fill_rect(0, EDIT_DEL_BTN_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - EDIT_DEL_BTN_Y, COLOR_BLACK);

    ui_draw_button(EDIT_DEL_BTN_X, EDIT_DEL_BTN_Y, EDIT_DEL_BTN_W, EDIT_DEL_BTN_H,
                   tr(STR_DEL), COLOR_WHITE, COLOR_GRAY);

    ui_keyboard_draw_keys(keyboard_rows, 4, KEYBOARD_Y, COLOR_DARKGRAY, COLOR_WHITE, COLOR_GRAY);
}

static void draw_edit_header(void) {
    ui_draw_header(tr(STR_NTP_SERVER), false);

    ui_draw_button(UI_BACK_BTN_X, UI_HEADER_BTN_Y, UI_BACK_BTN_W, UI_HEADER_BTN_H,
                   tr(STR_CANCEL), COLOR_WHITE, UI_COLOR_ITEM_BG);
    ui_draw_button(EDIT_DONE_BTN_X, UI_HEADER_BTN_Y, UI_BACK_BTN_W, UI_HEADER_BTN_H,
                   tr(STR_DONE), COLOR_BLACK, COLOR_GREEN);
}

static void draw_server_field(bool show_cursor, bool show_chevron) {
    display_fill_rect(0, SERVER_LABEL_Y, DISPLAY_WIDTH, SERVER_BOX_Y + SERVER_BOX_H - SERVER_LABEL_Y, COLOR_BLACK);
    display_string(10, SERVER_LABEL_Y, tr(STR_SERVER_LABEL), COLOR_GRAY, COLOR_BLACK);

    int max_chars = show_chevron ? SERVER_TEXT_CHARS - 2 : SERVER_TEXT_CHARS;
    ui_draw_text_field(SERVER_BOX_X, SERVER_BOX_Y, SERVER_BOX_W, SERVER_BOX_H,
                       SERVER_TEXT_X, SERVER_TEXT_Y, custom_server, custom_server_len,
                       max_chars, show_cursor, COLOR_WHITE, COLOR_CYAN, COLOR_DARKGRAY);

    if (show_chevron) {
        display_string(DISPLAY_WIDTH - 30, SERVER_TEXT_Y, ">", COLOR_WHITE, COLOR_DARKGRAY);
    }
}

static void draw_family_toggle(void) {
    // chrony's source options: none (both families), "ipv4", "ipv6".
    static const char *const labels[] = { "ipv4 + ipv6", "ipv4", "ipv6" };
    bool restricted = ui_family != NTP_FAMILY_ANY;
    ui_draw_button(FAMILY_TOGGLE_X, FAMILY_TOGGLE_Y, FAMILY_TOGGLE_W, FAMILY_TOGGLE_H,
                   labels[ui_family], restricted ? COLOR_BLACK : COLOR_WHITE,
                   restricted ? COLOR_CYAN : COLOR_DARKGRAY);
}

static void draw_nts_toggle(void) {
    char label[24];
    snprintf(label, sizeof(label), "NTS: %s", tr(ui_nts ? STR_ON : STR_OFF));
    ui_draw_button(NTS_TOGGLE_X, NTS_TOGGLE_Y, NTS_TOGGLE_W, NTS_TOGGLE_H, label,
                   ui_nts ? COLOR_BLACK : COLOR_WHITE, ui_nts ? COLOR_GREEN : COLOR_DARKGRAY);
}

static char get_key_at(int16_t x, int16_t y) {
    if (y < UI_HEADER_HEIGHT) {
        if (x < UI_BACK_BTN_X + UI_BACK_BTN_W) return VKEY_ESCAPE;
        if (x >= EDIT_DONE_BTN_X) return VKEY_ENTER;
    }

    if (x >= EDIT_DEL_BTN_X && x < EDIT_DEL_BTN_X + EDIT_DEL_BTN_W &&
        y >= EDIT_DEL_BTN_Y && y < EDIT_DEL_BTN_Y + EDIT_DEL_BTN_H) {
        return VKEY_BACKSPACE;
    }

    char key = ui_keyboard_get_key(keyboard_rows, 4, KEYBOARD_Y, x, y);
    if (key) return key;

    return 0;
}

static void draw_main_screen(void) {
    display_fill(COLOR_BLACK);

    ui_draw_header(tr(STR_NTP_SETTINGS), true);

    draw_server_field(false, true);

    ui_draw_button(PRESETS_BTN_X, PRESETS_BTN_Y, PRESETS_BTN_W, PRESETS_BTN_H,
                   tr(STR_PRESETS), COLOR_WHITE, UI_COLOR_ITEM_BG);

    draw_family_toggle();
    draw_nts_toggle();
}

static void draw_keyboard_screen(void) {
    display_fill(COLOR_BLACK);

    draw_edit_header();

    draw_server_field(true, false);
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
    ui_draw_header(tr(STR_NTP_PRESETS), true);
    draw_presets_list();
}

void ui_ntp_init(void) {
    ESP_LOGI(TAG, "Initializing NTP settings UI");
    last_touch_time = 0;
    ui_state = NTP_STATE_MAIN;

    str_copy(custom_server, sizeof(custom_server), wifi_get_custom_ntp_server());
    custom_server_len = strlen(custom_server);
    ui_family = wifi_get_ntp_family();
    ui_nts = wifi_get_nts();
    rebuild_preset_labels();

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
    if (ui_family != wifi_get_ntp_family()) {
        wifi_set_ntp_family(ui_family);
        nvs_config_set_ntp_family((uint8_t)ui_family);
    }
    if (ui_nts != wifi_get_nts()) {
        wifi_set_nts(ui_nts);
        nvs_config_set_nts(ui_nts);
    }
}

ntp_result_t ui_ntp_update(void) {
    touch_point_t touch;
    bool pressed;
    bool touched = ui_read_touch_ex(&touch, &last_touch_time, &pressed);

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
            } else {
                int preset_idx = ui_list_tap_to_item(t, list_scroll, N_PRESETS);
                if (preset_idx >= 0 && preset_idx < N_PRESETS) {
                    str_copy(custom_server, sizeof(custom_server), presets[preset_idx].host);
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

            // One tap resolves to at most one zone; the chain also prevents a
            // state change above from letting the same tap fall through to a
            // zone that happens to overlap on the next screen.
            if (touch.y >= SERVER_BOX_Y && touch.y < SERVER_BOX_Y + SERVER_BOX_H) {
                str_copy(kb_server_backup, sizeof(kb_server_backup), custom_server);
                ui_state = NTP_STATE_KEYBOARD;
                draw_keyboard_screen();
            } else if (touch.y >= FAMILY_TOGGLE_Y && touch.y < FAMILY_TOGGLE_Y + FAMILY_TOGGLE_H &&
                touch.x >= FAMILY_TOGGLE_X && touch.x < FAMILY_TOGGLE_X + FAMILY_TOGGLE_W) {
                ui_family = (ntp_family_t)((ui_family + 1) % 3);
                draw_family_toggle();
                ui_wait_for_touch_release();  // one tap, one step
            } else if (touch.y >= NTS_TOGGLE_Y && touch.y < NTS_TOGGLE_Y + NTS_TOGGLE_H &&
                touch.x >= NTS_TOGGLE_X && touch.x < NTS_TOGGLE_X + NTS_TOGGLE_W) {
                ui_nts = !ui_nts;
                draw_nts_toggle();
                ui_wait_for_touch_release();  // a held press otherwise toggles every debounce tick
            } else if (touch.y >= PRESETS_BTN_Y && touch.y < PRESETS_BTN_Y + PRESETS_BTN_H &&
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
                // Cancel overlaps MAIN's Back; don't let the held press pop
                // again and commit the settings being edited.
                ui_wait_for_touch_release();
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
                    draw_server_field(true, false);
                }
            } else if (key >= ' ' && key <= '~' && custom_server_len < (int)(sizeof(custom_server) - 1)) {
                custom_server[custom_server_len++] = key;
                custom_server[custom_server_len] = '\0';
                draw_server_field(true, false);
            }
        }
    }

    return NTP_RESULT_NONE;
}
