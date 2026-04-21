#include "ui_clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "ntp.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_common.h"
#include "wifi.h"

static const char *TAG = "ui_clock";

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

static const char *day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void reset_display_state(void) {
    last_hour = -1;
    last_min = -1;
    last_sec = -1;
    last_day = -1;
    last_synced_state = false;
    last_stats_sec = -1;
    last_time_valid = false;
}

void ui_clock_init(void) {
    ESP_LOGI(TAG, "Initializing clock UI");
    reset_display_state();

    if (!nvs_config_get_led_brightness(&led_brightness)) {
        led_brightness = BRIGHTNESS_DEFAULT;
    }
    // Turn off LED initially
    led_set_brightness(0);
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

static void fmt_duration(char *buf, size_t len, uint32_t seconds) {
    if (seconds < 60) snprintf(buf, len, "%lus", (unsigned long)seconds);
    else if (seconds < 3600) snprintf(buf, len, "%lum", (unsigned long)(seconds / 60));
    else if (seconds < 86400) snprintf(buf, len, "%luh", (unsigned long)(seconds / 3600));
    else snprintf(buf, len, "%lud", (unsigned long)(seconds / 86400));
}

static void fmt_signed_fixed(char *buf, size_t len, int32_t val_x1000, const char *unit) {
    char sign = (val_x1000 < 0) ? '-' : '+';
    uint32_t av = val_x1000 < 0 ? (uint32_t)-val_x1000 : (uint32_t)val_x1000;
    snprintf(buf, len, "%c%lu.%02lu%s", sign,
             (unsigned long)(av / 1000),
             (unsigned long)((av % 1000) / 10),
             unit);
}

static void fmt_offset(char *buf, size_t len, int64_t us) {
    int64_t av = us < 0 ? -us : us;
    char sign = (us < 0) ? '-' : '+';
    if (av < 10000) {
        snprintf(buf, len, "%c%lldus", sign, (long long)av);
    } else if (av < 10000000LL) {
        snprintf(buf, len, "%c%lld.%02lldms", sign,
                 (long long)(av / 1000),
                 (long long)((av % 1000) / 10));
    } else {
        snprintf(buf, len, "%c%llds", sign, (long long)(av / 1000000));
    }
}

static void draw_ntp_stats(time_t now, int sec) {
    ntp_sys_stats_t sys;
    ntp_get_sys_stats(&sys);
    const char *server = sys.server ? sys.server : wifi_get_custom_ntp_server();

    // Line 1: server + stratum (green) or "Syncing..." (orange)
    if (sys.synced != last_synced_state || last_stats_sec < 0) {
        char line1[64];
        if (sys.synced) {
            snprintf(line1, sizeof(line1), "%s  str %d", server, sys.stratum);
            ui_draw_centered_string(STATS_Y, line1, COLOR_SYNC_OK, COLOR_BLACK, false);
        } else {
            snprintf(line1, sizeof(line1), "Syncing: %s", server);
            ui_draw_centered_string(STATS_Y, line1, COLOR_SYNC_WAIT, COLOR_BLACK, false);
        }
        last_synced_state = sys.synced;
    }

    if (sec == last_stats_sec) return;
    last_stats_sec = sec;

    if (!sys.synced) {
        char line2[48];
        snprintf(line2, sizeof(line2), "Waiting: %lus",
                 (unsigned long)(sys.sync_elapsed_ms / 1000));
        ui_draw_centered_string(STATS_LINE2, line2, COLOR_STATS, COLOR_BLACK, false);
        ui_draw_centered_string(STATS_LINE3, "", COLOR_BLACK, COLOR_BLACK, false);
        return;
    }

    // Line 2: peer reach + adaptive poll + time since last sync
    int peers_reach = 0, peers_total = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_stats_t p;
        if (!ntp_get_peer_stats(i, &p)) continue;
        peers_total++;
        if (p.reach) peers_reach++;
    }
    char poll_buf[16], ago_buf[16];
    fmt_duration(poll_buf, sizeof(poll_buf), sys.current_poll_s);
    fmt_duration(ago_buf, sizeof(ago_buf),
                 (uint32_t)(now > sys.last_sync_time ? now - sys.last_sync_time : 0));
    char line2[56];
    snprintf(line2, sizeof(line2), "%d/%d peers  poll %s  %s ago",
             peers_reach, peers_total, poll_buf, ago_buf);
    ui_draw_centered_string(STATS_LINE2, line2, COLOR_STATS, COLOR_BLACK, false);

    // Line 3: offset + drift (ppm) - meaningful only after second sync
    char line3[56];
    if (sys.sync_count < 2) {
        snprintf(line3, sizeof(line3), "offset ----  drift ----");
    } else {
        char off_buf[20], drift_buf[16];
        fmt_offset(off_buf, sizeof(off_buf), sys.last_offset_us);
        fmt_signed_fixed(drift_buf, sizeof(drift_buf), sys.freq_ppm_x1000, "ppm");
        snprintf(line3, sizeof(line3), "off %s  drift %s", off_buf, drift_buf);
    }
    ui_draw_centered_string(STATS_LINE3, line3, COLOR_STATS, COLOR_BLACK, false);
}

void ui_clock_update(void) {
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    // Check if time is valid (year >= 2025)
    bool time_valid = (timeinfo.tm_year + 1900 >= 2025);

    // If time just became valid, force redraw
    if (time_valid && !last_time_valid) {
        last_hour = -1;
        last_min = -1;
        last_sec = -1;
        last_day = -1;
    }
    last_time_valid = time_valid;

    int hour = timeinfo.tm_hour;
    int min = timeinfo.tm_min;
    int sec = timeinfo.tm_sec;

    if (time_valid) {
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

        // Blink colons every second (and sync LED)
        bool new_colon_visible = (sec % 2 == 0);
        if (new_colon_visible != colon_visible || last_sec < 0) {
            draw_colon(0, new_colon_visible);
            draw_colon(1, new_colon_visible);
            led_set_brightness(new_colon_visible ? led_brightness : 0);
            colon_visible = new_colon_visible;
        }

        last_hour = hour;
        last_min = min;
        last_sec = sec;

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
