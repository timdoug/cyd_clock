#include "led.h"
#include <stdbool.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"

static const char *TAG = "led";

// 1PPS on the red LED and on the PPS_OUT_PIN header pin: rising edge on
// each second boundary of the NTP-disciplined clock, ~100 ms pulse, dark/low
// between. Only the rising edge needs accuracy. The generator runs whenever
// the clock is valid; LED brightness 0 darkens only the LED.
//
// A dedicated task re-derives each pulse from gettimeofday, so no state can
// go stale and any missed wake costs one beat at most. The predecessor - a
// self-re-arming chain of task-dispatch esp_timers - died permanently when
// esp_timer task dispatch stalled (timers stayed armed but never fired,
// which also defeated an is_active-based recovery check).
//
// Residual rising-edge error: the LEDC duty latches at the next PWM period,
// up to ~50 us at 20 kHz; everything software-side is ~1 us.

#define PPS_LED_PIN LED_PIN_R

#define PPS_PULSE_MS 100

// Coarse wake leads the boundary by this much; the task spins out the
// remainder, so the edge carries no semaphore wake jitter.
#define PPS_WAKE_EARLY_US 700

// Interrupts are masked for this final stretch of the spin so nothing can
// land between the spin exit and the duty latch trigger. Short enough that
// a delayed core-1 tick doesn't matter.
#define PPS_MASKED_SPIN_US 50

// Post-wake clock reread bounds: spin at most this far to the boundary...
#define PPS_SPIN_MAX_US 5000
// ...and fire at most this far past it; otherwise the wall clock moved
// underneath the sleep (NTP step), so skip the beat and reschedule.
#define PPS_LATE_FIRE_US 50000

// LEDC 8-bit, active-low: duty 256 = always HIGH = LED off.
#define LED_DUTY_OFF 256

static SemaphoreHandle_t  pps_wake_sem;
static esp_timer_handle_t pps_wake_timer;
static volatile bool      pps_enabled;      // clock valid; gates all pulses
static volatile uint8_t   pps_brightness;   // user setting; 0 = LED dark
static portMUX_TYPE       pps_spin_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_led_duty(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void IRAM_ATTR pps_wake_cb(void *arg) {
    (void)arg;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(pps_wake_sem, &hpw);
    if (hpw == pdTRUE) portYIELD_FROM_ISR();
}

// False on a missed wake; the caller recomputes from the clock either way.
static bool pps_sleep_us(uint32_t sleep_us) {
    esp_timer_stop(pps_wake_timer);
    xSemaphoreTake(pps_wake_sem, 0);             // drain a stale give
    esp_timer_start_once(pps_wake_timer, sleep_us);
    return xSemaphoreTake(pps_wake_sem,
                          pdMS_TO_TICKS(sleep_us / 1000 + 1000)) == pdTRUE;
}

static void pps_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!pps_enabled) {
            gpio_set_level(PPS_OUT_PIN, 0);
            set_led_duty(LED_DUTY_OFF);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        uint8_t b = pps_brightness;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint32_t us_to_boundary = 1000000 - (uint32_t)tv.tv_usec;
        if (us_to_boundary > PPS_WAKE_EARLY_US) {
            if (!pps_sleep_us(us_to_boundary - PPS_WAKE_EARLY_US)) {
                ESP_LOGW(TAG, "PPS wake missed; rescheduling");
                continue;
            }
        }

        // Reread the clock (NTP may have moved the boundary during the
        // sleep) and pin the edge to the monotonic clock, immune to a wall
        // step mid-spin. Sandwiching gettimeofday between two monotonic
        // reads removes the pairing skew.
        int64_t m0 = esp_timer_get_time();
        gettimeofday(&tv, NULL);
        int64_t mono = (m0 + esp_timer_get_time()) / 2;
        uint32_t usec = (uint32_t)tv.tv_usec;
        if (usec >= 1000000 - PPS_SPIN_MAX_US) {
            int64_t edge = mono + (int64_t)(1000000 - usec);
            // Pre-stage the duty so only the cheap latch trigger remains
            // after the spin.
            if (b) ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1,
                                 255 - gamma_correct(b));
            while (esp_timer_get_time() < edge - PPS_MASKED_SPIN_US) { }
            portENTER_CRITICAL(&pps_spin_lock);
            while (esp_timer_get_time() < edge) { }
            gpio_set_level(PPS_OUT_PIN, 1);   // the unlatched edge goes first
            // Re-check the live setting under the critical section (pps_task
            // and led_set_brightness both run on core 1, so this read and the
            // latch are atomic against a mid-pulse zeroing): if the user just
            // zeroed brightness, skip the duty latch so the LED stays dark now
            // instead of relighting for the rest of the pulse.
            if (b && pps_brightness) ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
            portEXIT_CRITICAL(&pps_spin_lock);
        } else if (usec >= PPS_LATE_FIRE_US) {
            continue;
        } else {
            gpio_set_level(PPS_OUT_PIN, 1);
            if (b && pps_brightness) set_led_duty(255 - gamma_correct(b));
        }

        // Falling edge needs no precision. Always write OFF, even if
        // brightness changed mid-pulse, so no path leaves the LED latched.
        vTaskDelay(pdMS_TO_TICKS(PPS_PULSE_MS));
        gpio_set_level(PPS_OUT_PIN, 0);
        set_led_duty(LED_DUTY_OFF);
    }
}

void led_init(void) {
    // Green/blue stay plain GPIO, held off. Red is driven by LEDC so we get
    // gamma-corrected PWM brightness while the 1PPS pulse is active.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_G) | (1ULL << LED_PIN_B) |
                        (1ULL << PPS_OUT_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_G, 1);
    gpio_set_level(LED_PIN_B, 1);
    gpio_set_level(PPS_OUT_PIN, 0);   // active high, idle low

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

    pps_wake_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(pps_wake_sem ? ESP_OK : ESP_ERR_NO_MEM);

    // ISR dispatch bypasses the esp_timer task (shared with WiFi's timers,
    // and observed to stall), like main.c's display tick.
    const esp_timer_create_args_t wake_args = {
        .callback        = pps_wake_cb,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "pps_wake",
    };
    ESP_ERROR_CHECK(esp_timer_create(&wake_args, &pps_wake_timer));

    // Core 1 with the UI task, which it must outrank so the pre-boundary
    // spin isn't preempted; core 0 has the WiFi/NTP latency tails.
    BaseType_t ok = xTaskCreatePinnedToCore(pps_task, "pps", 2560, NULL,
                                            10, NULL, 1);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void led_set_pps_enabled(bool enabled) {
    pps_enabled = enabled;
    if (!enabled) {
        gpio_set_level(PPS_OUT_PIN, 0);
        set_led_duty(LED_DUTY_OFF);
    }
}

void led_set_brightness(uint8_t brightness) {
    pps_brightness = brightness;
    // Immediate response when the user zeros the slider - don't wait up to
    // a pulse width for the task to notice. LED only; the header pin keeps
    // pulsing.
    if (brightness == 0) {
        set_led_duty(LED_DUTY_OFF);
    }
}
