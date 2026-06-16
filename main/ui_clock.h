#ifndef UI_CLOCK_H
#define UI_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCK_TOUCH_NONE,
    CLOCK_TOUCH_SETTINGS,
    CLOCK_TOUCH_STATS,
} clock_touch_zone_t;

void ui_clock_init(void);

void ui_clock_update(void);

// Rendering-cost class for the last/next clock update. Used by main.c to arm
// the second timer early enough for cheap one-digit ticks and expensive
// rollover ticks separately.
uint8_t ui_clock_last_update_digits(void);
uint8_t ui_clock_predict_next_update_digits(void);
int64_t ui_clock_last_draw_end_us(void);
bool ui_clock_last_draw_had_pixels(void);

void ui_clock_redraw(void);

clock_touch_zone_t ui_clock_check_touch(void);

#endif
