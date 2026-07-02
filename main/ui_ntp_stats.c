#include "ui_ntp_stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "ntp.h"
#include "touch.h"
#include "i18n.h"
#include "ui_common.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_ntp_stats";

#define SYS_ROWS      5
#define SYS_Y_START   36
#define SYS_LINE_H    18
#define PEER_HDR_Y    (SYS_Y_START + SYS_ROWS * SYS_LINE_H)
#define PEER_Y_START  (PEER_HDR_Y + 18)
#define PEER_LINE_H   18
#define PEER_ADDR_W   15   // address column width; longer addrs scroll
#define PEER_MARQUEE_MS 250

// Liveness indicator in the header - ticks every refresh so the user can tell
// the screen is active even if all displayed stats are steady. Colors match
// the header title so it looks part of the chrome. Padded equally on top,
// bottom, and right edges of the blue bar.
#define SPINNER_PAD   ((UI_HEADER_HEIGHT - FONT_CHAR_HEIGHT) / 2)
#define SPINNER_X     (DISPLAY_WIDTH - FONT_CHAR_WIDTH - SPINNER_PAD)
#define SPINNER_Y     SPINNER_PAD

static uint32_t last_touch_time = 0;
static time_t   last_refresh_sec = 0;
static uint8_t  spinner_frame   = 0;

// Cached rendered line contents - skip repaint when unchanged, and when only
// a few chars change, update just those cells instead of blanking the row.
static char     last_sys_row[SYS_ROWS][96];
static uint16_t last_sys_color[SYS_ROWS];
static char     last_peer_row[NTP_MAX_PEERS][96];
static uint16_t last_peer_color[NTP_MAX_PEERS];

// Per-slot horizontal scroll for addresses wider than the column. slot_peer
// caches the last-drawn stats so the marquee can repaint a row between the
// 1 Hz refreshes without re-fetching.
static int              peer_addr_scroll[NTP_MAX_PEERS];
static int              peer_addr_dwell[NTP_MAX_PEERS];
static ntp_peer_stats_t slot_peer[NTP_MAX_PEERS];
static bool             slot_filled[NTP_MAX_PEERS];
static uint32_t         marquee_ms;

// Compact unsigned us -> string (fits in 5 chars). Used for per-peer delay and
// jitter where the sign is always non-negative and horizontal space is tight.
// Buckets are chosen after rounding so edge values stay within the cap
// (9999 us renders as "10ms", not "10.0ms").
static void fmt_unsigned_compact(char *buf, size_t len, uint32_t us) {
    uint32_t tenth_ms = (us + 50) / 100;
    if (tenth_ms < 100) {
        snprintf(buf, len, "%lu.%lums",
                 (unsigned long)(tenth_ms / 10), (unsigned long)(tenth_ms % 10));
        return;
    }
    uint32_t ms = (us + 500) / 1000;
    if (ms < 1000) {
        snprintf(buf, len, "%lums", (unsigned long)ms);
        return;
    }
    uint32_t tenth_s = (us + 50000) / 100000;
    if (tenth_s < 100) {
        snprintf(buf, len, "%lu.%lus",
                 (unsigned long)(tenth_s / 10), (unsigned long)(tenth_s % 10));
        return;
    }
    snprintf(buf, len, "%lus", (unsigned long)((us + 500000) / 1000000));
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
    ui_diff_paint(vx, y, cache, text, 0, colors, COLOR_BLACK, first_time);

    str_copy(cache, sizeof(last_sys_row[row_idx]), text);
    last_sys_color[row_idx] = 0;
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

    ui_diff_paint(vx, y, cache, value, val_color, NULL, COLOR_BLACK,
               first_time || color_changed);

    str_copy(cache, sizeof(last_sys_row[row_idx]), value);
    last_sys_color[row_idx] = val_color;
}

static void draw_static_chrome(void) {
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_NTP_STATS), false);
    // Column headers for the peer rows below - padding matches the peer row
    // format "%-15s %-2s %-7s %-5s %-5s%c". Selection marker ('*') is the
    // last char of each row so there's no leading-blank column to skip.
    display_fill_rect(0, PEER_HDR_Y, DISPLAY_WIDTH, FONT_CHAR_HEIGHT, COLOR_BLACK);
    display_string(4, PEER_HDR_Y, tr(STR_PEER_HEADER), COLOR_GRAY, COLOR_BLACK);
}

static void draw_peer_row(int slot, const ntp_peer_stats_t *p) {
    int y = PEER_Y_START + slot * PEER_LINE_H;

    char line[64];
    uint16_t row_fg;
    if (!p || !p->active) {
        snprintf(line, sizeof(line), "-");
        row_fg = COLOR_DARKGRAY;
    } else {
        row_fg = p->selected ? COLOR_CYAN
               : p->fresh    ? COLOR_GREEN
                             : COLOR_WHITE;
        char addr[PEER_ADDR_W + 1];
        ui_marquee_window(addr, PEER_ADDR_W, p->addr_str, peer_addr_scroll[slot]);

        char off_buf[10], delay_buf[8], jitter_buf[8], reach_buf[3];
        snprintf(reach_buf, sizeof(reach_buf), "%02x", p->reach);
        // A reachable peer with absurdly large jitter is the cold-boot
        // first-sync peer - its response was used for settimeofday but its
        // filter was deliberately left empty (pre-step t1 would have produced
        // a garbage sample). Show "---" until its next poll produces a real
        // measurement.
        bool has_sample = p->reach && p->jitter_us < 1000000;
        if (has_sample) {
            ui_fmt_offset_us(off_buf, sizeof(off_buf), p->offset_us);
            fmt_unsigned_compact(delay_buf,  sizeof(delay_buf),  (uint32_t)p->delay_us);
            fmt_unsigned_compact(jitter_buf, sizeof(jitter_buf), (uint32_t)p->jitter_us);
        } else {
            snprintf(off_buf,    sizeof(off_buf),    "---");
            snprintf(delay_buf,  sizeof(delay_buf),  "---");
            snprintf(jitter_buf, sizeof(jitter_buf), "---");
        }

        // Format: "addr(15) reach(2) offset(7) delay(5) jitter(5) [marker]"
        // The trailing status character mirrors the row color: '*' for the
        // selected peer (cyan), '!' for a recently-installed slot, ' '
        // otherwise. Age sits on the system-level Drift row (all peers share
        // a poll tick, so per-peer ages would be identical).
        char marker = p->selected ? '*' : (p->fresh ? '!' : ' ');
        snprintf(line, sizeof(line), "%-15s %-2s %-7s %-5s %-5s%c",
                 addr, reach_buf, off_buf, delay_buf, jitter_buf, marker);
    }

    char *cache = last_peer_row[slot];
    uint16_t prev_color = last_peer_color[slot];
    bool first_time = (cache[0] == '\0');
    bool color_changed = !first_time && prev_color != row_fg;

    if (!first_time && !color_changed && strcmp(cache, line) == 0) return;

    ui_diff_paint(4, y, cache, line, row_fg, NULL, COLOR_BLACK,
               first_time || color_changed);

    str_copy(cache, sizeof(last_peer_row[slot]), line);
    last_peer_color[slot] = row_fg;
}

static void refresh_dynamic(void) {
    // Tick the liveness spinner - redraw even if nothing else changes this
    // cycle, so the screen visibly "breathes" once per second.
    static const char spinner_chars[] = "|/-\\";
    display_char(SPINNER_X, SPINNER_Y, spinner_chars[spinner_frame],
                 COLOR_WHITE, UI_COLOR_HEADER);
    spinner_frame = (spinner_frame + 1) & 3;

    ntp_sys_stats_t sys;
    ntp_peer_stats_t peers[NTP_MAX_PEERS];
    ntp_get_all_stats(&sys, peers);
    // Same fallback as the clock screen: before the engine has a server
    // string, show the configured one rather than a placeholder.
    const char *server = sys.server[0] ? sys.server : wifi_get_custom_ntp_server();

    char val[96];

    {
        int y = SYS_Y_START;
        segment_t segs[] = {
            { " ", COLOR_GRAY },
            { server, sys.synced ? COLOR_GREEN : COLOR_ORANGE },
            { sys.nts_active ? " [NTS]" : "", COLOR_GREEN },
        };
        draw_segmented_field_cached(4, y, 0, tr(STR_SERVER_LABEL), segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    {
        int y = SYS_Y_START + SYS_LINE_H;
        char poll_buf[16], syncs_buf[16], strat_buf[16];
        ui_fmt_duration_full(poll_buf, sizeof(poll_buf), sys.current_poll_s);
        snprintf(syncs_buf, sizeof(syncs_buf), "%lu", (unsigned long)sys.sync_count);
        if (sys.synced) {
            snprintf(strat_buf, sizeof(strat_buf), "%u", sys.stratum);
        } else {
            snprintf(strat_buf, sizeof(strat_buf), "%s", tr(STR_UNSYNCED));
        }
        segment_t segs[] = {
            { strat_buf,    COLOR_WHITE },
            { tr(STR_POLL_LABEL),   COLOR_GRAY  },
            { poll_buf,     COLOR_WHITE },
            { tr(STR_SYNCS_LABEL),  COLOR_GRAY  },
            { syncs_buf,    COLOR_WHITE },
        };
        draw_segmented_field_cached(4, y, 1, tr(STR_STRATUM_LABEL), segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    // Row 2: Drift + time since last sync. Peers all align to a shared tick
    // so their individual ages match - show it once at the system level
    // rather than per-peer. Drift needs a disciplined sample to be
    // meaningful (sync_count >= 2); Age is valid as soon as the first sync
    // sets the clock (sync_count >= 1).
    {
        int y = SYS_Y_START + 2 * SYS_LINE_H;
        char drift_buf[16], age_buf[16];
        if (!sys.freq_known) {
            snprintf(drift_buf, sizeof(drift_buf), "---");
        } else {
            ui_fmt_signed_x1000(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000, "ppm");
        }
        if (sys.sync_count < 1) {
            snprintf(age_buf, sizeof(age_buf), "--");
        } else {
            time_t now_t;
            time(&now_t);
            uint32_t age_sec = (now_t > sys.last_sync_time)
                               ? (uint32_t)(now_t - sys.last_sync_time) : 0;
            ui_fmt_duration_full(age_buf, sizeof(age_buf), age_sec);
        }
        segment_t segs[] = {
            { drift_buf, COLOR_WHITE },
            { tr(STR_AGE_LABEL), COLOR_GRAY  },
            { age_buf,   COLOR_WHITE },
        };
        draw_segmented_field_cached(4, y, 2, tr(STR_DRIFT_LABEL), segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    // Row 3: Combined offset (what we slewed by) + combined jitter.
    // Both are system-level values - the selected peer's own offset/jitter
    // is already visible on its '*' row below, so no "Sel:" inline here.
    {
        int y = SYS_Y_START + 3 * SYS_LINE_H;
        char off_buf[20], jit_buf[16];
        if (sys.sync_count < 2) {
            snprintf(off_buf, sizeof(off_buf), "---");
            snprintf(jit_buf, sizeof(jit_buf), "---");
        } else {
            ui_fmt_offset_us(off_buf, sizeof(off_buf), sys.last_offset_us);
            ui_fmt_offset_us(jit_buf, sizeof(jit_buf), sys.system_jitter_us);
        }
        segment_t segs[] = {
            { off_buf,       COLOR_WHITE },
            { tr(STR_JITTER_LABEL),  COLOR_GRAY  },
            { jit_buf,       COLOR_WHITE },
        };
        draw_segmented_field_cached(4, y, 3, tr(STR_OFFSET_LABEL), segs,
                                    sizeof(segs) / sizeof(segs[0]));
    }

    {
        int y = SYS_Y_START + 4 * SYS_LINE_H;
        // Valid from the FIRST sync: root delay/dispersion come from the
        // selected peer's actual sample, unlike the measured offset/jitter
        // above which need a real discipline pass (sync_count >= 2).
        if (sys.sync_count < 1 || !sys.synced) {
            snprintf(val, sizeof(val), "---");
        } else {
            char rd_buf[16], disp_buf[16];
            ui_fmt_offset_us(rd_buf, sizeof(rd_buf), sys.root_delay_us);
            ui_fmt_offset_us(disp_buf, sizeof(disp_buf), sys.root_dispersion_us);
            const char *rd = rd_buf[0] == '+' ? rd_buf + 1 : rd_buf;
            const char *dp = disp_buf[0] == '+' ? disp_buf + 1 : disp_buf;
            snprintf(val, sizeof(val), tr(STR_FMT_ROOT_DETAIL), rd, dp);
        }
        draw_field_cached(4, y, 4, tr(STR_ROOT_LABEL), val, COLOR_WHITE);
    }

    // Peer rows - sorted by IP so a given peer always lives on the same row
    // for the user, regardless of which internal slot it occupies. Pure
    // strcmp on addr_str (not numerical IP sort, but stable and identical
    // across reboots when DNS gives back the same set). Inactive slots
    // render as "-" rows below the active peers.
    int active_idx[NTP_MAX_PEERS];
    int n_active = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        if (peers[i].active) {
            active_idx[n_active++] = i;
        }
    }
    for (int i = 1; i < n_active; i++) {
        int cur = active_idx[i];
        int j = i - 1;
        while (j >= 0 &&
               strcmp(peers[active_idx[j]].addr_str, peers[cur].addr_str) > 0) {
            active_idx[j + 1] = active_idx[j];
            j--;
        }
        active_idx[j + 1] = cur;
    }
    for (int slot = 0; slot < NTP_MAX_PEERS; slot++) {
        if (slot < n_active) {
            ntp_peer_stats_t *p = &peers[active_idx[slot]];
            // Restart the scroll when the peer occupying this slot changes.
            if (!slot_filled[slot] || strcmp(slot_peer[slot].addr_str, p->addr_str) != 0) {
                peer_addr_scroll[slot] = 0;
                peer_addr_dwell[slot] = 0;
            }
            slot_peer[slot] = *p;
            slot_filled[slot] = true;
            draw_peer_row(slot, p);
        } else {
            slot_filled[slot] = false;
            draw_peer_row(slot, NULL);
        }
    }
}

// Advance the address scroll for slots whose address is wider than the column,
// faster than the 1 Hz full refresh so it's readable. Repaints only those rows
// (ui_diff_paint touches just the address cells that moved).
static void marquee_peer_addrs(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(now - marquee_ms) < PEER_MARQUEE_MS) return;
    marquee_ms = now;
    for (int slot = 0; slot < NTP_MAX_PEERS; slot++) {
        if (!slot_filled[slot]) continue;
        if (ui_marquee_advance(&peer_addr_scroll[slot], &peer_addr_dwell[slot],
                               PEER_ADDR_W, slot_peer[slot].addr_str))
            draw_peer_row(slot, &slot_peer[slot]);
    }
}

void ui_ntp_stats_init(void) {
    ESP_LOGI(TAG, "Opening NTP stats");
    last_touch_time  = 0;
    spinner_frame    = 0;
    for (int i = 0; i < SYS_ROWS; i++) last_sys_row[i][0] = '\0';
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        last_peer_row[i][0] = '\0';
        peer_addr_scroll[i] = 0;
        peer_addr_dwell[i] = 0;
        slot_filled[i] = false;
    }
    draw_static_chrome();
    refresh_dynamic();
    // Mark the current second as already-painted so the next polled tick
    // doesn't double-refresh; subsequent updates only fire on a wall-clock
    // second rollover (see ui_ntp_stats_update).
    struct timeval tv;
    gettimeofday(&tv, NULL);
    last_refresh_sec = tv.tv_sec;
}

ntp_stats_result_t ui_ntp_stats_update(void) {
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return NTP_STATS_RESULT_SETTINGS;
    }
    touch_point_t touch;
    if (ui_read_touch(&touch, &last_touch_time)) {
        return NTP_STATS_RESULT_BACK;
    }

    // Refresh on each wall-clock second boundary so the displayed values
    // tick in lockstep with the clock screen (Age / spinner / etc. advance
    // at the same instant the seconds digit would on the time page).
    // Detection latency is bounded by the outer TOUCH_RELEASE_POLL_MS
    // (50 ms) - small enough to be invisible.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec != last_refresh_sec) {
        last_refresh_sec = tv.tv_sec;
        refresh_dynamic();
    }
    marquee_peer_addrs();
    return NTP_STATS_RESULT_NONE;
}
