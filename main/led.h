#ifndef LED_H
#define LED_H

#include <stdint.h>

// Initialize the RGB LED (red channel PWM, green/blue off)
void led_init(void);

// Set LED brightness (0-255, 0 = off)
void led_set_brightness(uint8_t brightness);

#endif // LED_H
