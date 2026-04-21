#include "ui_ntp_stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "ntp.h"
#include "touch.h"
#include "ui_common.h"

static const char *TAG = "ui_ntp_stats";

#define SYS_Y_START   36
#define SYS_LINE_H    18
#define PEER_HDR_Y    (SYS_Y_START + 4 * SYS_LINE_H)
#define PEER_Y_START  (PEER_HDR_Y + 18)
#define PEER_LINE_H   18

static uint32_t last_touch_time = 0;
static uint32_t last_refresh_ms = 0;

static void fmt_duration(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60) snprintf(buf, len, "%lus", (unsigned long)seconds);
    else if (seconds < 3600) snprintf(buf, len, "%lum", (unsigned long)(seconds / 60));
    else if (seconds < 86400) snprintf(buf, len, "%luh", (unsigned long)(seconds / 3600));
    else snprintf(buf, len, "%lud", (unsigned long)(seconds / 86400));
}

static void fmt_offset_us(char *buf, size_t len, int64_t us) {
    int64_t av = us < 0 ? -us : us;
    char sign = (us < 0) ? '-' : '+';
    if (av < 10000) {
        snprintf(buf, len, "%c%lldus", sign, (long long)av);
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

// Draw a "label: value" line at (x, y). Clears the value area to the right.
static void draw_field(int x, int y, const char *label, const char *value,
                       uint16_t val_color) {
    display_string(x, y, label, COLOR_GRAY, COLOR_BLACK);
    int vx = x + (int)strlen(label) * FONT_CHAR_WIDTH;
    // Clear area to the right
    display_fill_rect(vx, y, DISPLAY_WIDTH - vx, FONT_CHAR_HEIGHT, COLOR_BLACK);
    display_string(vx, y, value, val_color, COLOR_BLACK);
}

static void draw_static_chrome(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header("NTP Stats", true);
    // Peer section divider label
    display_fill_rect(0, PEER_HDR_Y, DISPLAY_WIDTH, FONT_CHAR_HEIGHT, COLOR_BLACK);
    ui_draw_centered_string(PEER_HDR_Y, "--- Peers ---", COLOR_GRAY, COLOR_BLACK, false);
}

static void draw_peer_row(int slot, const ntp_peer_stats_t *p) {
    int y = PEER_Y_START + slot * PEER_LINE_H;
    display_fill_rect(0, y, DISPLAY_WIDTH, FONT_CHAR_HEIGHT, COLOR_BLACK);

    if (!p || !p->active) {
        display_string(10, y, "-", COLOR_DARKGRAY, COLOR_BLACK);
        return;
    }

    uint16_t row_fg = p->selected ? COLOR_CYAN : COLOR_WHITE;
    char line[64];
    // Truncate address to 17 chars to leave room for metrics
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
             p->selected ? '*' : ' ',
             addr,
             p->stratum,
             p->reach,
             off_buf,
             age_buf);
    display_string(4, y, line, row_fg, COLOR_BLACK);
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
        draw_field(10, y, "Server: ", val,
                   sys.synced ? COLOR_GREEN : COLOR_ORANGE);
    }

    // Row 1: Stratum / sync count / poll
    {
        int y = SYS_Y_START + SYS_LINE_H;
        char poll_buf[16];
        fmt_duration(poll_buf, sizeof(poll_buf), sys.current_poll_s);
        if (sys.synced) {
            snprintf(val, sizeof(val), "%u   syncs %lu   poll %s",
                     sys.stratum, (unsigned long)sys.sync_count, poll_buf);
        } else {
            snprintf(val, sizeof(val), "unsynced   syncs %lu   poll %s",
                     (unsigned long)sys.sync_count, poll_buf);
        }
        draw_field(10, y, "Stratum: ", val, COLOR_WHITE);
    }

    // Row 2: Offset / jitter
    {
        int y = SYS_Y_START + 2 * SYS_LINE_H;
        char off_buf[20];
        if (sys.sync_count < 2) {
            snprintf(val, sizeof(val), "---       jitter ---       drift ---");
        } else {
            char jit_buf[16], drift_buf[16];
            fmt_offset_us(off_buf, sizeof(off_buf), sys.last_offset_us);
            fmt_offset_us(jit_buf, sizeof(jit_buf), sys.system_jitter_us);
            fmt_ppm_x1000(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000);
            snprintf(val, sizeof(val), "%s  jit %s  drift %s",
                     off_buf, jit_buf, drift_buf);
        }
        draw_field(10, y, "Offset: ", val, COLOR_WHITE);
    }

    // Row 3: Root delay / dispersion
    {
        int y = SYS_Y_START + 3 * SYS_LINE_H;
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
        draw_field(10, y, "Root: ", val, COLOR_WHITE);
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
    draw_static_chrome();
    refresh_dynamic();
}

ntp_stats_result_t ui_ntp_stats_update(void) {
    touch_point_t touch;
    bool touched = ui_read_touch(&touch, &last_touch_time);
    if (touched && touch.y < UI_HEADER_HEIGHT &&
        touch.x < UI_BACK_BTN_X + UI_BACK_BTN_W) {
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
