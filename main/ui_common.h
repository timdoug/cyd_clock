#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "touch.h"

#define UI_HEADER_HEIGHT    30
#define UI_ITEM_HEIGHT      26
#define UI_ITEM_MARGIN      10
#define UI_TEXT_Y_OFFSET    5
#define UI_HEADER_TEXT_Y    8

#define UI_BACK_BTN_X       5
#define UI_BACK_BTN_W       50

#define UI_LIST_ITEM_H      28
#define UI_LIST_START_Y     35
#define UI_LIST_VISIBLE     7
#define UI_LIST_SCROLL_REDRAW_MS 60
#define UI_MARQUEE_GAP      3
#define UI_MARQUEE_DWELL    6   // ticks to hold at each end before scrolling on

#define UI_COLOR_HEADER     0x001F
#define UI_COLOR_ITEM_BG    0x4208
#define UI_COLOR_ITEM_FG    0xFFFF
#define UI_COLOR_SELECTED   0x07FF

typedef enum {
    UI_LIST_TOUCH_NONE,
    UI_LIST_TOUCH_PRESSED,
    UI_LIST_TOUCH_SCROLLED,
    UI_LIST_TOUCH_TAPPED,
} ui_list_touch_result_t;

typedef struct {
    bool was_pressed;
    bool drag_tracking;
    bool drag_moved;
    bool redraw_pending;
    int drag_start_y;
    int drag_start_scroll;
    uint32_t last_redraw_ticks;
    touch_point_t tap_start;
} ui_list_touch_t;

#define VKEY_SHIFT      '\x01'
#define VKEY_MODE       '\x02'
#define VKEY_BACKSPACE  '\x08'
#define VKEY_ENTER      '\x0D'
#define VKEY_ESCAPE     '\x1B'

#define UI_SLIDER_BAR_X     100
#define UI_SLIDER_BAR_W     150
#define UI_SLIDER_BAR_H     14
#define UI_SLIDER_BTN_X1    260
#define UI_SLIDER_BTN_X2    288
#define UI_SLIDER_BTN_W     22
#define UI_SLIDER_BTN_H     18

void ui_draw_header(const char *title, bool show_back);

void ui_draw_menu_item(int y, const char *label);

void ui_draw_slider(int y, const char *label, uint8_t value, uint8_t max_value, uint16_t fill_color);
void ui_draw_slider_value(int y, uint8_t value, uint8_t max_value, uint16_t fill_color);
void ui_draw_slider_value_delta(int y, uint8_t old_value, uint8_t new_value,
                                uint8_t max_value, uint16_t fill_color);

void ui_draw_centered_string(int16_t y, const char *str, uint16_t fg, uint16_t bg, bool scale_2x);

// Fill out[0..width] with a width-char window of s for a horizontal marquee
// (space-padded if s fits). Returns the scroll period for `scroll % period`;
// a string that fits returns 1 (no scrolling).
int ui_marquee_window(char *out, int width, const char *s, int scroll);

// Advance a marquee one tick, holding *scroll for UI_MARQUEE_DWELL ticks each
// time the start is flush-left or the end is flush-right. *scroll and *dwell are
// caller-persisted state. Returns true when the position changed (redraw).
bool ui_marquee_advance(int *scroll, int *dwell, int width, const char *s);

void ui_draw_list(const char **labels, int count, int scroll_offset, int selected);

int ui_list_clamp_scroll(int scroll, int count);
int ui_list_scroll_to_item(int item, int count);
void ui_list_touch_reset(ui_list_touch_t *state);
ui_list_touch_result_t ui_list_touch_update(ui_list_touch_t *state,
                                            const touch_point_t *touch,
                                            bool pressed,
                                            int count,
                                            int *scroll_offset);

bool ui_back_button_hit(const touch_point_t *touch);

void ui_wait_for_touch_release(void);

bool ui_should_debounce(uint32_t last_time_ticks);

bool ui_read_touch(touch_point_t *touch, uint32_t *last_time_ticks);

// Signed microsecond quantity with adaptive precision, at most 7 chars for
// int32-range magnitudes: "+0.99ms", "+9.99ms", "+99.9ms", "+9999ms",
// "+99.9s", "+2147s". Shared by the clock stats line and the NTP stats
// rows, whose fixed-width columns rely on the 7-char cap.
void ui_fmt_offset_us(char *buf, size_t len, int64_t us);

// x1000 fixed-point value with unit suffix and adaptive decimals:
// "+9.99ppm", "+99.9ppm", "+500ppm".
void ui_fmt_signed_x1000(char *buf, size_t len, int32_t val_x1000, const char *unit);

// Format a number of seconds into a short human-readable string:
//   <60s    -> "Xs"
//   <3600   -> "Xm"
//   <86400  -> "Xh"
//   else    -> "Xd"
void ui_fmt_duration(char *buf, size_t len, uint32_t seconds);

// Compound two-unit form:
//   <60s    -> "Xs"
//   <3600   -> "Xm Ys"
//   <86400  -> "Xh Ym"
//   else    -> "Xd Yh"
// Worst case 8 chars ("365d 23h") for ages up to ~3 years. Capped at two
// units so the clock screen's peers/poll/ago line fits 40 columns in every
// language (the wordiest translations leave 10 columns for this field).
void ui_fmt_duration_full(char *buf, size_t len, uint32_t seconds);

#endif
