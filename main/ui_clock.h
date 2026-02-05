#ifndef UI_CLOCK_H
#define UI_CLOCK_H

#include <stdbool.h>

// Touch zone identifiers
typedef enum {
    CLOCK_TOUCH_NONE,
    CLOCK_TOUCH_SETTINGS,
} clock_touch_zone_t;

// Initialize clock display
void ui_clock_init(void);

// Update clock display (call periodically)
void ui_clock_update(void);

// Force full redraw of clock
void ui_clock_redraw(void);

// Set sync status indicator
void ui_clock_set_synced(bool synced);

// Check for touch and return which zone was touched
clock_touch_zone_t ui_clock_check_touch(void);

#endif // UI_CLOCK_H
