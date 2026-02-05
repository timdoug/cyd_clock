#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

// Touch point structure
typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
} touch_point_t;

// Initialize the XPT2046 touch controller
void touch_init(void);

// Read current touch state (returns true if touched)
bool touch_read(touch_point_t *point);

// Check if screen is currently being touched (non-blocking)
bool touch_is_pressed(void);

#endif // TOUCH_H
