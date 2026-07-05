#include "ui_ntp.h"
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "ntp.h"
#include "ntp_benchmark.h"
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
static char            preset_labels[N_PRESETS][40];
static const char     *preset_label_ptrs[N_PRESETS];
static ui_list_touch_t list_touch;
static int             list_scroll = 0;

#define BENCH_BTN_W 88
#define BENCH_BTN_H 20
#define BENCH_BTN_X (DISPLAY_WIDTH - BENCH_BTN_W - 5)
#define BENCH_BTN_Y 5
#define BENCH_LIVE_QUIESCE_MS 2600
#define BENCH_KE_QUIESCE_MS 9000
#define BENCH_TASK_STACK_BYTES 24576

typedef enum {
    PRESET_BENCH_IDLE,
    PRESET_BENCH_RUNNING,
    PRESET_BENCH_DONE,
    PRESET_BENCH_FAILED,
} preset_bench_state_t;

static preset_bench_state_t preset_bench_state[N_PRESETS];
static int32_t              preset_bench_delay_us[N_PRESETS];
static bool                 preset_bench_nts[N_PRESETS];
static int                  preset_order[N_PRESETS];
static SemaphoreHandle_t    preset_bench_lock;
static volatile uint32_t    preset_bench_generation;
static uint32_t             last_drawn_bench_generation;
static bool                 preset_benchmarking;
static bool                 preset_bench_cancel;

// Main-task-owned snapshot of the labels/order actually on screen. Copied
// under the bench lock, then drawn (multi-ms of SPI) and hit-tested against
// WITHOUT the lock - so repaints don't stall the benchmark task, and a tap
// resolves to the row the user saw even if a probe reordered the live list
// between paint and tap.
static char                 drawn_labels[N_PRESETS][sizeof(preset_labels[0])];
static const char          *drawn_label_ptrs[N_PRESETS];
static int                  drawn_order[N_PRESETS];

static void rebuild_preset_order_locked(void);
static void rebuild_preset_labels_locked(void);

static void bench_lock(void) {
    if (preset_bench_lock) xSemaphoreTake(preset_bench_lock, portMAX_DELAY);
}

static void bench_unlock(void) {
    if (preset_bench_lock) xSemaphoreGive(preset_bench_lock);
}

static int preset_rank(int idx) {
    switch (preset_bench_state[idx]) {
    case PRESET_BENCH_DONE:    return 0;
    case PRESET_BENCH_RUNNING: return 1;
    case PRESET_BENCH_FAILED:  return 2;
    default:                   return 3;
    }
}

static bool preset_less(int a, int b) {
    int ra = preset_rank(a);
    int rb = preset_rank(b);
    if (ra != rb) return ra < rb;
    if (preset_bench_state[a] == PRESET_BENCH_DONE &&
        preset_bench_delay_us[a] != preset_bench_delay_us[b]) {
        return preset_bench_delay_us[a] < preset_bench_delay_us[b];
    }
    return a < b;
}

static void rebuild_preset_order_locked(void) {
    for (int i = 0; i < N_PRESETS; i++) preset_order[i] = i;

    for (int i = 1; i < N_PRESETS; i++) {
        int v = preset_order[i];
        int j = i - 1;
        while (j >= 0 && preset_less(v, preset_order[j])) {
            preset_order[j + 1] = preset_order[j];
            j--;
        }
        preset_order[j + 1] = v;
    }
}

static void format_delay_prefix(int32_t delay_us, char *buf, size_t len) {
    int32_t ms = (delay_us + 500) / 1000;
    if (ms < 1000) {
        snprintf(buf, len, "%ldms", (long)ms);
    } else {
        snprintf(buf, len, "%ld.%01lds", (long)(ms / 1000), (long)((ms % 1000) / 100));
    }
}

static void rebuild_preset_labels_locked(void) {
    for (int row = 0; row < N_PRESETS; row++) {
        int idx = preset_order[row];
        char prefix[20] = "";
        if (preset_bench_state[idx] == PRESET_BENCH_RUNNING) {
            str_copy(prefix, sizeof(prefix), "... ");
        } else if (preset_bench_state[idx] == PRESET_BENCH_DONE) {
            char d[16];
            format_delay_prefix(preset_bench_delay_us[idx], d, sizeof(d));
            snprintf(prefix, sizeof(prefix), "%s ", d);
        } else if (preset_bench_state[idx] == PRESET_BENCH_FAILED) {
            snprintf(prefix, sizeof(prefix), "%s ", tr(STR_FAIL));
        }

        bool show_nts = preset_bench_state[idx] == PRESET_BENCH_DONE
            ? preset_bench_nts[idx]
            : presets[idx].nts;
        snprintf(preset_labels[row], sizeof(preset_labels[row]), "%s%s%s",
                 prefix, presets[idx].host, show_nts ? " [NTS]" : "");
        preset_labels[row][38] = '\0';
        preset_label_ptrs[row] = preset_labels[row];
    }
}

// Resolve a tapped row against the snapshot that was last drawn (main task
// only; no lock needed).
static int preset_index_for_row(int row) {
    if (row < 0 || row >= N_PRESETS) return -1;
    return drawn_order[row];
}

static bool request_benchmark_cancel_locked(void) {
    if (!preset_benchmarking) return false;
    preset_bench_cancel = true;
    preset_bench_generation++;
    return true;
}

static void benchmark_mark_dirty_locked(void) {
    rebuild_preset_order_locked();
    rebuild_preset_labels_locked();
    preset_bench_generation++;
}

static void benchmark_task(void *arg) {
    (void)arg;
    bool prefer_ipv6 = ui_prefer_ipv6;
    nts_mode_t nts_mode = ui_nts_mode;

    uint32_t waited_ms = 0;
    while (ntp_nts_ke_in_flight() && waited_ms < BENCH_KE_QUIESCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
    if (waited_ms > 0) {
        ESP_LOGI(TAG, "Benchmark waited %lu ms for live NTS-KE",
                 (unsigned long)waited_ms);
    }
    vTaskDelay(pdMS_TO_TICKS(BENCH_LIVE_QUIESCE_MS));

    for (int i = 0; i < N_PRESETS; i++) {
        bench_lock();
        bool cancel = preset_bench_cancel;
        bench_unlock();
        if (cancel) break;

        bench_lock();
        preset_bench_state[i] = PRESET_BENCH_RUNNING;
        benchmark_mark_dirty_locked();
        bench_unlock();

        ntp_benchmark_result_t result;
        ntp_benchmark_status_t status =
            ntp_benchmark_server(presets[i].host, prefer_ipv6, nts_mode, &result);

        bench_lock();
        if (status == NTP_BENCHMARK_OK) {
            preset_bench_state[i] = PRESET_BENCH_DONE;
            preset_bench_delay_us[i] = result.delay_us;
            preset_bench_nts[i] = result.nts;
        } else {
            preset_bench_state[i] = PRESET_BENCH_FAILED;
            preset_bench_delay_us[i] = INT32_MAX;
            preset_bench_nts[i] = false;
        }
        benchmark_mark_dirty_locked();
        cancel = preset_bench_cancel;
        bench_unlock();

        if (cancel) break;
    }

    UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    ntp_set_poll_paused(false);
    ESP_LOGI(TAG, "Benchmark stopped (stack high water %lu)",
             (unsigned long)stack_hwm);

    bench_lock();
    preset_benchmarking = false;
    preset_bench_cancel = false;
    preset_bench_generation++;
    bench_unlock();
    vTaskDelete(NULL);
}

static void draw_benchmark_button(void) {
    bench_lock();
    bool running = preset_benchmarking;
    bool stopping = preset_benchmarking && preset_bench_cancel;
    bench_unlock();

    uint16_t bg = stopping ? COLOR_ORANGE : (running ? COLOR_CYAN : UI_COLOR_ITEM_BG);
    uint16_t fg = running ? COLOR_BLACK : COLOR_WHITE;
    const char *label = stopping ? tr(STR_STOPPING) : (running ? tr(STR_RUNNING) : tr(STR_BENCHMARK));
    ui_draw_button(BENCH_BTN_X, BENCH_BTN_Y, BENCH_BTN_W, BENCH_BTN_H, label, fg, bg);
}

static bool benchmark_button_hit(const touch_point_t *touch) {
    return touch->x >= BENCH_BTN_X && touch->x < BENCH_BTN_X + BENCH_BTN_W &&
           touch->y >= BENCH_BTN_Y && touch->y < BENCH_BTN_Y + BENCH_BTN_H;
}

static void start_benchmark(void) {
    bench_lock();
    if (preset_benchmarking) {
        bool cancel_requested = request_benchmark_cancel_locked();
        bench_unlock();
        if (cancel_requested) ESP_LOGI(TAG, "Benchmark cancel requested");
        return;
    }
    for (int i = 0; i < N_PRESETS; i++) {
        preset_bench_state[i] = PRESET_BENCH_IDLE;
        preset_bench_delay_us[i] = INT32_MAX;
        preset_bench_nts[i] = false;
    }
    preset_benchmarking = true;
    preset_bench_cancel = false;
    benchmark_mark_dirty_locked();
    bench_unlock();

    ntp_set_poll_paused(true);
    if (xTaskCreatePinnedToCore(benchmark_task, "ntp_bench",
                                BENCH_TASK_STACK_BYTES, NULL, 3, NULL, 0) != pdPASS) {
        ntp_set_poll_paused(false);
        bench_lock();
        preset_benchmarking = false;
        preset_bench_cancel = false;
        preset_bench_generation++;
        bench_unlock();
        ESP_LOGW(TAG, "Failed to start NTP benchmark task");
    }
}

static void cancel_benchmark(void) {
    bench_lock();
    bool cancel_requested = request_benchmark_cancel_locked();
    bench_unlock();
    if (cancel_requested) ESP_LOGI(TAG, "Benchmark cancel requested");
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

static void draw_ipv6_toggle(void) {
    bool ipv6 = ui_prefer_ipv6;
    uint16_t ipv6_bg = ipv6 ? COLOR_CYAN : COLOR_DARKGRAY;
    uint16_t ipv6_fg = ipv6 ? COLOR_BLACK : COLOR_WHITE;
    // The setting is an address-family preference, not a filter: DNS
    // still returns both families and the loser fills leftover peer
    // slots (an IPv4-only filter would never sync on an IPv6-only
    // network). Show the preference order, which also needs no
    // translation.
    ui_draw_button(IPV6_TOGGLE_X, IPV6_TOGGLE_Y, IPV6_TOGGLE_W, IPV6_TOGGLE_H,
                   ipv6 ? "IPv6 > IPv4" : "IPv4 > IPv6", ipv6_fg, ipv6_bg);
}

static void draw_nts_toggle(void) {
    const char *label;
    char label_buf[24];
    uint16_t bg, fg;
    switch (ui_nts_mode) {
    case NTS_MODE_REQUIRE:
        snprintf(label_buf, sizeof(label_buf), "NTS: %s", tr(STR_NTS_REQUIRE));
        label = label_buf; bg = COLOR_GREEN; fg = COLOR_BLACK; break;
    case NTS_MODE_OPPORTUNISTIC:
        snprintf(label_buf, sizeof(label_buf), "NTS: %s", tr(STR_NTS_ATTEMPT));
        label = label_buf; bg = COLOR_CYAN; fg = COLOR_BLACK; break;
    default:
        snprintf(label_buf, sizeof(label_buf), "NTS: %s", tr(STR_NTS_NO));
        label = label_buf; bg = COLOR_DARKGRAY; fg = COLOR_WHITE; break;
    }
    ui_draw_button(NTS_TOGGLE_X, NTS_TOGGLE_Y, NTS_TOGGLE_W, NTS_TOGGLE_H,
                   label, fg, bg);
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

    draw_ipv6_toggle();
    draw_nts_toggle();
}

static void draw_keyboard_screen(void) {
    display_fill(COLOR_BLACK);

    draw_edit_header();

    draw_server_field(true, false);
    draw_keyboard();
}

static int preset_highlight(void) {
    bench_lock();
    for (int row = 0; row < N_PRESETS; row++) {
        int idx = preset_order[row];
        if (strcmp(custom_server, presets[idx].host) == 0) {
            bench_unlock();
            return row;
        }
    }
    bench_unlock();
    return -1;
}

static void draw_presets_list(void) {
    bench_lock();
    uint32_t generation = preset_bench_generation;
    memcpy(drawn_labels, preset_labels, sizeof(drawn_labels));
    memcpy(drawn_order, preset_order, sizeof(drawn_order));
    bench_unlock();

    int highlight = -1;
    for (int row = 0; row < N_PRESETS; row++) {
        drawn_label_ptrs[row] = drawn_labels[row];
        if (strcmp(custom_server, presets[drawn_order[row]].host) == 0) {
            highlight = row;
        }
    }
    ui_draw_list(drawn_label_ptrs, N_PRESETS, list_scroll, highlight);
    last_drawn_bench_generation = generation;
}

static void draw_presets_screen(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_NTP_PRESETS), true);
    draw_benchmark_button();
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

    if (!preset_bench_lock) preset_bench_lock = xSemaphoreCreateMutex();
    bench_lock();
    for (int i = 0; i < N_PRESETS; i++) {
        preset_order[i] = i;
        if (preset_bench_state[i] != PRESET_BENCH_DONE) {
            preset_bench_delay_us[i] = INT32_MAX;
        }
    }
    rebuild_preset_order_locked();
    rebuild_preset_labels_locked();
    last_drawn_bench_generation = preset_bench_generation;
    bench_unlock();

    draw_main_screen();
}

// Apply (and persist) only the settings that changed - applying one is what
// triggers the engine resync, so unchanged settings leave the clock alone.
static void apply_pending_settings(void) {
    cancel_benchmark();

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
    bool pressed;
    bool touched = ui_read_touch_ex(&touch, &last_touch_time, &pressed);

    if (ui_state == NTP_STATE_PRESETS) {
        if (last_drawn_bench_generation != preset_bench_generation) {
            draw_benchmark_button();
            draw_presets_list();
        }

        ui_list_touch_result_t r =
            ui_list_touch_update(&list_touch, &touch, pressed, N_PRESETS, &list_scroll);
        if (r == UI_LIST_TOUCH_SCROLLED) {
            draw_presets_list();
        } else if (r == UI_LIST_TOUCH_TAPPED) {
            const touch_point_t *t = &list_touch.tap_start;
            if (ui_back_button_hit(t)) {
                cancel_benchmark();
                ui_state = NTP_STATE_MAIN;
                draw_main_screen();
            } else if (benchmark_button_hit(t)) {
                start_benchmark();
                draw_benchmark_button();
                draw_presets_list();
            } else {
                int item = ui_list_tap_to_item(t, list_scroll, N_PRESETS);
                int preset_idx = preset_index_for_row(item);
                if (preset_idx >= 0) {
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
            } else if (touch.y >= IPV6_TOGGLE_Y && touch.y < IPV6_TOGGLE_Y + IPV6_TOGGLE_H &&
                touch.x >= IPV6_TOGGLE_X && touch.x < IPV6_TOGGLE_X + IPV6_TOGGLE_W) {
                ui_prefer_ipv6 = !ui_prefer_ipv6;
                draw_ipv6_toggle();
                ui_wait_for_touch_release();  // one tap, one toggle
            } else if (touch.y >= NTS_TOGGLE_Y && touch.y < NTS_TOGGLE_Y + NTS_TOGGLE_H &&
                touch.x >= NTS_TOGGLE_X && touch.x < NTS_TOGGLE_X + NTS_TOGGLE_W) {
                // Cycle NTS mode: No -> Attempt (opportunistic) -> Require -> No.
                ui_nts_mode = (nts_mode_t)((ui_nts_mode + 1) % 3);
                draw_nts_toggle();
                ui_wait_for_touch_release();  // a held press otherwise cycles every debounce tick
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
