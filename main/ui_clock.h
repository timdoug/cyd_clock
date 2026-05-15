#ifndef UI_CLOCK_H
#define UI_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Touch zone identifiers
typedef enum {
    CLOCK_TOUCH_NONE,
    CLOCK_TOUCH_SETTINGS,   // BOOT button
    CLOCK_TOUCH_STATS,      // Touchscreen tap
} clock_touch_zone_t;

// Initialize clock display
void ui_clock_init(void);

// Update clock display (call periodically)
void ui_clock_update(void);

// Rendering-cost class for the last/next clock update. Used by main.c to arm
// the second timer early enough for cheap one-digit ticks and expensive
// rollover ticks separately.
uint8_t ui_clock_last_update_digits(void);
uint8_t ui_clock_predict_next_update_digits(void);
uint32_t ui_clock_last_visible_latency_us(void);

// Force full redraw of clock
void ui_clock_redraw(void);

// Check for touch and return which zone was touched
clock_touch_zone_t ui_clock_check_touch(void);

#endif // UI_CLOCK_H
