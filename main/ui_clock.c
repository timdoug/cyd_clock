#include "ui_clock.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "wifi.h"
#include "nvs_config.h"
#include "ui_common.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

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
    int total_width = 6 * TIME_DIGIT_WIDTH + 5 * TIME_DIGIT_SPACING + 2 * COLON_7SEG_WIDTH;
    int start_x = (DISPLAY_WIDTH - total_width) / 2;
    int step = TIME_DIGIT_WIDTH + TIME_DIGIT_SPACING;
    int x;

    switch (position) {
        case 0: x = start_x; break;
        case 1: x = start_x + step; break;
        case 2: x = start_x + 2 * step + COLON_7SEG_WIDTH; break;
        case 3: x = start_x + 3 * step + COLON_7SEG_WIDTH; break;
        case 4: x = start_x + 4 * step + 2 * COLON_7SEG_WIDTH; break;
        case 5: x = start_x + 5 * step + 2 * COLON_7SEG_WIDTH; break;
        default: return;
    }

    display_digit_7seg(x, TIME_Y, digit, 2, COLOR_TIME_FG, COLOR_TIME_BG);
}

static void draw_colon(int position, bool visible) {
    int total_width = 6 * TIME_DIGIT_WIDTH + 5 * TIME_DIGIT_SPACING + 2 * COLON_7SEG_WIDTH;
    int start_x = (DISPLAY_WIDTH - total_width) / 2;
    int step = TIME_DIGIT_WIDTH + TIME_DIGIT_SPACING;
    int x;

    if (position == 0) {
        x = start_x + 2 * step - TIME_DIGIT_SPACING / 2;
    } else {
        x = start_x + 4 * step + COLON_7SEG_WIDTH - TIME_DIGIT_SPACING / 2;
    }

    if (visible) {
        display_colon_7seg(x, TIME_Y, 2, COLOR_TIME_FG, COLOR_TIME_BG);
    } else {
        display_colon_7seg(x, TIME_Y, 2, COLOR_TIME_BG, COLOR_TIME_BG);
    }
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

    // Update NTP stats every second
    ntp_stats_t stats;
    wifi_get_ntp_stats(&stats);

    // Line 1: Sync status with server
    if (stats.synced != last_synced_state || last_stats_sec < 0) {
        char status_str[48];
        if (stats.synced) {
            snprintf(status_str, sizeof(status_str), "NTP: %s", stats.server);
            ui_draw_centered_string(STATS_Y, status_str, COLOR_SYNC_OK, COLOR_BLACK, false);
        } else {
            snprintf(status_str, sizeof(status_str), "Syncing: %s", wifi_get_custom_ntp_server());
            ui_draw_centered_string(STATS_Y, status_str, COLOR_SYNC_WAIT, COLOR_BLACK, false);
        }
        last_synced_state = stats.synced;
    }

    // Line 2 & 3: Update stats display every second
    if (sec != last_stats_sec) {
        last_stats_sec = sec;

        if (stats.synced) {
            // Line 2: Last sync and sync count
            if (stats.last_sync_time > 0) {
                time_t time_since = now - stats.last_sync_time;
                char line2[48];

                if (time_since < 60) {
                    snprintf(line2, sizeof(line2), "Last: %lds ago  Syncs: %lu",
                             (long)time_since, (unsigned long)stats.sync_count);
                } else if (time_since < 3600) {
                    snprintf(line2, sizeof(line2), "Last: %ldm ago  Syncs: %lu",
                             (long)(time_since / 60), (unsigned long)stats.sync_count);
                } else {
                    snprintf(line2, sizeof(line2), "Last: %ldh %ldm ago  Syncs: %lu",
                             (long)(time_since / 3600), (long)((time_since % 3600) / 60),
                             (unsigned long)stats.sync_count);
                }
                ui_draw_centered_string(STATS_LINE2, line2, COLOR_STATS, COLOR_BLACK, false);

                // Line 3: Next sync countdown
                time_t next_sync = stats.last_sync_time + stats.sync_interval;
                time_t until_next = next_sync - now;

                if (until_next > 0) {
                    char line3[48];
                    if (until_next < 60) {
                        snprintf(line3, sizeof(line3), "Next sync: %lds", (long)until_next);
                    } else {
                        snprintf(line3, sizeof(line3), "Next sync: %ldm %lds",
                                 (long)(until_next / 60), (long)(until_next % 60));
                    }
                    ui_draw_centered_string(STATS_LINE3, line3, COLOR_STATS, COLOR_BLACK, false);
                } else {
                    ui_draw_centered_string(STATS_LINE3, "Sync pending...", COLOR_SYNC_WAIT, COLOR_BLACK, false);
                }
            }
        } else {
            // Not synced - show elapsed time on line 2
            char line2[48];
            uint32_t elapsed_sec = stats.sync_elapsed_ms / 1000;
            snprintf(line2, sizeof(line2), "Waiting: %lus", (unsigned long)elapsed_sec);
            ui_draw_centered_string(STATS_LINE2, line2, COLOR_STATS, COLOR_BLACK, false);
            ui_draw_centered_string(STATS_LINE3, "", COLOR_BLACK, COLOR_BLACK, false);
        }
    }
}

clock_touch_zone_t ui_clock_check_touch(void) {
    // BOOT button (active low) opens settings
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return CLOCK_TOUCH_SETTINGS;
    }
    return CLOCK_TOUCH_NONE;
}
