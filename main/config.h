#ifndef CYD_CONFIG_H
#define CYD_CONFIG_H

#include <stdint.h>

// Display SPI pins (ILI9341 on ESP32-CYD). The panel's reset line is tied to
// the board EN signal, not a GPIO; init relies on SWRESET instead.
#define PIN_DC    2
#define PIN_CS    15
#define PIN_MOSI  13
#define PIN_CLK   14
#define PIN_BL    21

// Touch SPI pins (XPT2046 on ESP32-CYD)
#define PIN_T_CLK   25
#define PIN_T_MOSI  32
#define PIN_T_MISO  39
#define PIN_T_CS    33
#define PIN_T_IRQ   36

// RGB LED pins (active low)
#define LED_PIN_R  4
#define LED_PIN_G  16
#define LED_PIN_B  17

// Bare-GPIO 1PPS output on the CN1 header (GND/IO22/IO27/3V3), active high.
// A direct register write has no PWM period latch, so this edge is ~1 us
// from the local clock boundary vs up to ~50 us for the LED.
#define PPS_OUT_PIN 27

// Hardware configuration
#define SPI_CLOCK_HZ        (40 * 1000 * 1000)  // 40 MHz display SPI
#define TOUCH_SPI_CLOCK_HZ  (1 * 1000 * 1000)   // 1 MHz touch SPI
// Backlight + status LED PWM frequency (shared LEDC timer). 20 kHz keeps
// 8-bit duty resolution (LEDC ceiling at 8 bits is 312.5 kHz) while sitting
// above the audible range - 5 kHz was right at peak hearing sensitivity and
// could make the backlight supply sing. LEDC latches duty changes at the
// period boundary, so this also bounds the 1PPS LED edge jitter to one
// period: 50 us at 20 kHz vs 200 us at 5 kHz.
#define PWM_FREQUENCY_HZ    20000
#define BOOT_BUTTON_GPIO    0                    // BOOT button (active low)

// Touch calibration (hardware-specific, adjust for your display)
#define TOUCH_MIN_X         340
#define TOUCH_MAX_X         3900
#define TOUCH_MIN_Y         240
#define TOUCH_MAX_Y         3800

// UI layout constants
#define FONT_CHAR_WIDTH     8
#define FONT_CHAR_HEIGHT    16
#define FONT_CHAR_WIDTH_2X  16
#define FONT_CHAR_HEIGHT_2X 32

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

// Clock polling interval (ms) - upper bound on the display-tick semaphore
// wait so touch input stays responsive even if a tick is missed.
#define POLL_NORMAL_MS      20

// Display tick period for the clock screen: 100 Hz paints the hundredths
// field once per centisecond.
#define CLOCK_TICK_PERIOD_US 10000

// Forward bias applied when choosing the time a clock repaint displays.
// Two stacked delays separate a GRAM write from the value being read off
// the glass: the ILI9341 scans at ~70 Hz with no TE/VSYNC, so the write
// becomes visible 0..14 ms later (mean ~7 ms), and the painted value then
// persists a full tick, so a viewer samples it on average half a tick after
// it appears. 7 ms + 5 ms crosses one whole tick: every value is painted
// one tick early, which puts the visible flip ~2.8 ms early on average
// rather than ~7.2 ms late - as centered as the 10 ms paint quantization
// allows. (A bias under one tick would be eaten by truncation: ticks are
// phase-locked to centisecond boundaries, so it would never change the
// painted value.) main.c adds this when deciding which tick crosses a
// second; ui_clock.c adds it to the displayed time itself.
#define DISPLAY_SCAN_BIAS_US (7000 + CLOCK_TICK_PERIOD_US / 2)

// WiFi
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Gamma correction for perceptually linear brightness (quadratic approximation of gamma 2.2)
static inline uint8_t gamma_correct(uint8_t linear) {
    return (uint16_t)linear * linear / 255;
}

#endif // CYD_CONFIG_H
