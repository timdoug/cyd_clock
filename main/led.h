#ifndef LED_H
#define LED_H

#include <stdint.h>

// Initialize the RGB LED and start the 1PPS pulse infrastructure. The red
// LED pulses 100 ms ON every wall-clock second boundary while enabled; the
// configured brightness sets the PWM duty within each pulse.
void led_init(void);

// Set the 1PPS pulse brightness. 0 disables the pulse entirely; any other
// value is gamma-corrected and used as the LEDC duty during each 100 ms
// pulse (between pulses the LED stays dark).
void led_set_brightness(uint8_t brightness);

#endif // LED_H
