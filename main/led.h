#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include <stdint.h>

// Initialize the RGB LED, the PPS_OUT_PIN header output, and the 1PPS task.
// While enabled, both pulse 100 ms ON at every wall-clock second boundary.
void led_init(void);

// Gate the 1PPS generator on clock validity. While disabled (no NTP sync
// yet) neither the LED nor the header pin pulses.
void led_set_pps_enabled(bool enabled);

// Set the 1PPS LED brightness. 0 keeps the LED dark (the header pin still
// pulses); any other value is gamma-corrected and used as the LEDC duty
// during each pulse.
void led_set_brightness(uint8_t brightness);

#endif
