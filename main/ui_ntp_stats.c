#include "ui_ntp_stats.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "chronyd_esp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "display.h"
#include "touch.h"
#include "ui_common.h"
#include "util.h"

// chronyd's reports as chronyc prints them, one command per page: tracking,
// sources, authdata. A tap shows the next page; past the last, back to the
// clock. chronyc's 80-column tables are folded into 40 columns: each source
// takes two rows, and long names/values scroll in place.

static const char *TAG = "ui_ntp_stats";

#define COLS          (DISPLAY_WIDTH / FONT_CHAR_WIDTH)   // 40
#define ROWS          13
#define ROW_Y0        (UI_HEADER_HEIGHT + 2)
#define MARQUEE_MS    250
// sources/authdata: two header rows, two rows per source, one row for
// "+N more"
#define SOURCE_ROWS   2
#define MAX_LISTED    ((ROWS - 2 - 1) / SOURCE_ROWS)

// Liveness indicator in the header - ticks every refresh so the user can tell
// the screen is active even if all displayed stats are steady.
#define SPINNER_PAD   ((UI_HEADER_HEIGHT - FONT_CHAR_HEIGHT) / 2)
#define SPINNER_X     (DISPLAY_WIDTH - FONT_CHAR_WIDTH - SPINNER_PAD)
#define SPINNER_Y     SPINNER_PAD

typedef enum { PAGE_TRACKING, PAGE_SOURCES, PAGE_AUTHDATA, PAGE_COUNT } page_t;

static const char *const page_titles[PAGE_COUNT] = {
    "chronyc tracking", "chronyc sources", "chronyc authdata",
};

static page_t      page;
static uint32_t    last_touch_time;
static time_t      last_refresh_sec;
static uint32_t    marquee_ms;
static uint8_t     spinner_frame;
static ces_status_t status;

// What is on the glass, per row, and the per-row marquee state (at most one
// scrolling field per row).
static char     row_text[ROWS][COLS + 1];
static uint16_t row_hash[ROWS];
static int      row_scroll[ROWS];
static int      row_dwell[ROWS];
static bool     row_advance;   // advance marquees during this render

typedef struct {
    char     text[COLS + 1];
    uint16_t colors[COLS];
} row_t;

// Row and report buffers are static: this screen runs on the main task,
// whose stack is 3.5 KB (CONFIG_ESP_MAIN_TASK_STACK_SIZE).

static void row_clear(row_t *r) {
    memset(r->text, ' ', COLS);
    r->text[COLS] = '\0';
    for (int i = 0; i < COLS; i++) r->colors[i] = COLOR_WHITE;
}

// Write s at col, clipped to width (whole string if width < 0).
static void row_put(row_t *r, int col, const char *s, int width, uint16_t color) {
    int n = (int)strlen(s);
    if (width >= 0 && n > width) n = width;
    for (int i = 0; i < n && col + i < COLS; i++) {
        r->text[col + i] = s[i];
        r->colors[col + i] = color;
    }
}

// Write s at col in a width-cell window that scrolls when s is wider.
static void row_put_scrolling(row_t *r, int row, int col, const char *s, int width,
                              uint16_t color) {
    if (row_advance) ui_marquee_advance(&row_scroll[row], &row_dwell[row], width, s);
    char win[COLS + 1];
    ui_marquee_window(win, width, s, row_scroll[row]);
    row_put(r, col, win, width, color);
}

static void row_paint(int row, const row_t *r) {
    uint16_t hash = 0;
    for (int i = 0; i < COLS; i++) hash = (uint16_t)(hash * 31u + r->colors[i]);
    bool first = row_text[row][0] == '\0';
    bool recolor = !first && hash != row_hash[row];
    if (!first && !recolor && strcmp(row_text[row], r->text) == 0) return;
    ui_diff_paint(0, ROW_Y0 + row * FONT_CHAR_HEIGHT, first ? "" : row_text[row],
                  r->text, 0, r->colors, COLOR_BLACK, first || recolor);
    str_copy(row_text[row], sizeof(row_text[row]), r->text);
    row_hash[row] = hash;
}

static uint16_t state_color(char state) {
    switch (state) {
    case '*': return COLOR_CYAN;
    case 'x': return COLOR_ORANGE;
    case '?': return COLOR_GRAY;
    default:  return COLOR_WHITE;
    }
}

static void render_tracking(row_t rows[ROWS]) {
    static char labels[CES_TRACKING_LINES][16], values[CES_TRACKING_LINES][64];
    ces_fmt_tracking(&status, labels, values);
    for (int i = 0; i < CES_TRACKING_LINES && i < ROWS; i++) {
        char label[20];
        snprintf(label, sizeof(label), "%-15.15s: ", labels[i]);
        row_put(&rows[i], 0, label, -1, COLOR_GRAY);
        bool unsynced = i == CES_TRACKING_LINES - 1 &&
                        status.leap_status == CES_LEAP_UNSYNCHRONISED;
        row_put_scrolling(&rows[i], i, 17, values[i], COLS - 17,
                          unsynced ? COLOR_ORANGE : COLOR_WHITE);
    }
}

static void render_more(row_t rows[ROWS], int row, int listed) {
    if (status.n_sources <= listed) return;
    char more[COLS + 1];
    snprintf(more, sizeof(more), "+%d more", status.n_sources - listed);
    row_put(&rows[row], 0, more, -1, COLOR_GRAY);
}

static void render_sources(row_t rows[ROWS]) {
    char line[COLS + 1];
    snprintf(line, sizeof(line), "%-2s %-16s %2s %4s %5s %6s",
             "MS", "Name/IP address", "St", "Poll", "Reach", "LastRx");
    row_put(&rows[0], 0, line, -1, COLOR_GRAY);
    row_put(&rows[1], 3, "Last sample", -1, COLOR_GRAY);

    int listed = 0;
    for (int i = 0; i < status.n_sources && listed < MAX_LISTED; i++, listed++) {
        const ces_source_t *s = &status.sources[i];
        int row = 2 + listed * SOURCE_ROWS;
        uint16_t color = state_color(s->state);
        char head[4], tail[COLS + 1], rx[8], sample[COLS + 1];

        snprintf(head, sizeof(head), "%c%c ", s->mode, s->state);
        row_put(&rows[row], 0, head, -1, color);
        row_put_scrolling(&rows[row], row, 3, s->addr, 16, color);
        ces_fmt_interval(rx, sizeof(rx), s->since_sample);
        snprintf(tail, sizeof(tail), " %2d %4d %5o %6s", s->stratum, s->poll, s->reach, rx);
        row_put(&rows[row], 19, tail, -1, color);

        ces_fmt_last_sample(sample, sizeof(sample), s);
        row_put(&rows[row + 1], 3, sample, -1, color);
    }
    render_more(rows, 2 + listed * SOURCE_ROWS, listed);
}

static void render_authdata(row_t rows[ROWS]) {
    char line[COLS + 1];
    snprintf(line, sizeof(line), "%-16s %4s %5s %4s %4s",
             "Name/IP address", "Mode", "KeyID", "Type", "KLen");
    row_put(&rows[0], 0, line, -1, COLOR_GRAY);
    snprintf(line, sizeof(line), "   %4s %4s %4s %4s %4s",
             "Last", "Atmp", "NAK", "Cook", "CLen");
    row_put(&rows[1], 0, line, -1, COLOR_GRAY);

    int listed = 0;
    for (int i = 0; i < status.n_sources && listed < MAX_LISTED; i++, listed++) {
        const ces_source_t *s = &status.sources[i];
        int row = 2 + listed * SOURCE_ROWS;
        char tail[COLS + 1], last[8];

        row_put_scrolling(&rows[row], row, 0, s->addr, 16, COLOR_WHITE);
        snprintf(tail, sizeof(tail), " %4s %5lu %4d %4d", s->auth_mode,
                 (unsigned long)s->key_id, s->key_type, s->key_length);
        row_put(&rows[row], 16, tail, -1, COLOR_WHITE);

        ces_fmt_interval(last, sizeof(last), s->last_ke_ago);
        snprintf(tail, sizeof(tail), "   %4s %4d %4d %4d %4d", last, s->ke_attempts,
                 s->nak, s->cookies, s->cookie_length);
        row_put(&rows[row + 1], 0, tail, -1, COLOR_WHITE);
    }
    render_more(rows, 2 + listed * SOURCE_ROWS, listed);
}

static void render(bool advance_marquee) {
    static row_t rows[ROWS];
    for (int i = 0; i < ROWS; i++) row_clear(&rows[i]);
    row_advance = advance_marquee;
    switch (page) {
    case PAGE_TRACKING: render_tracking(rows); break;
    case PAGE_SOURCES:  render_sources(rows);  break;
    default:            render_authdata(rows); break;
    }
    for (int i = 0; i < ROWS; i++) row_paint(i, &rows[i]);
}

static void refresh(void) {
    static const char spinner_chars[] = "|/-\\";
    display_char(SPINNER_X, SPINNER_Y, spinner_chars[spinner_frame],
                 COLOR_WHITE, UI_COLOR_HEADER);
    spinner_frame = (spinner_frame + 1) & 3;

    ces_get_status(&status);
    render(false);
}

static void show_page(page_t p) {
    page = p;
    display_fill(COLOR_BLACK);
    ui_draw_header(page_titles[page], false);
    for (int i = 0; i < ROWS; i++) {
        row_text[i][0] = '\0';
        row_scroll[i] = 0;
        row_dwell[i] = 0;
    }
    refresh();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    last_refresh_sec = tv.tv_sec;
}

void ui_ntp_stats_init(void) {
    ESP_LOGI(TAG, "Opening chronyc pages");
    last_touch_time = 0;
    spinner_frame = 0;
    show_page(PAGE_TRACKING);
}

ntp_stats_result_t ui_ntp_stats_update(void) {
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        return NTP_STATS_RESULT_SETTINGS;
    }
    touch_point_t touch;
    if (ui_read_touch(&touch, &last_touch_time)) {
        if (page + 1 >= PAGE_COUNT) return NTP_STATS_RESULT_BACK;
        show_page((page_t)(page + 1));
        ui_wait_for_touch_release();  // one tap, one page
        return NTP_STATS_RESULT_NONE;
    }

    // Refresh on each wall-clock second boundary, in step with the clock
    // screen's seconds.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec != last_refresh_sec) {
        last_refresh_sec = tv.tv_sec;
        refresh();
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(now - marquee_ms) >= MARQUEE_MS) {
        marquee_ms = now;
        render(true);
    }
    return NTP_STATS_RESULT_NONE;
}
