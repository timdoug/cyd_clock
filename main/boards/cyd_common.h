#ifndef CYD_BOARD_COMMON_H
#define CYD_BOARD_COMMON_H

// Pins common to the supported CYD boards. A board profile includes this and
// adds what differs (backlight, MISO, panel flags, touch bus, calibration,
// 1PPS).

// Display SPI data lines (ILI9341). Reset is tied to the board EN line, not a
// GPIO, so init relies on SWRESET.
#define PIN_DC    2
#define PIN_CS    15
#define PIN_MOSI  13
#define PIN_CLK   14

// XPT2046 touch chip-select and PENIRQ (the touch data bus is per-board).
#define PIN_T_CS    33
#define PIN_T_IRQ   36

// RGB status LED.
#define LED_PIN_R  4
#define LED_PIN_G  16
#define LED_PIN_B  17

#endif
