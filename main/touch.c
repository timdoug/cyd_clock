#include "touch.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"

static const char *TAG = "touch";

// XPT2046 commands (PD bits 00: power down between conversions, PENIRQ on)
#define XPT2046_CMD_X   0xD0
#define XPT2046_CMD_Y   0x90
#define XPT2046_CMD_Z1  0xB0

// Minimum Z1 pressure for a read to count as a real touch. The PENIRQ line
// goes active on feather-light contact whose X/Y conversions are mostly
// noise; idle Z1 reads ~0-30, a deliberate finger press reads several
// hundred. Coordinates below this are garbage even when IRQ says pressed.
#define TOUCH_Z1_MIN    100


static spi_device_handle_t touch_spi;

void touch_init(void) {
    ESP_LOGI(TAG, "Initializing touch controller");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_T_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = TOUCH_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = PIN_T_CS,
        .queue_size = 1,
    };

#if BOARD_TOUCH_SHARED_BUS
    // Already on the display's SPI2 bus (set up by display_init()); just add
    // the device.
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &touch_spi));
#else
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_T_MOSI,
        .miso_io_num = PIN_T_MISO,
        .sclk_io_num = PIN_T_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &touch_spi));
#endif

    // Issue a dummy conversion with PD=00 to ensure PENIRQ is enabled.
    // If the controller was left in always-powered mode (PD=11), PENIRQ stays
    // disabled and soft-resets of the MCU don't clear it.
    uint8_t wake_tx[3] = {XPT2046_CMD_X, 0, 0};
    uint8_t wake_rx[3] = {0};
    spi_transaction_t wake_t = {
        .length = 24, .tx_buffer = wake_tx, .rx_buffer = wake_rx,
    };
    spi_device_polling_transmit(touch_spi, &wake_t);

    ESP_LOGI(TAG, "Touch controller initialized");
}

static uint16_t touch_read_channel(uint8_t cmd) {
    uint8_t tx_data[3] = {cmd, 0, 0};
    uint8_t rx_data[3] = {0};

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    spi_device_polling_transmit(touch_spi, &t);

    return ((rx_data[1] << 8) | rx_data[2]) >> 3;
}

bool touch_is_pressed(void) {
    return gpio_get_level(PIN_T_IRQ) == 0;
}

bool touch_read(touch_point_t *point) {
    if (!point) return false;
    if (!touch_is_pressed()) {
        point->pressed = false;
        return false;
    }

    if (touch_read_channel(XPT2046_CMD_Z1) < TOUCH_Z1_MIN) {
        point->pressed = false;
        return false;
    }

    // Take multiple samples; discard the min and max per axis and average
    // the middle two, so a single noisy conversion (common at the edges of
    // a press) cannot drag the mean.
    const int samples = 4;
    int32_t sum_x = 0, sum_y = 0;
    uint16_t min_x = UINT16_MAX, max_x = 0, min_y = UINT16_MAX, max_y = 0;

    for (int i = 0; i < samples; i++) {
        uint16_t x_s = touch_read_channel(XPT2046_CMD_X);
        uint16_t y_s = touch_read_channel(XPT2046_CMD_Y);
        sum_x += x_s;
        sum_y += y_s;
        if (x_s < min_x) min_x = x_s;
        if (x_s > max_x) max_x = x_s;
        if (y_s < min_y) min_y = y_s;
        if (y_s > max_y) max_y = y_s;
    }

    // Finger lifted mid-read: the trailing samples were of a fading press.
    if (!touch_is_pressed()) {
        point->pressed = false;
        return false;
    }

    uint16_t raw_x = (sum_x - min_x - max_x) / (samples - 2);
    uint16_t raw_y = (sum_y - min_y - max_y) / (samples - 2);

    // Map the raw channels onto screen axes; SWAP_XY picks which channel drives
    // which.
#if BOARD_TOUCH_SWAP_XY
    int32_t x = (int32_t)(raw_y - TOUCH_MIN_Y) * DISPLAY_WIDTH / (TOUCH_MAX_Y - TOUCH_MIN_Y);
    int32_t y = (int32_t)(raw_x - TOUCH_MIN_X) * DISPLAY_HEIGHT / (TOUCH_MAX_X - TOUCH_MIN_X);
#else
    int32_t x = (int32_t)(raw_x - TOUCH_MIN_X) * DISPLAY_WIDTH / (TOUCH_MAX_X - TOUCH_MIN_X);
    int32_t y = (int32_t)(raw_y - TOUCH_MIN_Y) * DISPLAY_HEIGHT / (TOUCH_MAX_Y - TOUCH_MIN_Y);
#endif

    if (x < 0) x = 0;
    if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;

    if (display_is_rotated()) {
        x = DISPLAY_WIDTH - 1 - x;
        y = DISPLAY_HEIGHT - 1 - y;
    }

    point->x = x;
    point->y = y;
    point->pressed = true;

    return true;
}
