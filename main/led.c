#include "led.h"
#include <stdbool.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"

static const char *TAG = "led";

// Two-timer 1PPS generator on the red status LED. pps_on_timer fires at
// every wall-clock second boundary and programs the LEDC channel to the
// user-configured brightness; pps_off_timer fires 100 ms later and programs
// duty=off. Between pulses, duty is 0% (LED dark).
//
// The RISING edge is what matters for a 1PPS signal, and that's the edge we
// want precise. Two error sources, two countermeasures:
//
//   * esp_timer dispatch latency: pps_on_cb runs in the esp_timer task on
//     core 0, which WiFi's internal timers share, so the callback starts a
//     fat-tailed 100-300+ us after the armed deadline. Countermeasure: arm
//     the wake EARLY by an EMA of the observed dispatch latency plus a
//     margin, then spin out the remainder on the lock-free monotonic clock
//     so the duty write lands on the boundary itself.
//   * LEDC duty latch: the new duty takes effect at the next PWM period
//     boundary, up to one period (at 20 kHz, ~50 us) after the write. Not
//     compensated; this is the residual error floor.
//
// The falling edge lands ~100 ms later +/- the same effects; pulse width is
// visibly constant but not sample-accurate, which is fine because pulse
// width isn't what 1PPS conveys.
//
// PWM lets us keep brightness control (gamma-corrected, active-low inverted
// just like display_set_backlight). brightness = 0 disables the pulse.
static esp_timer_handle_t pps_on_timer;
static esp_timer_handle_t pps_off_timer;
static volatile uint8_t   pps_brightness;   // user setting; 0 = PPS disabled

// How early to wake before the boundary: EMA of dispatch latency + margin.
// Normally everything below runs on the esp_timer task; the one exception
// is led_set_brightness's dead-chain recovery, which calls pps_arm_next
// from the main task. Safe without locking: esp_timer's API is thread-safe,
// start_once on an already-armed timer fails as a harmless no-op (so a
// recovery racing the callback's own re-arm converges to a single arm of
// the same boundary), and pps_wake_early_us is an aligned 32-bit read.
#define PPS_EARLY_MARGIN_US 150
#define PPS_EARLY_MIN_US    200
#define PPS_EARLY_MAX_US    1500
static uint32_t pps_wake_early_us = 400;

#define PPS_LED_PIN LED_PIN_R

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

// Arm pps_on_timer for (next boundary - pps_wake_early_us), reading
// gettimeofday fresh so NTP slews move the boundary with the disciplined
// clock - no drift accumulated across pulses. The < 1000 guard handles the
// pre-boundary wake position (and the disabled-pulse path): a target inside
// the next millisecond means we're aiming at the boundary we just serviced,
// so push to the following one.
static void pps_arm_next(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int32_t us_until = (int32_t)(1000000 - (uint32_t)tv.tv_usec) -
                       (int32_t)pps_wake_early_us;
    if (us_until < 1000) us_until += 1000000;
    esp_timer_start_once(pps_on_timer, (uint32_t)us_until);
}

static void pps_on_cb(void *arg) {
    (void)arg;
    uint8_t b = pps_brightness;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t usec = (uint32_t)tv.tv_usec;

    // Dispatch latency sample: how far past the armed target
    // (boundary - pps_wake_early_us) did this callback actually start?
    // usec in the pre-boundary window means we woke as planned; usec just
    // past the boundary means dispatch ate the whole margin. Anything else
    // means the wall clock moved underneath us (NTP step) - no sample.
    uint32_t target = 1000000 - pps_wake_early_us;
    uint32_t late   = UINT32_MAX;
    if (usec >= target)                  late = usec - target;
    else if (usec < PPS_EARLY_MAX_US)    late = usec + pps_wake_early_us;
    if (late != UINT32_MAX && late < 10000) {
        static uint32_t dispatch_ema_us = 250;
        dispatch_ema_us = dispatch_ema_us - dispatch_ema_us / 8 + late / 8;
        uint32_t early = dispatch_ema_us + PPS_EARLY_MARGIN_US;
        if (early < PPS_EARLY_MIN_US) early = PPS_EARLY_MIN_US;
        if (early > PPS_EARLY_MAX_US) early = PPS_EARLY_MAX_US;
        pps_wake_early_us = early;
    }

    if (b != 0) {
        // Spin out the remainder to the boundary on the monotonic clock
        // (lock-free, immune to a wall step mid-spin), bounded by the early
        // window, then write the duty. The rising edge then carries only
        // the LEDC period latch (~50 us at 20 kHz), not dispatch latency.
        if (usec >= 1000000 - PPS_EARLY_MAX_US) {
            int64_t boundary = esp_timer_get_time() + (int64_t)(1000000 - usec);
            while (esp_timer_get_time() < boundary) { }
        }
        uint8_t corrected = gamma_correct(b);
        set_led_duty(255 - corrected);
        esp_timer_start_once(pps_off_timer, 100 * 1000);   // 100 ms pulse
    }

    pps_arm_next();
}

void led_init(void) {
    // Green/blue stay plain GPIO, held off. Red is driven by LEDC so we get
    // gamma-corrected PWM brightness while the 1PPS pulse is active.
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
        .gpio_num   = PPS_LED_PIN,
        .duty       = LED_DUTY_OFF,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    esp_timer_create_args_t on_args  = { .callback = pps_on_cb,  .name = "pps_on"  };
    esp_timer_create_args_t off_args = { .callback = pps_off_cb, .name = "pps_off" };
    ESP_ERROR_CHECK(esp_timer_create(&on_args,  &pps_on_timer));
    ESP_ERROR_CHECK(esp_timer_create(&off_args, &pps_off_timer));

    // Arm for the first boundary; the callback keeps re-arming itself.
    pps_arm_next();
}

void led_set_brightness(uint8_t brightness) {
    pps_brightness = brightness;
    // Immediate response when the user zeros the slider - don't wait up to
    // 100 ms for the in-flight pulse to end.
    if (brightness == 0) {
        esp_timer_stop(pps_off_timer);
        set_led_duty(LED_DUTY_OFF);
    } else if (!esp_timer_is_active(pps_on_timer)) {
        ESP_LOGW(TAG, "PPS on-timer inactive while brightness=%u; re-arming",
                 (unsigned)brightness);
        set_led_duty(LED_DUTY_OFF);
        esp_timer_stop(pps_off_timer);
        pps_arm_next();
    }
}
