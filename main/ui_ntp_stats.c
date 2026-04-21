#include "ui_ntp_stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "ntp.h"
#include "touch.h"
#include "ui_common.h"

static const char *TAG = "ui_ntp_stats";

#define SYS_ROWS      5
#define SYS_Y_START   36
#define SYS_LINE_H    18
#define PEER_HDR_Y    (SYS_Y_START + SYS_ROWS * SYS_LINE_H)
#define PEER_Y_START  (PEER_HDR_Y + 18)
#define PEER_LINE_H   18

static uint32_t last_touch_time = 0;
static uint32_t last_refresh_ms = 0;

// Cached rendered line contents - skip repaint when unchanged, and when only
// a few chars change, update just those cells instead of blanking the row.
static char     last_sys_row[SYS_ROWS][96];
static uint16_t last_sys_color[SYS_ROWS];
static char     last_peer_row[NTP_MAX_PEERS][96];
static uint16_t last_peer_color[NTP_MAX_PEERS];

static void fmt_duration(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60) snprintf(buf, len, "%lus", (unsigned long)seconds);
    else if (seconds < 3600) snprintf(buf, len, "%lum", (unsigned long)(seconds / 60));
    else if (seconds < 86400) snprintf(buf, len, "%luh", (unsigned long)(seconds / 3600));
    else snprintf(buf, len, "%lud", (unsigned long)(seconds / 86400));
}

static void fmt_offset_us(char *buf, size_t len, int64_t us) {
    int64_t av = us < 0 ? -us : us;
    char sign = (us < 0) ? '-' : '+';
    if (av < 1000) {
        snprintf(buf, len, "%c0.%03lldms", sign, (long long)av);
    } else if (av < 10000000LL) {
        snprintf(buf, len, "%c%lld.%02lldms", sign,
                 (long long)(av / 1000), (long long)((av % 1000) / 10));
    } else {
        snprintf(buf, len, "%c%llds", sign, (long long)(av / 1000000));
    }
}

static void fmt_ppm_x1000(char *buf, size_t len, int32_t val) {
    char sign = (val < 0) ? '-' : '+';
    uint32_t av = val < 0 ? (uint32_t)-val : (uint32_t)val;
    snprintf(buf, len, "%c%lu.%02luppm", sign,
             (unsigned long)(av / 1000),
             (unsigned long)((av % 1000) / 10));
}

// Paint new_text starting at (x, y) vs old_text already on screen, touching
// only the cells that differ. Handles shrink (erases tail) and grow (draws
// extra chars). Color change forces a full repaint of the value.
static void diff_paint(int x, int y, const char *old_text, const char *new_text,
                       uint16_t fg, uint16_t bg, bool force_full) {
    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t min_len = new_len < old_len ? new_len : old_len;

    for (size_t i = 0; i < min_len; i++) {
        if (force_full || old_text[i] != new_text[i]) {
            display_char(x + (int)i * FONT_CHAR_WIDTH, y, new_text[i], fg, bg);
        }
    }
    for (size_t i = min_len; i < new_len; i++) {
        display_char(x + (int)i * FONT_CHAR_WIDTH, y, new_text[i], fg, bg);
    }
    if (old_len > new_len) {
        display_fill_rect(x + (int)new_len * FONT_CHAR_WIDTH, y,
                          (int)(old_len - new_len) * FONT_CHAR_WIDTH,
                          FONT_CHAR_HEIGHT, bg);
    }
}

// A colored text segment, used to render rows that mix gray inline labels
// ("Syncs:", "Jitter:") with white values on a single line.
typedef struct {
    const char *text;
    uint16_t    color;
} segment_t;

// Flatten segments into a contiguous text + parallel per-char color array.
static size_t flatten_segments(const segment_t *segs, int nsegs,
                               char *text_out, uint16_t *colors_out, size_t cap) {
    size_t pos = 0;
    for (int i = 0; i < nsegs; i++) {
        size_t len = strlen(segs[i].text);
        for (size_t j = 0; j < len && pos + 1 < cap; j++) {
            text_out[pos]   = segs[i].text[j];
            colors_out[pos] = segs[i].color;
            pos++;
        }
    }
    text_out[pos] = '\0';
    return pos;
}

// Color-per-character variant of diff_paint for multi-colored rows.
static void diff_paint_multicolor(int x, int y,
                                  const char *old_text, const char *new_text,
                                  const uint16_t *new_colors,
                                  uint16_t bg, bool force_full) {
    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t min_len = new_len < old_len ? new_len : old_len;

    for (size_t i = 0; i < min_len; i++) {
        if (force_full || old_text[i] != new_text[i]) {
            display_char(x + (int)i * FONT_CHAR_WIDTH, y,
                         new_text[i], new_colors[i], bg);
        }
    }
    for (size_t i = min_len; i < new_len; i++) {
        display_char(x + (int)i * FONT_CHAR_WIDTH, y,
                     new_text[i], new_colors[i], bg);
    }
    if (old_len > new_len) {
        display_fill_rect(x + (int)new_len * FONT_CHAR_WIDTH, y,
                          (int)(old_len - new_len) * FONT_CHAR_WIDTH,
                          FONT_CHAR_HEIGHT, bg);
    }
}

static void draw_segmented_field_cached(int x, int y, int row_idx,
                                        const char *label,
                                        const segment_t *segs, int nsegs) {
    char     text[96];
    uint16_t colors[96];
    flatten_segments(segs, nsegs, text, colors, sizeof(text));

    char *cache = last_sys_row[row_idx];
    bool first_time = (cache[0] == '\0');
    if (!first_time && strcmp(cache, text) == 0) return;

    int vx = x + (int)strlen(label) * FONT_CHAR_WIDTH;
    if (first_time) {
        display_string(x, y, label, COLOR_GRAY, COLOR_BLACK);
    }
    diff_paint_multicolor(vx, y, cache, text, colors, COLOR_BLACK, first_time);

    strncpy(cache, text, sizeof(last_sys_row[row_idx]) - 1);
    cache[sizeof(last_sys_row[row_idx]) - 1] = '\0';
    last_sys_color[row_idx] = 0;   // unused for segmented rows
}

// Draw "label: value" at (x, y) with per-row caching + char-level diff so
// only changed cells get written. Label is painted once when the cache is
// empty; value is diffed against the previous render.
static void draw_field_cached(int x, int y, int row_idx,
                              const char *label, const char *value,
                              uint16_t val_color) {
    char *cache = last_sys_row[row_idx];
    uint16_t prev_color = last_sys_color[row_idx];
    bool first_time = (cache[0] == '\0');
    bool color_changed = !first_time && prev_color != val_color;

    if (!first_time && !color_changed && strcmp(cache, value) == 0) return;

    int vx = x + (int)strlen(label) * FONT_CHAR_WIDTH;
    if (first_time) {
        display_string(x, y, label, COLOR_GRAY, COLOR_BLACK);
    }

    diff_paint(vx, y, cache, value, val_color, COLOR_BLACK,
               first_time || color_changed);

    strncpy(cache, value, sizeof(last_sys_row[row_idx]) - 1);
    cache[sizeof(last_sys_row[row_idx]) - 1] = '\0';
    last_sys_color[row_idx] = val_color;
}

static void draw_static_chrome(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("NTP Stats", false);
    // Peer section divider label
    display_fill_rect(0, PEER_HDR_Y, DISPLAY_WIDTH, FONT_CHAR_HEIGHT, COLOR_BLACK);
    ui_draw_centered_string(PEER_HDR_Y, "--- Peers ---", COLOR_GRAY, COLOR_BLACK, false);
}

static void draw_peer_row(int slot, const ntp_peer_stats_t *p) {
    int y = PEER_Y_START + slot * PEER_LINE_H;

    char line[64];
    uint16_t row_fg;
    if (!p || !p->active) {
        snprintf(line, sizeof(line), "-");
        row_fg = COLOR_DARKGRAY;
    } else {
        row_fg = p->selected ? COLOR_CYAN : COLOR_WHITE;
        char addr[18];
        strncpy(addr, p->addr_str, sizeof(addr) - 1);
        addr[sizeof(addr) - 1] = '\0';

        char off_buf[16], age_buf[8];
        if (p->reach) {
            fmt_offset_us(off_buf, sizeof(off_buf), p->offset_us);
        } else {
            snprintf(off_buf, sizeof(off_buf), "---");
        }
        if (p->last_response_ms == UINT32_MAX) {
            snprintf(age_buf, sizeof(age_buf), "--");
        } else {
            fmt_duration(age_buf, sizeof(age_buf), p->last_response_ms / 1000);
        }

        // Format: "[*] addr... sN RR off AGE"
        snprintf(line, sizeof(line), "%c%-17s s%d %02x %-8s %3s",
                 p->selected ? '*' : ' ', addr,
                 p->stratum, p->reach, off_buf, age_buf);
    }

    char *cache = last_peer_row[slot];
    uint16_t prev_color = last_peer_color[slot];
    bool first_time = (cache[0] == '\0');
    bool color_changed = !first_time && prev_color != row_fg;

    if (!first_time && !color_changed && strcmp(cache, line) == 0) return;

    int x = (!p || !p->active) ? 10 : 4;
    diff_paint(x, y, cache, line, row_fg, COLOR_BLACK,
               first_time || color_changed);

    strncpy(cache, line, sizeof(last_peer_row[slot]) - 1);
    cache[sizeof(last_peer_row[slot]) - 1] = '\0';
    last_peer_color[slot] = row_fg;
}

static void refresh_dynamic(void) {
    ntp_sys_stats_t sys;
    ntp_get_sys_stats(&sys);
    const char *server = sys.server ? sys.server : "?";

    char val[96];

    // Row 0: Server
    {
        int y = SYS_Y_START;
        snprintf(val, sizeof(val), "%s", server);
        draw_field_cached(10, y, 0, "Server: ", val,
                          sys.synced ? COLOR_GREEN : COLOR_ORANGE);
    }

    // Row 1: Stratum / sync count / poll
    {
        int y = SYS_Y_START + SYS_LINE_H;
        char poll_buf[16], syncs_buf[16], strat_buf[16];
        fmt_duration(poll_buf, sizeof(poll_buf), sys.current_poll_s);
        snprintf(syncs_buf, sizeof(syncs_buf), "%lu", (unsigned long)sys.sync_count);
        if (sys.synced) {
            snprintf(strat_buf, sizeof(strat_buf), "%u", sys.stratum);
        } else {
            snprintf(strat_buf, sizeof(strat_buf), "unsynced");
        }
        segment_t segs[] = {
            { strat_buf,     COLOR_WHITE },
            { "   Syncs: ",  COLOR_GRAY  },
            { syncs_buf,     COLOR_WHITE },
            { "   Poll: ",   COLOR_GRAY  },
            { poll_buf,      COLOR_WHITE },
        };
        draw_segmented_field_cached(10, y, 1, "Stratum: ", segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    // Row 2: Offset / jitter
    {
        int y = SYS_Y_START + 2 * SYS_LINE_H;
        char off_buf[20], jit_buf[16];
        if (sys.sync_count < 2) {
            snprintf(off_buf, sizeof(off_buf), "---");
            snprintf(jit_buf, sizeof(jit_buf), "---");
        } else {
            fmt_offset_us(off_buf, sizeof(off_buf), sys.last_offset_us);
            fmt_offset_us(jit_buf, sizeof(jit_buf), sys.system_jitter_us);
        }
        segment_t segs[] = {
            { off_buf,       COLOR_WHITE },
            { "  Jitter: ",  COLOR_GRAY  },
            { jit_buf,       COLOR_WHITE },
        };
        draw_segmented_field_cached(10, y, 2, "Offset: ", segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    // Row 3: Drift (crystal frequency estimate)
    {
        int y = SYS_Y_START + 3 * SYS_LINE_H;
        if (sys.sync_count < 2) {
            snprintf(val, sizeof(val), "---");
        } else {
            char drift_buf[16];
            fmt_ppm_x1000(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000);
            snprintf(val, sizeof(val), "%s", drift_buf);
        }
        draw_field_cached(10, y, 3, "Drift: ", val, COLOR_WHITE);
    }

    // Row 4: Root delay / dispersion
    {
        int y = SYS_Y_START + 4 * SYS_LINE_H;
        if (sys.sync_count < 2 || !sys.synced) {
            snprintf(val, sizeof(val), "---");
        } else {
            char rd_buf[16], disp_buf[16];
            fmt_offset_us(rd_buf, sizeof(rd_buf), sys.root_delay_us);
            fmt_offset_us(disp_buf, sizeof(disp_buf), sys.root_dispersion_us);
            // Strip leading "+" from unsigned quantities
            const char *rd = rd_buf[0] == '+' ? rd_buf + 1 : rd_buf;
            const char *dp = disp_buf[0] == '+' ? disp_buf + 1 : disp_buf;
            snprintf(val, sizeof(val), "%s delay, %s disp", rd, dp);
        }
        draw_field_cached(10, y, 4, "Root: ", val, COLOR_WHITE);
    }

    // Peer rows
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_stats_t p;
        bool ok = ntp_get_peer_stats(i, &p);
        draw_peer_row(i, ok ? &p : NULL);
    }
}

void ui_ntp_stats_init(void) {
    ESP_LOGI(TAG, "Opening NTP stats");
    last_touch_time = 0;
    last_refresh_ms = 0;
    for (int i = 0; i < SYS_ROWS; i++) last_sys_row[i][0] = '\0';
    for (int i = 0; i < NTP_MAX_PEERS; i++) last_peer_row[i][0] = '\0';
    draw_static_chrome();
    refresh_dynamic();
}

ntp_stats_result_t ui_ntp_stats_update(void) {
    // BOOT button opens settings (same behavior as from the clock screen)
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return NTP_STATS_RESULT_SETTINGS;
    }
    touch_point_t touch;
    if (ui_read_touch(&touch, &last_touch_time)) {
        return NTP_STATS_RESULT_BACK;
    }

    // Refresh at 1 Hz
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    if (now_ms - last_refresh_ms >= 1000) {
        last_refresh_ms = now_ms;
        refresh_dynamic();
    }
    return NTP_STATS_RESULT_NONE;
}
