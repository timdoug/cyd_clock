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
#include "i18n.h"
#include "ui_common.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ui_clock";

// Measured display-pipeline latency, owned and EMA-updated by main.c. Used
// here to pick the second that will be current when the pixels actually land.
extern volatile uint32_t clock_latency_us;

#define TIME_Y      20
#define FRACTION_Y  98
#define DATE_Y      124
#define STATS_Y     168
#define STATS_LINE2 188
#define STATS_LINE3 208

#define TIME_DIGIT_WIDTH    38
#define TIME_DIGIT_SPACING  6
#define TIME_DIGIT_STEP     (TIME_DIGIT_WIDTH + TIME_DIGIT_SPACING)
#define TIME_TOTAL_WIDTH    (6 * TIME_DIGIT_WIDTH + 5 * TIME_DIGIT_SPACING + 2 * COLON_7SEG_WIDTH)
#define TIME_START_X        ((DISPLAY_WIDTH - TIME_TOTAL_WIDTH) / 2)
#define FRACTION_WIDTH      (3 * FONT_CHAR_WIDTH)
#define FRACTION_X          (TIME_START_X + 5 * TIME_DIGIT_STEP + 2 * COLON_7SEG_WIDTH + TIME_DIGIT_WIDTH - FRACTION_WIDTH)

// ASCII stats lines are drawn on a fixed 40-column character grid (see
// draw_line_cached). Tokenized rows use pixel-width centering instead.
#define STATS_COLS (DISPLAY_WIDTH / FONT_CHAR_WIDTH)
#define STATS_CACHE_BYTES 128

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
static int last_centisecond = -1;
static uint8_t led_brightness = BRIGHTNESS_DEFAULT;
static bool last_time_valid = false;
static uint8_t last_update_digits = 1;
static int64_t last_draw_end_us = 0;
static bool last_draw_had_pixels = false;

static char last_line1[STATS_CACHE_BYTES] = "";
static char last_line2[STATS_CACHE_BYTES] = "";
static char last_line3[STATS_CACHE_BYTES] = "";
static char last_fraction[4] = "";

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
    return changes;
}

uint8_t ui_clock_last_update_digits(void) {
    return last_update_digits;
}

int64_t ui_clock_last_draw_end_us(void) {
    return last_draw_end_us;
}

bool ui_clock_last_draw_had_pixels(void) {
    return last_draw_had_pixels;
}

// May return 0: with the forward display bias the upcoming second is painted
// by the .99 tick, so the boundary tick that follows sees no digit changes
// and main.c gives it the fraction-only latency bucket.
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
    last_centisecond = -1;
    last_synced_state = false;
    last_stats_sec = -1;
    last_time_valid = false;
    last_line1[0] = '\0';
    last_line2[0] = '\0';
    last_line3[0] = '\0';
    last_fraction[0] = '\0';
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

    display_colon_7seg(x, TIME_Y, 2, visible ? COLOR_TIME_FG : COLOR_TIME_BG);
}

static bool draw_fraction(int centisecond) {
    if (centisecond < 0) centisecond = 0;
    if (centisecond > 99) centisecond = 99;

    char fraction[4];
    snprintf(fraction, sizeof(fraction), ".%02d", centisecond);
    if (strcmp(last_fraction, fraction) == 0) return false;

    if (last_fraction[0] == '\0') {
        display_string(FRACTION_X, FRACTION_Y, fraction, COLOR_TIME_FG, COLOR_TIME_BG);
    } else {
        for (int i = 0; i < 3; i++) {
            if (last_fraction[i] != fraction[i]) {
                display_char(FRACTION_X + i * FONT_CHAR_WIDTH, FRACTION_Y,
                             fraction[i], COLOR_TIME_FG, COLOR_TIME_BG);
            }
        }
    }
    str_copy(last_fraction, sizeof(last_fraction), fraction);
    return true;
}

static bool clear_fraction(void) {
    if (last_fraction[0] == '\0') return false;
    display_fill_rect(FRACTION_X, FRACTION_Y, FRACTION_WIDTH, FONT_CHAR_HEIGHT, COLOR_TIME_BG);
    last_fraction[0] = '\0';
    last_centisecond = -1;
    return true;
}

static void fmt_pm_us(char *buf, size_t len, int64_t us) {
    // ui_fmt_offset_us with the sign replaced by "+/-".
    char t[16];
    ui_fmt_offset_us(t, sizeof(t), us < 0 ? -us : us);
    snprintf(buf, len, "+/-%s", t + 1);
}

static bool clock_text_has_glyph_tokens(const char *s) {
    while (*s) {
        if ((unsigned char)*s == (unsigned char)DISPLAY_GLYPH_ESCAPE && s[1] != '\0') {
            return true;
        }
        s++;
    }
    return false;
}

// ASCII draw path: the line is centered into a fixed 40-column field (space
// padded, clipped if a long custom server name overflows it) and diffed
// cell-by-cell against what is on the glass. Tokenized strings cannot use
// this byte-grid path because their two-byte glyph tokens must stay intact;
// those rows are repainted as pixel-centered strings.
static void draw_line_cached(int y, char *cache, size_t cache_len,
                             const char *line, uint16_t fg, bool force) {
    bool line_tokens = clock_text_has_glyph_tokens(line);
    if (line_tokens || clock_text_has_glyph_tokens(cache)) {
        bool blank = (cache[0] == '\0');
        if (!blank && !force && strcmp(cache, line) == 0) return;

        int old_width = blank ? 0 : display_text_width(cache);
        int old_x = (DISPLAY_WIDTH - old_width) / 2;
        if (old_x < 0) old_x = 0;

        if (!line_tokens) {
            // The row lost its tokens: erase the pixel-centered rendering
            // and rebuild below with an empty cache, so the grid path never
            // diffs against a raw token-path cache (the two cache formats
            // store different strings for the same glass contents).
            if (!blank) {
                display_fill_rect(old_x, y, old_width, FONT_CHAR_HEIGHT, COLOR_BLACK);
            }
            cache[0] = '\0';
        } else {
            int width = display_text_width(line);
            int x = (DISPLAY_WIDTH - width) / 2;
            if (x < 0) x = 0;

            if (blank) {
                display_string(x, y, line, fg, COLOR_BLACK);
            } else if (x == old_x) {
                ui_diff_paint(x, y, cache, line, fg, NULL, COLOR_BLACK, force);
            } else {
                display_fill_rect(old_x, y, old_width, FONT_CHAR_HEIGHT, COLOR_BLACK);
                display_string(x, y, line, fg, COLOR_BLACK);
            }

            str_copy(cache, cache_len, line);
            return;
        }
    }

    char field[STATS_COLS + 1];
    size_t len = strlen(line);
    if (len > STATS_COLS) len = STATS_COLS;
    size_t pad = (STATS_COLS - len) / 2;
    memset(field, ' ', STATS_COLS);
    memcpy(field + pad, line, len);
    field[STATS_COLS] = '\0';

    bool blank = (cache[0] == '\0');
    for (size_t i = 0; i < STATS_COLS; i++) {
        char prev = blank ? ' ' : cache[i];
        if (prev != field[i] || (force && field[i] != ' ')) {
            display_char((int)i * FONT_CHAR_WIDTH, y, field[i], fg, COLOR_BLACK);
        }
    }

    str_copy(cache, cache_len, field);
}

static void draw_ntp_stats(time_t now, int sec) {
    ntp_sys_stats_t sys;
    ntp_peer_stats_t peers[NTP_MAX_PEERS];
    ntp_get_all_stats(&sys, peers);
    const char *server = sys.server[0] ? sys.server : wifi_get_custom_ntp_server();

    // Line 1 reports clock validity, not whether the current peer set has
    // completed its first wave. After changing servers the clock is still
    // synced; line 2 carries the "no sync yet" detail for the new peer set.
    bool synced_here = sys.synced;
    char line1[80];
    uint16_t line1_fg;
    if (synced_here) {
        snprintf(line1, sizeof(line1), tr(STR_FMT_SYNCED), server);
        line1_fg = COLOR_SYNC_OK;
    } else {
        snprintf(line1, sizeof(line1), tr(STR_FMT_SYNCING), server);
        line1_fg = COLOR_SYNC_WAIT;
    }
    // A sync-state flip recolors the line even where the text is unchanged.
    bool line1_force = (synced_here != last_synced_state);
    last_synced_state = synced_here;
    draw_line_cached(STATS_Y, last_line1, sizeof(last_line1), line1, line1_fg,
                     line1_force);

    if (sec == last_stats_sec) return;
    last_stats_sec = sec;

    if (!sys.synced) {
        char line2[48];
        snprintf(line2, sizeof(line2), tr(STR_FMT_WAITING),
                 (unsigned long)(sys.sync_elapsed_ms / 1000));
        draw_line_cached(STATS_LINE2, last_line2, sizeof(last_line2), line2,
                         COLOR_STATS, false);
        draw_line_cached(STATS_LINE3, last_line3, sizeof(last_line3), "",
                         COLOR_BLACK, false);
        return;
    }

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
        snprintf(line2, sizeof(line2), tr(STR_FMT_PEERS_NOSYNC),
                 peers_reach, peers_total, poll_buf);
    } else {
        ui_fmt_duration_full(ago_buf, sizeof(ago_buf),
                     (uint32_t)(now > sys.last_sync_time ? now - sys.last_sync_time : 0));
        snprintf(line2, sizeof(line2), tr(STR_FMT_PEERS_AGO),
                 peers_reach, peers_total, poll_buf, ago_buf);
    }
    draw_line_cached(STATS_LINE2, last_line2, sizeof(last_line2), line2,
                     COLOR_STATS, false);

    char line3[96];
    char off_buf[20], disp_buf[20], drift_buf[16];
    if (sys.sync_count < 2) {
        snprintf(off_buf,  sizeof(off_buf),  "----");
    } else {
        ui_fmt_offset_us(off_buf, sizeof(off_buf), sys.last_offset_us);
    }
    if (sys.sync_count < 1) {
        snprintf(disp_buf, sizeof(disp_buf), "----");
    } else {
        fmt_pm_us(disp_buf, sizeof(disp_buf),
                  (int64_t)sys.root_delay_us / 2 + sys.root_dispersion_us);
    }
    if (!sys.freq_known) {
        snprintf(drift_buf, sizeof(drift_buf), "----");
    } else {
        ui_fmt_signed_x1000(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000, "ppm");
    }
    snprintf(line3, sizeof(line3), tr(STR_FMT_OFF_DRIFT), off_buf, disp_buf, drift_buf);
    draw_line_cached(STATS_LINE3, last_line3, sizeof(last_line3), line3,
                     COLOR_STATS, false);
}

void ui_clock_update(void) {
    time_t now;
    struct tm timeinfo;

    // Pick the time that should be current when these pixels are likely seen.
    // Two calling contexts:
    //   (a) Tick-timer fire: timer was armed for a 10 ms display tick minus
    //       the estimated render latency, so add that latency back before
    //       formatting HH:MM:SS.xx.
    //   (b) ui_clock_redraw after returning from another screen: tv_usec can
    //       be at any phase. We still want to show the time that should be
    //       current after the freshly-drawn pixels land.
    // DISPLAY_SCAN_BIAS_US (config.h) then pre-advances the result past one
    // whole tick so each value's VISIBLE flip - after panel scanout - centers
    // on its true boundary; see the definition for the math.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t display_us = (int64_t)tv.tv_sec * 1000000LL +
                         (int64_t)tv.tv_usec +
                         (int64_t)clock_latency_us +
                         DISPLAY_SCAN_BIAS_US;
    now = (time_t)(display_us / 1000000LL);
    int centisecond = (int)((display_us % 1000000LL) / 10000LL);
    localtime_r(&now, &timeinfo);

    // Time is valid once NTP has set it: the clock cannot legitimately read
    // earlier than the anchor year (util.h).
    bool time_valid = (now >= (time_t)util_anchor_epoch());

    if (time_valid && !last_time_valid) {
        last_hour = -1;
        last_min = -1;
        last_sec = -1;
        last_day = -1;
        led_set_brightness(led_brightness);
        led_set_pps_enabled(true);
    }
    last_time_valid = time_valid;

    int hour = timeinfo.tm_hour;
    int min = timeinfo.tm_min;
    int sec = timeinfo.tm_sec;
    uint8_t update_digits = time_valid ? digit_change_count(&timeinfo) : 6;
    bool drew_pixels = false;

    if (time_valid) {
        // Draw in increasing order of time-criticality. The latency EMA
        // centers the END of this block on the tick boundary, so the LAST
        // element drawn is closest to the predicted visible time. Colons and
        // date are cosmetic/slow-changing, then HH:MM:SS, then hundredths.

        // Blink colons every second. LED is not toggled here anymore - the
        // red LED runs an independent 1PPS pulse driven by a dedicated task
        // in led.c, so its edges align to the wall-clock boundary rather
        // than the display-latency-compensated tick that fires a few ms early.
        bool new_colon_visible = (sec % 2 == 0);
        if (new_colon_visible != colon_visible || last_sec < 0) {
            draw_colon(0, new_colon_visible);
            draw_colon(1, new_colon_visible);
            colon_visible = new_colon_visible;
            drew_pixels = true;
        }

        if (timeinfo.tm_yday != last_day || last_day < 0) {
            char date_str[32];
            tr_date(date_str, sizeof(date_str),
                    timeinfo.tm_wday, timeinfo.tm_mon,
                    timeinfo.tm_mday, timeinfo.tm_year + 1900);

            ui_draw_centered_string(DATE_Y, date_str, COLOR_DATE_FG, COLOR_BLACK, true);
            last_day = timeinfo.tm_yday;
            drew_pixels = true;
        }

        if (hour / 10 != last_hour / 10 || last_hour < 0) {
            draw_time_digit(0, hour / 10);
            drew_pixels = true;
        }
        if (hour % 10 != last_hour % 10 || last_hour < 0) {
            draw_time_digit(1, hour % 10);
            drew_pixels = true;
        }
        if (min / 10 != last_min / 10 || last_min < 0) {
            draw_time_digit(2, min / 10);
            drew_pixels = true;
        }
        if (min % 10 != last_min % 10 || last_min < 0) {
            draw_time_digit(3, min % 10);
            drew_pixels = true;
        }
        if (sec / 10 != last_sec / 10 || last_sec < 0) {
            draw_time_digit(4, sec / 10);
            drew_pixels = true;
        }
        if (sec % 10 != last_sec % 10 || last_sec < 0) {
            draw_time_digit(5, sec % 10);
            drew_pixels = true;
        }

        last_hour = hour;
        last_min = min;
        last_sec = sec;

        if (centisecond != last_centisecond || last_centisecond < 0) {
            drew_pixels |= draw_fraction(centisecond);
            last_centisecond = centisecond;
        }
    } else {
        if (last_hour != -2) {
            for (int i = 0; i < 6; i++) {
                draw_time_digit(i, 10);
            }
            draw_colon(0, false);
            draw_colon(1, false);
            drew_pixels = true;
            clear_fraction();
            led_set_pps_enabled(false);
            ui_draw_centered_string(DATE_Y, tr(STR_WAITING_NTP), COLOR_DATE_FG, COLOR_BLACK, true);
            last_hour = -2;
        }
    }

    // Absolute stamp of "time pixels are on the wire": main.c subtracts
    // the tick ISR's fire stamp to get the full boundary-to-pixels latency
    // (including the task wake hop) for the fire-early EMA. Stats drawing
    // below is deliberately outside the measured window - it happens after
    // the deadline-relevant pixels.
    last_draw_end_us = esp_timer_get_time();

    last_update_digits = update_digits;
    last_draw_had_pixels = drew_pixels;
    if (sec != last_stats_sec || last_line1[0] == '\0') {
        draw_ntp_stats(now, sec);
    }
}

clock_touch_zone_t ui_clock_check_touch(void) {
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return CLOCK_TOUCH_SETTINGS;
    }
    // Go through touch_read (not the bare PENIRQ level) so the TOUCH_Z1_MIN
    // pressure gate applies here too - a feather-light/condensation touch must
    // not oscillate the clock in and out of stats.
    touch_point_t touch;
    if (touch_read(&touch)) {
        return CLOCK_TOUCH_STATS;
    }
    return CLOCK_TOUCH_NONE;
}
