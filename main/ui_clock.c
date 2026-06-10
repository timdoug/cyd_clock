#include "ui_clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "ntp.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_common.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_clock";

// Measured display-pipeline latency, owned and EMA-updated by main.c. Used
// here to pick the second that will be current when the pixels actually land.
extern volatile uint32_t clock_latency_us;

// Layout constants
#define TIME_Y      20
#define DATE_Y      116
#define STATS_Y     168
#define STATS_LINE2 188
#define STATS_LINE3 208

// 7-segment layout for time display (size 2)
#define TIME_DIGIT_WIDTH    38
#define TIME_DIGIT_SPACING  6
#define TIME_DIGIT_STEP     (TIME_DIGIT_WIDTH + TIME_DIGIT_SPACING)
#define TIME_TOTAL_WIDTH    (6 * TIME_DIGIT_WIDTH + 5 * TIME_DIGIT_SPACING + 2 * COLON_7SEG_WIDTH)
#define TIME_START_X        ((DISPLAY_WIDTH - TIME_TOTAL_WIDTH) / 2)

// Colors
#define COLOR_TIME_FG   COLOR_RED
#define COLOR_TIME_BG   COLOR_BLACK
#define COLOR_DATE_FG   COLOR_WHITE
#define COLOR_SYNC_OK   COLOR_GREEN
#define COLOR_SYNC_WAIT COLOR_ORANGE
#define COLOR_STATS     COLOR_GRAY

static int last_hour = -1;
static int last_min = -1;
static int last_sec = -1;
static int last_day = -1;
static bool colon_visible = true;
static bool last_synced_state = false;
static int last_stats_sec = -1;
static uint8_t led_brightness = BRIGHTNESS_DEFAULT;
static bool last_time_valid = false;
static uint8_t last_update_digits = 1;
static int64_t last_draw_end_us = 0;

// Cached rendered contents for each stats line - skip repaints when unchanged.
static char last_line1[80] = "";
static char last_line2[96] = "";
static char last_line3[96] = "";

static const char *day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static uint8_t digit_change_count(const struct tm *timeinfo) {
    if (!last_time_valid || last_hour < 0 || last_min < 0 || last_sec < 0) return 7;

    int hour = timeinfo->tm_hour;
    int min = timeinfo->tm_min;
    int sec = timeinfo->tm_sec;
    uint8_t changes = 0;
    if (hour / 10 != last_hour / 10) changes++;
    if (hour % 10 != last_hour % 10) changes++;
    if (min / 10 != last_min / 10) changes++;
    if (min % 10 != last_min % 10) changes++;
    if (sec / 10 != last_sec / 10) changes++;
    if (sec % 10 != last_sec % 10) changes++;
    if (timeinfo->tm_yday != last_day) changes = 7;
    if (changes == 0) changes = 1;
    return changes;
}

uint8_t ui_clock_last_update_digits(void) {
    return last_update_digits;
}

int64_t ui_clock_last_draw_end_us(void) {
    return last_draw_end_us;
}

uint8_t ui_clock_predict_next_update_digits(void) {
    if (!last_time_valid || last_hour < 0) return 7;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t next = tv.tv_sec + 1;
    struct tm timeinfo;
    localtime_r(&next, &timeinfo);
    return digit_change_count(&timeinfo);
}

static void reset_display_state(void) {
    last_hour = -1;
    last_min = -1;
    last_sec = -1;
    last_day = -1;
    last_synced_state = false;
    last_stats_sec = -1;
    last_time_valid = false;
    last_line1[0] = '\0';
    last_line2[0] = '\0';
    last_line3[0] = '\0';
}

void ui_clock_init(void) {
    ESP_LOGI(TAG, "Initializing clock UI");
    reset_display_state();

    if (!nvs_config_get_led_brightness(&led_brightness)) {
        led_brightness = BRIGHTNESS_DEFAULT;
    }
    // Don't touch the 1PPS pulse state here - it runs independently of the
    // clock screen so transitions into/out of this state shouldn't visibly
    // interrupt the pulse. reset_display_state() already cleared
    // last_time_valid, so the next ui_clock_update will re-assert the
    // current brightness via the "time just became valid" edge.
}

void ui_clock_redraw(void) {
    display_fill(COLOR_BLACK);
    reset_display_state();
    ui_clock_update();
}

static void draw_time_digit(int position, int digit) {
    // Calculate x position based on digit position
    // Format: HH:MM:SS
    // Positions: 0,1 = hours, 2,3 = minutes, 4,5 = seconds
    int x;

    switch (position) {
        case 0: x = TIME_START_X; break;
        case 1: x = TIME_START_X + TIME_DIGIT_STEP; break;
        case 2: x = TIME_START_X + 2 * TIME_DIGIT_STEP + COLON_7SEG_WIDTH; break;
        case 3: x = TIME_START_X + 3 * TIME_DIGIT_STEP + COLON_7SEG_WIDTH; break;
        case 4: x = TIME_START_X + 4 * TIME_DIGIT_STEP + 2 * COLON_7SEG_WIDTH; break;
        case 5: x = TIME_START_X + 5 * TIME_DIGIT_STEP + 2 * COLON_7SEG_WIDTH; break;
        default: return;
    }

    display_digit_7seg(x, TIME_Y, digit, 2, COLOR_TIME_FG, COLOR_TIME_BG);
}

static void draw_colon(int position, bool visible) {
    int x;

    if (position == 0) {
        x = TIME_START_X + 2 * TIME_DIGIT_STEP - TIME_DIGIT_SPACING / 2;
    } else {
        x = TIME_START_X + 4 * TIME_DIGIT_STEP + COLON_7SEG_WIDTH - TIME_DIGIT_SPACING / 2;
    }

    if (visible) {
        display_colon_7seg(x, TIME_Y, 2, COLOR_TIME_FG, COLOR_TIME_BG);
    } else {
        display_colon_7seg(x, TIME_Y, 2, COLOR_TIME_BG, COLOR_TIME_BG);
    }
}

// Drift in ppm with adaptive precision: 2 decimals under 10, 1 under 100, none above.
static void fmt_signed_fixed(char *buf, size_t len, int32_t val_x1000, const char *unit) {
    char sign = (val_x1000 < 0) ? '-' : '+';
    uint32_t av = val_x1000 < 0 ? (uint32_t)-val_x1000 : (uint32_t)val_x1000;
    if (av < 10000) {           // |x| < 10 ppm: "X.XX"
        uint32_t r = (av + 5) / 10;
        snprintf(buf, len, "%c%lu.%02lu%s", sign,
                 (unsigned long)(r / 100),
                 (unsigned long)(r % 100), unit);
    } else if (av < 100000) {   // 10 - 99 ppm: "XX.X"
        uint32_t r = (av + 50) / 100;
        snprintf(buf, len, "%c%lu.%lu%s", sign,
                 (unsigned long)(r / 10),
                 (unsigned long)(r % 10), unit);
    } else {                    // >= 100 ppm: "XXX"
        snprintf(buf, len, "%c%lu%s", sign,
                 (unsigned long)((av + 500) / 1000), unit);
    }
}

// Signed microseconds with adaptive precision. Always in ms (or s for large
// values); no "us" unit - sub-ms renders as "0.XXXms".
static void fmt_offset(char *buf, size_t len, int64_t us) {
    int64_t av = us < 0 ? -us : us;
    char sign = (us < 0) ? '-' : '+';
    if (av < 1000) {                    // "0.XXXms" - exact at us precision
        snprintf(buf, len, "%c0.%03lldms", sign, (long long)av);
    } else if (av < 10000) {            // "X.XXms" - round to 10us
        int64_t r = (av + 5) / 10;
        snprintf(buf, len, "%c%lld.%02lldms", sign,
                 (long long)(r / 100), (long long)(r % 100));
    } else if (av < 100000) {           // "XX.Xms" - round to 100us
        int64_t r = (av + 50) / 100;
        snprintf(buf, len, "%c%lld.%lldms", sign,
                 (long long)(r / 10), (long long)(r % 10));
    } else if (av < 10000000LL) {       // "XXXms" / "XXXXms" - round to 1ms
        snprintf(buf, len, "%c%lldms", sign, (long long)((av + 500) / 1000));
    } else if (av < 100000000LL) {      // "XX.Xs" - round to 100ms
        int64_t r = (av + 50000) / 100000;
        snprintf(buf, len, "%c%lld.%llds", sign,
                 (long long)(r / 10), (long long)(r % 10));
    } else {                            // "XXXs" - round to 1s
        snprintf(buf, len, "%c%llds", sign, (long long)((av + 500000) / 1000000));
    }
}

// Unsigned magnitude with "+/-" prefix - used for uncertainty bounds.
static void fmt_pm_us(char *buf, size_t len, int64_t us) {
    if (us < 0) us = -us;
    if (us < 1000) {                    // "0.XXXms" - exact at us precision
        snprintf(buf, len, "+/-0.%03lldms", (long long)us);
    } else if (us < 10000) {            // "X.Xms" - round to 100us
        int64_t r = (us + 50) / 100;
        snprintf(buf, len, "+/-%lld.%lldms",
                 (long long)(r / 10), (long long)(r % 10));
    } else if (us < 10000000LL) {       // "XXms" ... "XXXXms" - round to 1ms
        snprintf(buf, len, "+/-%lldms", (long long)((us + 500) / 1000));
    } else {                            // "XXs" ... - round to 1s
        snprintf(buf, len, "+/-%llds", (long long)((us + 500000) / 1000000));
    }
}

static void draw_line_cached(int y, char *cache, size_t cache_len,
                             const char *line, uint16_t fg) {
    if (strcmp(cache, line) == 0) return;

    size_t old_len = strlen(cache);
    size_t new_len = strlen(line);

    if (old_len == new_len && old_len > 0) {
        // Same centered position; redraw only the characters that differ.
        int x0 = (DISPLAY_WIDTH - (int)new_len * FONT_CHAR_WIDTH) / 2;
        for (size_t i = 0; i < new_len; i++) {
            if (cache[i] != line[i]) {
                display_char(x0 + (int)i * FONT_CHAR_WIDTH, y,
                             line[i], fg, COLOR_BLACK);
            }
        }
    } else {
        // Length change = every char shifts, so full redraw + padding reset.
        ui_draw_centered_string(y, line, fg, COLOR_BLACK, false);
    }

    str_copy(cache, cache_len, line);
}

static void draw_ntp_stats(time_t now, int sec) {
    ntp_sys_stats_t sys;
    ntp_peer_stats_t peers[NTP_MAX_PEERS];
    ntp_get_all_stats(&sys, peers);
    const char *server = sys.server[0] ? sys.server : wifi_get_custom_ntp_server();

    // Line 1: "Synced: <server>" (green) or "Syncing: <server>" (orange).
    // Green requires the CURRENT peer set to have disciplined the clock at
    // least once (sync_count > 0), not just first_sync_done: changing the
    // server (or a staleness re-resolve) resets sync accounting while the
    // clock keeps running on the old discipline, and an unreachable new
    // server used to sit there claiming "Synced" indefinitely.
    bool synced_here = sys.synced && sys.sync_count > 0;
    char line1[80];
    uint16_t line1_fg;
    if (synced_here) {
        snprintf(line1, sizeof(line1), "Synced: %s", server);
        line1_fg = COLOR_SYNC_OK;
    } else {
        snprintf(line1, sizeof(line1), "Syncing: %s", server);
        line1_fg = COLOR_SYNC_WAIT;
    }
    // Recolor forces a redraw when sync state flips (same text, different color)
    if (synced_here != last_synced_state) {
        last_line1[0] = '\0';
        last_synced_state = synced_here;
    }
    draw_line_cached(STATS_Y, last_line1, sizeof(last_line1), line1, line1_fg);

    if (sec == last_stats_sec) return;
    last_stats_sec = sec;

    if (!sys.synced) {
        char line2[48];
        snprintf(line2, sizeof(line2), "Waiting: %lus",
                 (unsigned long)(sys.sync_elapsed_ms / 1000));
        draw_line_cached(STATS_LINE2, last_line2, sizeof(last_line2), line2, COLOR_STATS);
        draw_line_cached(STATS_LINE3, last_line3, sizeof(last_line3), "", COLOR_BLACK);
        return;
    }

    // Line 2: peer reach + adaptive poll + time since last sync
    int peers_reach = 0, peers_total = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        peers_total++;
        if (peers[i].reach) peers_reach++;
    }
    char poll_buf[16], ago_buf[16];
    ui_fmt_duration_full(poll_buf, sizeof(poll_buf), sys.current_poll_s);
    char line2[80];
    if (sys.sync_count == 0) {
        // No discipline from the current peer set yet: there is no age to
        // show. Rendering now - last_sync_time(0) here used to display the
        // full Unix epoch as "20614d 15h 35m ago".
        snprintf(line2, sizeof(line2), "%d/%d peers  poll %s  no sync yet",
                 peers_reach, peers_total, poll_buf);
    } else {
        ui_fmt_duration_full(ago_buf, sizeof(ago_buf),
                     (uint32_t)(now > sys.last_sync_time ? now - sys.last_sync_time : 0));
        snprintf(line2, sizeof(line2), "%d/%d peers  poll %s  %s ago",
                 peers_reach, peers_total, poll_buf, ago_buf);
    }
    draw_line_cached(STATS_LINE2, last_line2, sizeof(last_line2), line2, COLOR_STATS);

    // Line 3: offset + root dispersion + drift. Gated independently - drift
    // can come from NVS (valid before any sync), offset/dispersion need a
    // current-session discipline (sync_count >= 2) to be meaningful.
    // Gates by what each value IS. The offset is a MEASUREMENT - the
    // residual found by a discipline pass - and the cold-boot step is not
    // one (last_offset_us is a hardwired 0 there), so it needs the second
    // sync. The +/- bound is real from the FIRST sync: it bounds the cold
    // step itself. Drift can predate both, restored from NVS.
    char line3[96];
    char off_buf[20], disp_buf[20], drift_buf[16];
    if (sys.sync_count < 2) {
        snprintf(off_buf,  sizeof(off_buf),  "----");
    } else {
        fmt_offset(off_buf, sizeof(off_buf), sys.last_offset_us);
    }
    if (sys.sync_count < 1) {
        snprintf(disp_buf, sizeof(disp_buf), "----");
    } else {
        // Synchronization distance (root_delay/2 + root_dispersion): the
        // RFC 5905 worst-case bound on our error vs UTC. Dispersion alone
        // understated the claim by the path-asymmetry term, delay/2 -
        // ~10-15 ms against pool servers. The stats drilldown still shows
        // the components separately.
        fmt_pm_us(disp_buf, sizeof(disp_buf),
                  (int64_t)sys.root_delay_us / 2 + sys.root_dispersion_us);
    }
    if (!sys.freq_known) {
        snprintf(drift_buf, sizeof(drift_buf), "----");
    } else {
        fmt_signed_fixed(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000, "ppm");
    }
    snprintf(line3, sizeof(line3), "off %s %s drift %s", off_buf, disp_buf, drift_buf);
    draw_line_cached(STATS_LINE3, last_line3, sizeof(last_line3), line3, COLOR_STATS);
}

void ui_clock_update(void) {
    time_t now;
    struct tm timeinfo;

    // Pick the second that will be current when the pixels actually land.
    // Two calling contexts:
    //   (a) Tick-timer fire: timer was armed for (boundary - clock_latency_us),
    //       so tv_usec is close to 1e6 here and we need to round up to the
    //       upcoming second so the digit appears in sync with the boundary.
    //   (b) ui_clock_redraw after returning from another screen: tv_usec can
    //       be at any phase. We want to show whatever second will be current
    //       when the freshly-drawn pixels land, not jump ahead prematurely.
    // (tv_usec + latency) >= 1e6 correctly captures both: it's true only when
    // rendering will actually cross the next boundary.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec + ((tv.tv_usec + (suseconds_t)clock_latency_us)
                       >= 1000000 ? 1 : 0);
    localtime_r(&now, &timeinfo);

    // Time is valid once NTP has set it: the clock cannot legitimately read
    // earlier than the anchor year (util.h).
    bool time_valid = (now >= (time_t)util_anchor_epoch());

    // If time just became valid, force redraw and enable the 1PPS pulse.
    if (time_valid && !last_time_valid) {
        last_hour = -1;
        last_min = -1;
        last_sec = -1;
        last_day = -1;
        led_set_brightness(led_brightness);
    }
    last_time_valid = time_valid;

    int hour = timeinfo.tm_hour;
    int min = timeinfo.tm_min;
    int sec = timeinfo.tm_sec;
    uint8_t update_digits = time_valid ? digit_change_count(&timeinfo) : 6;

    if (time_valid) {
        // Draw in increasing order of time-criticality. The latency EMA
        // centers the END of this block on the second boundary, so every
        // element lands early by however much drawing remains after it -
        // the LAST element drawn is the boundary-accurate one. That should
        // be the seconds digit (the pixel that carries the time), not the
        // cosmetic colon blink: colons first, then the once-a-day date,
        // then digits hours -> seconds.

        // Blink colons every second. LED is not toggled here anymore - the
        // red LED runs an independent 1PPS pulse driven by a dedicated timer
        // in led.c, so its edges align to the wall-clock boundary rather
        // than the display-latency-compensated tick that fires a few ms early.
        bool new_colon_visible = (sec % 2 == 0);
        if (new_colon_visible != colon_visible || last_sec < 0) {
            draw_colon(0, new_colon_visible);
            draw_colon(1, new_colon_visible);
            colon_visible = new_colon_visible;
        }

        // Update date only when day changes
        if (timeinfo.tm_yday != last_day || last_day < 0) {
            char date_str[32];
            snprintf(date_str, sizeof(date_str), "%s %s %d, %d",
                     day_names[timeinfo.tm_wday],
                     month_names[timeinfo.tm_mon],
                     timeinfo.tm_mday,
                     timeinfo.tm_year + 1900);

            ui_draw_centered_string(DATE_Y, date_str, COLOR_DATE_FG, COLOR_BLACK, true);
            last_day = timeinfo.tm_yday;
        }

        // Update time digits only when they change
        if (hour / 10 != last_hour / 10 || last_hour < 0) {
            draw_time_digit(0, hour / 10);
        }
        if (hour % 10 != last_hour % 10 || last_hour < 0) {
            draw_time_digit(1, hour % 10);
        }
        if (min / 10 != last_min / 10 || last_min < 0) {
            draw_time_digit(2, min / 10);
        }
        if (min % 10 != last_min % 10 || last_min < 0) {
            draw_time_digit(3, min % 10);
        }
        if (sec / 10 != last_sec / 10 || last_sec < 0) {
            draw_time_digit(4, sec / 10);
        }
        if (sec % 10 != last_sec % 10 || last_sec < 0) {
            draw_time_digit(5, sec % 10);
        }

        last_hour = hour;
        last_min = min;
        last_sec = sec;
    } else {
        // Time not valid - show dashes, no colons, LED off
        if (last_hour != -2) {
            for (int i = 0; i < 6; i++) {
                draw_time_digit(i, 10);  // 10 = dash
            }
            draw_colon(0, false);
            draw_colon(1, false);
            led_set_brightness(0);
            ui_draw_centered_string(DATE_Y, "Waiting for NTP...", COLOR_DATE_FG, COLOR_BLACK, true);
            last_hour = -2;  // Mark as showing dashes
        }
    }

    // Absolute stamp of "digit pixels are on the wire": main.c subtracts
    // the tick ISR's fire stamp to get the full boundary-to-pixels latency
    // (including the task wake hop) for the fire-early EMA. Stats drawing
    // below is deliberately outside the measured window - it happens after
    // the deadline-relevant pixels.
    last_draw_end_us = esp_timer_get_time();

    last_update_digits = update_digits;
    draw_ntp_stats(now, sec);
}

clock_touch_zone_t ui_clock_check_touch(void) {
    // BOOT button (active low) opens settings
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return CLOCK_TOUCH_SETTINGS;
    }
    // Touchscreen tap opens stats
    if (touch_is_pressed()) {
        return CLOCK_TOUCH_STATS;
    }
    return CLOCK_TOUCH_NONE;
}
