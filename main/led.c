#include "led.h"
#include <stdbool.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "config.h"

// Two-timer 1PPS generator on the red status LED. pps_on_timer fires at
// every wall-clock second boundary and programs the LEDC channel to the
// user-configured brightness; pps_off_timer fires 100 ms later and programs
// duty=off. Between pulses, duty is 0% (LED dark).
//
// The RISING edge is what matters for a 1PPS signal, and that's the edge we
// want precise. LEDC starts driving at the requested duty on the next PWM
// cycle boundary, so there's up to one PWM period (at 5 kHz, ~200 us) of
// jitter between our timer callback and the observable rising edge. That's
// way under the display pipeline's ms-scale error and still a very good
// 1PPS for a hobby setup. The falling edge lands ~100 ms later +/- another
// PWM period; pulse width is visibly constant but not sample-accurate, which
// is fine because pulse width isn't what 1PPS conveys.
//
// PWM lets us keep brightness control (gamma-corrected, active-low inverted
// just like display_set_backlight). brightness = 0 disables the pulse.
static esp_timer_handle_t pps_on_timer;
static esp_timer_handle_t pps_off_timer;
static volatile uint8_t   pps_brightness;   // user setting; 0 = PPS disabled

// LEDC 8-bit: 0 = always LOW (= LED fully ON with active-low wiring),
// 256 = always HIGH (= LED fully OFF). duty = 255 - gamma(brightness) gives
// a mostly-dark "fully off" state when brightness == 0, though we also
// explicitly disable the pulse in that case.
#define LED_DUTY_OFF 256

static void set_led_duty(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void pps_off_cb(void *arg) {
    (void)arg;
    set_led_duty(LED_DUTY_OFF);
}

static void pps_on_cb(void *arg) {
    (void)arg;
    uint8_t b = pps_brightness;
    if (b != 0) {
        uint8_t corrected = gamma_correct(b);
        set_led_duty(255 - corrected);
        esp_timer_start_once(pps_off_timer, 100 * 1000);   // 100 ms pulse
    }
    // Re-arm for the NEXT wall-clock second boundary. Reading gettimeofday
    // each cycle keeps us locked to the disciplined clock - any slew from
    // NTP moves the boundary with it, no drift accumulated across pulses.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t us_until = 1000000 - (uint32_t)tv.tv_usec;
    if (us_until < 1000) us_until += 1000000;
    esp_timer_start_once(pps_on_timer, us_until);
}

void led_init(void) {
    // Green/blue stay plain GPIO (we only need them as always-off indicators
    // on this board); red is driven by LEDC so we get gamma-corrected PWM
    // brightness while the 1PPS pulse is active.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_G) | (1ULL << LED_PIN_B),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_G, 1);
    gpio_set_level(LED_PIN_B, 1);

    // Red on LEDC channel 1; timer 0 is shared with the backlight (see
    // display.c), both running at PWM_FREQUENCY_HZ / 8-bit.
    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LED_PIN_R,
        .duty       = LED_DUTY_OFF,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    esp_timer_create_args_t on_args  = { .callback = pps_on_cb,  .name = "pps_on"  };
    esp_timer_create_args_t off_args = { .callback = pps_off_cb, .name = "pps_off" };
    esp_timer_create(&on_args,  &pps_on_timer);
    esp_timer_create(&off_args, &pps_off_timer);

    // Arm for the first boundary; the callback keeps re-arming itself.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t us_until = 1000000 - (uint32_t)tv.tv_usec;
    if (us_until < 1000) us_until += 1000000;
    esp_timer_start_once(pps_on_timer, us_until);
}

void led_set_brightness(uint8_t brightness) {
    pps_brightness = brightness;
    // Immediate response when the user zeros the slider - don't wait up to
    // 100 ms for the in-flight pulse to end.
    if (brightness == 0) {
        esp_timer_stop(pps_off_timer);
        set_led_duty(LED_DUTY_OFF);
    }
}
