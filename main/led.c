#include "led.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// CYD RGB LED pins (active low)
#define LED_PIN_R  4
#define LED_PIN_G  16
#define LED_PIN_B  17

void led_init(void) {
    // Turn off green and blue LEDs (active low, so high = off)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_G) | (1ULL << LED_PIN_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_G, 1);
    gpio_set_level(LED_PIN_B, 1);

    // Setup PWM for red LED (use channel 1, timer 0 is shared with backlight)
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_PIN_R,
        .duty = 256,  // Start fully off (active low, 256 = 100% high = off)
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);
}

void led_set_brightness(uint8_t brightness) {
    if (brightness == 0) {
        // Fully off - use max duty (256) for complete off with active low
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 256);
    } else {
        // Apply gamma correction then invert for active low
        uint8_t corrected = gamma_correct(brightness);
        uint8_t duty = 255 - corrected;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
