#ifndef CYD_BOARD_ESP32_2432S028_H
#define CYD_BOARD_ESP32_2432S028_H

#include "cyd_common.h"

// ESP32-2432S028R, the 2.8" CYD: ILI9341 panel, XPT2046 resistive touch on its
// own SPI bus. The default board.

#define BOARD_NAME "ESP32-2432S028R"

// Release asset this board OTA-updates from; must match the CI upload name.
#define BOARD_OTA_ASSET "cyd_clock.bin"

#define PIN_MISO  -1      // panel never reads
#define PIN_BL    21

#define BOARD_PANEL_BGR        1
#define BOARD_PANEL_MIRROR_X   0

// Touch (XPT2046) on its own SPI bus.
#define BOARD_TOUCH_SHARED_BUS 0
#define PIN_T_CLK   25
#define PIN_T_MOSI  32
#define PIN_T_MISO  39

// MIN/MAX_X bound the raw_x channel, MIN/MAX_Y the raw_y channel; SWAP_XY
// orients them to the screen.
#define BOARD_TOUCH_SWAP_XY    1
#define TOUCH_MIN_X         340
#define TOUCH_MAX_X         3900
#define TOUCH_MIN_Y         240
#define TOUCH_MAX_Y         3800

// 1PPS on the CN1 header (GND/IO22/IO27/3V3), active high.
#define PPS_OUT_PIN 27

#endif
