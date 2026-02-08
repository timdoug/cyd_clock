#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// Hardware configuration
#define SPI_CLOCK_HZ        (40 * 1000 * 1000)  // 40 MHz display SPI
#define PWM_FREQUENCY_HZ    5000                 // Backlight PWM frequency
#define BOOT_BUTTON_GPIO    0                    // BOOT button (active low)

// Touch calibration (hardware-specific, adjust for your display)
#define TOUCH_MIN_X         340
#define TOUCH_MAX_X         3900
#define TOUCH_MIN_Y         240
#define TOUCH_MAX_Y         3800

// UI layout constants
#define CHAR_WIDTH          8
#define CHAR_HEIGHT         16
#define CHAR_WIDTH_2X       16
#define CHAR_HEIGHT_2X      32

// 7-segment digit dimensions (per size multiplier)
#define DIGIT_7SEG_WIDTH    19   // Base width, multiply by size
#define DIGIT_7SEG_HEIGHT   40   // Base height, multiply by size
#define DIGIT_7SEG_SPACING  6    // Space between digits
#define COLON_7SEG_WIDTH    14   // Colon width at size 2

// Settings UI
#define BRIGHTNESS_STEP     16
#define BRIGHTNESS_MIN      32
#define BRIGHTNESS_DEFAULT  255
#define BRIGHTNESS_MAX      255

// Touch debounce
#define TOUCH_DEBOUNCE_MS   200
#define TOUCH_RELEASE_POLL_MS 50

// Clock polling intervals (ms)
#define POLL_FAST_MS        2    // Near second boundary (>980ms)
#define POLL_MID_MS         10   // Approaching boundary (>900ms)
#define POLL_NORMAL_MS      20   // Normal polling
#define POLL_THRESHOLD_MS   980  // When to switch to fast polling

// NTP defaults
#define NTP_MIN_INTERVAL_SEC    15
#define NTP_DEFAULT_INTERVAL_SEC 86400  // 24 hours

// WiFi
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Gamma correction for perceptually linear brightness (quadratic approximation of gamma 2.2)
static inline uint8_t gamma_correct(uint8_t linear) {
    return (uint16_t)linear * linear / 255;
}

#endif // CONFIG_H
