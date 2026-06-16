#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
} touch_point_t;

void touch_init(void);

bool touch_read(touch_point_t *point);

bool touch_is_pressed(void);

#endif
