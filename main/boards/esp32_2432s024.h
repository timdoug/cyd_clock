#ifndef CYD_BOARD_ESP32_2432S024_H
#define CYD_BOARD_ESP32_2432S024_H

#include "cyd_common.h"

// ESP32-2432S024R, the 2.4" CYD. Same ILI9341 panel as the 2.8" board, but the
// backlight is on GPIO27, the panel scans RGB and mirrored, and the XPT2046
// shares the display's SPI bus.

#define BOARD_NAME "ESP32-2432S024R"

// Release asset this board OTA-updates from; must match the CI upload name.
#define BOARD_OTA_ASSET "cyd_clock-esp32_2432s024.bin"

#define PIN_MISO  12      // touch shares this bus, so MISO must be live
#define PIN_BL    27

#define BOARD_PANEL_BGR        0
#define BOARD_PANEL_MIRROR_X   1

// Touch (XPT2046) shares the display's SPI2 bus; only CS and PENIRQ are its own.
#define BOARD_TOUCH_SHARED_BUS 1

#define BOARD_TOUCH_SWAP_XY    0
#define TOUCH_MIN_X         325
#define TOUCH_MAX_X         3760
#define TOUCH_MIN_Y         280
#define TOUCH_MAX_Y         3820

// GPIO27 is the backlight here, so 1PPS moves to the adjacent IO22.
#define PPS_OUT_PIN 22

#endif
