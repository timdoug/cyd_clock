#include "ui_clock.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "esp_log.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_clock";

// Layout constants
#define TIME_Y      20
#define DATE_Y      130
#define STATS_Y     155
#define STATS_LINE2 170
#define STATS_LINE3 185

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

static const char *day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *month_names[] = {
    "January", "February", "March", "April",
    "May", "June", "July", "August",
    "September", "October", "November", "December"
};


void ui_clock_init(void) {
    ESP_LOGI(TAG, "Initializing clock UI");
    last_hour = -1;
    last_min = -1;
    last_sec = -1;
    last_day = -1;
    last_synced_state = false;
    last_stats_sec = -1;
}

void ui_clock_redraw(void) {
    display_fill(COLOR_BLACK);
    last_hour = -1;
    last_min = -1;
    last_sec = -1;
    last_day = -1;
    last_synced_state = false;
    last_stats_sec = -1;
    ui_clock_update();
}

void ui_clock_set_synced(bool synced) {
    // Now handled via wifi_get_ntp_stats() - kept for API compatibility
    (void)synced;
}

static void draw_time_digit(int position, int digit) {
    // Calculate x position based on digit position
    // Format: HH:MM:SS
    // Positions: 0,1 = hours, 2,3 = minutes, 4,5 = seconds
    int x;
    int digit_width = 38;  // size 2 digit width
    int digit_spacing = 6; // space between digits
    int colon_width = 14;
    int total_width = 6 * digit_width + 5 * digit_spacing + 2 * colon_width;
    int start_x = (DISPLAY_WIDTH - total_width) / 2;
    int step = digit_width + digit_spacing;

    switch (position) {
        case 0: x = start_x; break;
        case 1: x = start_x + step; break;
        case 2: x = start_x + 2 * step + colon_width; break;
        case 3: x = start_x + 3 * step + colon_width; break;
        case 4: x = start_x + 4 * step + 2 * colon_width; break;
        case 5: x = start_x + 5 * step + 2 * colon_width; break;
        default: return;
    }

    display_digit_7seg(x, TIME_Y, digit, 2, COLOR_TIME_FG, COLOR_TIME_BG);
}

static void draw_colon(int position, bool visible) {
    int digit_width = 38;
    int digit_spacing = 6;
    int colon_width = 14;
    int total_width = 6 * digit_width + 5 * digit_spacing + 2 * colon_width;
    int start_x = (DISPLAY_WIDTH - total_width) / 2;
    int step = digit_width + digit_spacing;
    int x;

    if (position == 0) {
        x = start_x + 2 * step - digit_spacing / 2;
    } else {
        x = start_x + 4 * step + colon_width - digit_spacing / 2;
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

    int hour = timeinfo.tm_hour;
    int min = timeinfo.tm_min;
    int sec = timeinfo.tm_sec;

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

    // Blink colons every second
    bool new_colon_visible = (sec % 2 == 0);
    if (new_colon_visible != colon_visible || last_sec < 0) {
        draw_colon(0, new_colon_visible);
        draw_colon(1, new_colon_visible);
        colon_visible = new_colon_visible;
    }

    last_hour = hour;
    last_min = min;
    last_sec = sec;

    // Update date only when day changes
    if (timeinfo.tm_yday != last_day || last_day < 0) {
        char date_str[64];
        snprintf(date_str, sizeof(date_str), "%s, %s %d, %d",
                 day_names[timeinfo.tm_wday],
                 month_names[timeinfo.tm_mon],
                 timeinfo.tm_mday,
                 timeinfo.tm_year + 1900);

        // Center the date string
        int len = strlen(date_str);
        int x = (DISPLAY_WIDTH - len * 8) / 2;

        // Clear date area
        display_fill_rect(0, DATE_Y, DISPLAY_WIDTH, 20, COLOR_BLACK);
        display_string(x, DATE_Y, date_str, COLOR_DATE_FG, COLOR_BLACK);

        last_day = timeinfo.tm_yday;
    }

    // Update NTP stats every second
    ntp_stats_t stats;
    wifi_get_ntp_stats(&stats);

    // Line 1: Sync status with server
    if (stats.synced != last_synced_state || last_stats_sec < 0) {
        display_fill_rect(0, STATS_Y, DISPLAY_WIDTH, 14, COLOR_BLACK);
        if (stats.synced) {
            char status_str[48];
            snprintf(status_str, sizeof(status_str), "NTP: %s", stats.server);
            int x = (DISPLAY_WIDTH - strlen(status_str) * 8) / 2;
            display_string(x, STATS_Y, status_str, COLOR_SYNC_OK, COLOR_BLACK);
        } else {
            display_string(120, STATS_Y, "Syncing...", COLOR_SYNC_WAIT, COLOR_BLACK);
        }
        last_synced_state = stats.synced;
    }

    // Line 2 & 3: Update stats display every second
    if (sec != last_stats_sec) {
        last_stats_sec = sec;

        // Line 2: Last sync and sync count
        display_fill_rect(0, STATS_LINE2, DISPLAY_WIDTH, 14, COLOR_BLACK);
        if (stats.synced && stats.last_sync_time > 0) {
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
            int x = (DISPLAY_WIDTH - strlen(line2) * 8) / 2;
            display_string(x, STATS_LINE2, line2, COLOR_STATS, COLOR_BLACK);
        }

        // Line 3: Next sync countdown
        display_fill_rect(0, STATS_LINE3, DISPLAY_WIDTH, 14, COLOR_BLACK);
        if (stats.synced && stats.last_sync_time > 0) {
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
                int x = (DISPLAY_WIDTH - strlen(line3) * 8) / 2;
                display_string(x, STATS_LINE3, line3, COLOR_STATS, COLOR_BLACK);
            } else {
                display_string(110, STATS_LINE3, "Sync pending...", COLOR_SYNC_WAIT, COLOR_BLACK);
            }
        }
    }

}

clock_touch_zone_t ui_clock_check_touch(void) {
    touch_point_t point;
    if (!touch_read(&point)) {
        return CLOCK_TOUCH_NONE;
    }

    // Any touch opens settings
    return CLOCK_TOUCH_SETTINGS;
}
