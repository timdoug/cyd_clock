#include "touch.h"
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Pin definitions for XPT2046 on ESP32-CYD
#define PIN_T_CLK   25
#define PIN_T_MOSI  32
#define PIN_T_MISO  39
#define PIN_T_CS    33
#define PIN_T_IRQ   36

// XPT2046 commands
#define XPT2046_CMD_X  0xD0  // X position
#define XPT2046_CMD_Y  0x90  // Y position

// Calibration values (adjust for your specific display)
#define TOUCH_MIN_X  340
#define TOUCH_MAX_X  3900
#define TOUCH_MIN_Y  240
#define TOUCH_MAX_Y  3800

static spi_device_handle_t touch_spi;

void touch_init(void) {
    ESP_LOGI(TAG, "Initializing touch controller");

    // Configure IRQ pin as input
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_T_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Initialize SPI bus for touch (VSPI / SPI3)
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_T_MOSI,
        .miso_io_num = PIN_T_MISO,
        .sclk_io_num = PIN_T_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED));

    // Add touch device
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,  // 1 MHz
        .mode = 0,
        .spics_io_num = PIN_T_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &touch_spi));

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

    // 12-bit value from bits 1-12 of response
    return ((rx_data[1] << 8) | rx_data[2]) >> 3;
}

bool touch_is_pressed(void) {
    return gpio_get_level(PIN_T_IRQ) == 0;
}

bool touch_read(touch_point_t *point) {
    if (!touch_is_pressed()) {
        point->pressed = false;
        return false;
    }

    // Take multiple samples and average for stability
    int32_t sum_x = 0, sum_y = 0;
    const int samples = 4;

    for (int i = 0; i < samples; i++) {
        sum_x += touch_read_channel(XPT2046_CMD_X);
        sum_y += touch_read_channel(XPT2046_CMD_Y);
    }

    uint16_t raw_x = sum_x / samples;
    uint16_t raw_y = sum_y / samples;

    // Map to screen coordinates (X/Y swapped for landscape orientation)
    int32_t x = (int32_t)(raw_y - TOUCH_MIN_Y) * DISPLAY_WIDTH / (TOUCH_MAX_Y - TOUCH_MIN_Y);
    int32_t y = (int32_t)(raw_x - TOUCH_MIN_X) * DISPLAY_HEIGHT / (TOUCH_MAX_X - TOUCH_MIN_X);

    // Clamp to screen bounds
    if (x < 0) x = 0;
    if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;

    // Handle 180 degree rotation
    if (display_is_rotated()) {
        x = DISPLAY_WIDTH - 1 - x;
        y = DISPLAY_HEIGHT - 1 - y;
    }

    point->x = x;
    point->y = y;
    point->pressed = true;

    return true;
}
