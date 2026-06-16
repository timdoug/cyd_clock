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

// Format a number of seconds into a short human-readable string:
//   <60s    -> "Xs"
//   <3600   -> "Xm"
//   <86400  -> "Xh"
//   else    -> "Xd"
void ui_fmt_duration(char *buf, size_t len, uint32_t seconds);

// Compound form that always carries seconds granularity:
//   <60s    -> "Xs"
//   <3600   -> "Xm Ys"
//   <86400  -> "Xh Ym Zs"
//   else    -> "Xd Yh Zm"
// Worst case 11 chars ("23h 59m 59s").
void ui_fmt_duration_full(char *buf, size_t len, uint32_t seconds);

#endif
