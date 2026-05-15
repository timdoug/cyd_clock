#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_about.h"
#include "ui_clock.h"
#include "ui_common.h"
#include "ui_ntp.h"
#include "ui_ntp_stats.h"
#include "ui_settings.h"
#include "ui_timezone.h"
#include "ui_wifi_setup.h"
#include "wifi.h"

static const char *TAG = "main";

// Application states
typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_SETUP,
    APP_STATE_CONNECTING,
    APP_STATE_CLOCK,
    APP_STATE_SETTINGS,
    APP_STATE_TIMEZONE,
    APP_STATE_ABOUT,
    APP_STATE_NTP,
    APP_STATE_NTP_STATS,
} app_state_t;

static app_state_t app_state = APP_STATE_INIT;
static bool wifi_setup_from_settings = false;
static bool initial_setup = false;
static char stored_ssid[WIFI_SSID_BUF_LEN];
static char stored_password[MAX_PASSWORD_LEN];
static char stored_tz[MAX_TIMEZONE_LEN];
static bool ntp_started = false;

// Hardware-timer-driven second boundary. esp_timer fires a callback at the
// precise us boundary (aligned to the NTP-disciplined clock), the callback
// signals this semaphore, and the clock state wakes, updates the display,
// and re-arms for the next boundary.
static SemaphoreHandle_t clock_tick_sem;
static esp_timer_handle_t clock_tick_timer;

// Adaptive display-pipeline compensation. ui_clock_update takes nonzero time
// to push pixels over SPI, so firing exactly at the wall-clock boundary makes
// the visible digit late. Keep separate EMAs by "number of digits that will
// repaint" so cheap one-second ticks and expensive rollovers don't poison each
// other's estimate, then fire the timer that many us early.
// Non-static: ui_clock.c reads this to pick the second that will be current
// when its pixels actually land (see comment in ui_clock_update).
volatile uint32_t clock_latency_us = 12000;
static uint32_t clock_latency_by_digits[7] = {
    0, 12000, 16000, 20000, 24000, 28000, 32000
};

static void clock_tick_cb(void *arg) {
    (void)arg;
    xSemaphoreGive(clock_tick_sem);
}

static void clock_tick_arm(void) {
    uint8_t predicted_digits = ui_clock_predict_next_update_digits();
    if (predicted_digits > 6) predicted_digits = 6;
    clock_latency_us = clock_latency_by_digits[predicted_digits];

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t us_until = (int64_t)(1000000 - (uint32_t)tv.tv_usec) -
                       (int64_t)clock_latency_us;
    // Too-soon guard: if the compensated fire time has already passed, fire
    // ASAP rather than pushing to the following second. Pushing a full second
    // skips the boundary we were aiming for - visible after ui_clock_redraw
    // when arming lands in the last ~latency us of the second (e.g. :35 jumps
    // straight to :37, never displaying :36). Firing immediately means the
    // pixels for the upcoming second land a few ms late but every second is
    // shown.
    if (us_until < 1000) us_until = 1000;
    esp_timer_stop(clock_tick_timer);
    esp_timer_start_once(clock_tick_timer, (uint64_t)us_until);
}

static void show_splash(void) {
    display_fill(COLOR_BLACK);
    ui_draw_centered_string(85, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK, false);
    ui_draw_centered_string(110, "The CYD Clock", COLOR_CYAN, COLOR_BLACK, false);
    ui_draw_centered_string(140, "Initializing...", COLOR_GRAY, COLOR_BLACK, false);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

static void try_connect_stored_credentials(void) {
    app_state = APP_STATE_CONNECTING;

    display_fill(COLOR_BLACK);
    ui_draw_centered_string(100, "Connecting to", COLOR_WHITE, COLOR_BLACK, false);
    ui_draw_centered_string(130, stored_ssid, COLOR_CYAN, COLOR_BLACK, false);

    wifi_init();
    if (wifi_connect(stored_ssid, stored_password)) {
        ESP_LOGI(TAG, "Connected with stored credentials");
        app_state = APP_STATE_CLOCK;
        ui_clock_init();
        ui_clock_redraw();

        // Start NTP
        wifi_start_ntp();
        ntp_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to connect with stored credentials");
        app_state = APP_STATE_WIFI_SETUP;
        ui_wifi_setup_init(false);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "CYD Clock starting");

    // Initialize hardware
    nvs_config_init();
    display_init();
    touch_init();
    led_init();

    // Second-boundary tick machinery
    clock_tick_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(clock_tick_sem ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t tick_args = {
        .callback = clock_tick_cb,
        .name     = "clock_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &clock_tick_timer));

    // Configure BOOT button as input with pull-up
    gpio_config_t boot_btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_btn_cfg);

    // Load and apply saved brightness (minimum 32)
    uint8_t brightness;
    if (nvs_config_get_brightness(&brightness) && brightness >= BRIGHTNESS_MIN) {
        display_set_backlight(brightness);
    }

    // Load and apply saved rotation
    bool rotated;
    if (nvs_config_get_rotation(&rotated)) {
        display_set_rotation(rotated);
    }

    show_splash();

    // Load timezone (default to UTC)
    if (!nvs_config_get_timezone(stored_tz)) {
        strncpy(stored_tz, "UTC0", sizeof(stored_tz) - 1);
        stored_tz[sizeof(stored_tz) - 1] = '\0';
    }
    wifi_set_timezone(stored_tz);

    // Load NTP settings
    char custom_ntp[MAX_NTP_SERVER_LEN];
    if (nvs_config_get_custom_ntp_server(custom_ntp)) {
        wifi_set_custom_ntp_server(custom_ntp);
    }
    bool ntp_ipv6;
    if (nvs_config_get_ntp_ipv6(&ntp_ipv6)) {
        wifi_set_ntp_prefer_ipv6(ntp_ipv6);
    }

    // Check for stored WiFi credentials
    if (nvs_config_get_wifi(stored_ssid, stored_password)) {
        try_connect_stored_credentials();
    } else {
        app_state = APP_STATE_WIFI_SETUP;
        initial_setup = true;
        ui_wifi_setup_init(false);
    }

    // Main loop
    while (1) {
        switch (app_state) {
            case APP_STATE_INIT:
                // Should not reach here
                break;

            case APP_STATE_WIFI_SETUP: {
                wifi_setup_result_t result = ui_wifi_setup_update();
                if (result == WIFI_SETUP_CONNECTED) {
                    // Save credentials
                    char ssid[WIFI_SSID_BUF_LEN], password[MAX_PASSWORD_LEN];
                    ui_wifi_setup_get_credentials(ssid, password);
                    nvs_config_set_wifi(ssid, password);

                    // Start NTP if not already started
                    if (!ntp_started) {
                        wifi_start_ntp();
                        ntp_started = true;
                    }

                    if (initial_setup) {
                        // First boot: prompt for timezone before showing clock
                        app_state = APP_STATE_TIMEZONE;
                        ui_timezone_init(stored_tz, false);
                    } else {
                        // From settings: go straight to clock
                        app_state = APP_STATE_CLOCK;
                        ui_clock_init();
                        ui_clock_redraw();
                    }
                    wifi_setup_from_settings = false;
                } else if (result == WIFI_SETUP_CANCELLED) {
                    if (wifi_setup_from_settings) {
                        // Return to settings
                        app_state = APP_STATE_SETTINGS;
                        wifi_setup_from_settings = false;
                        ui_settings_init();
                        ui_wait_for_touch_release();
                    }
                    // If not from settings, stay in WiFi setup (no stored credentials)
                }
                break;
            }

            case APP_STATE_CONNECTING:
                // Handled in try_connect_stored_credentials()
                break;

            case APP_STATE_CLOCK: {
                // On state (re)entry we need to arm the tick timer; this flag
                // gets set true when we transition away and on first boot.
                static bool tick_needs_arm = true;

                clock_touch_zone_t zone = ui_clock_check_touch();
                if (zone == CLOCK_TOUCH_SETTINGS) {
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                    tick_needs_arm = true;
                    // Wait for BOOT button release
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
                    }
                    continue;
                } else if (zone == CLOCK_TOUCH_STATS) {
                    app_state = APP_STATE_NTP_STATS;
                    ui_ntp_stats_init();
                    tick_needs_arm = true;
                    ui_wait_for_touch_release();
                    continue;
                }

                // Skip the FIRST latency measurement after re-entering the
                // clock state. ui_clock_redraw just repainted everything, so
                // that initial ui_clock_update typically has nothing to draw
                // and measures unrealistically short (~1-2 ms). Feeding that
                // into the EMA over many tap-settings-back cycles would drag
                // clock_latency_us far below the real steady-state value and
                // leave every subsequent tick's pixels landing visibly late.
                static bool skip_next_measurement = true;
                if (tick_needs_arm) {
                    // Drain any stale semaphore left over from a previous
                    // CLOCK session (timer may have fired while we were in
                    // SETTINGS etc), then arm for the next real boundary.
                    xSemaphoreTake(clock_tick_sem, 0);
                    // Wall time may have advanced a whole second past whatever
                    // ui_clock_redraw painted: ui_wait_for_touch_release blocks
                    // until the user lifts their finger, and the redraw itself
                    // takes nontrivial time. Without this catch-up call, the
                    // display sits on the stale second until the next tick,
                    // which then rounds up to the *following* second and
                    // visibly skips one (e.g. :00 -> :02).
                    ui_clock_update();
                    clock_tick_arm();
                    tick_needs_arm = false;
                    skip_next_measurement = true;
                }

                // Wait for the next second boundary (esp_timer callback) or
                // POLL_NORMAL_MS - whichever comes first. The timeout keeps
                // touch input responsive during the 1-second wait.
                if (xSemaphoreTake(clock_tick_sem,
                                   pdMS_TO_TICKS(POLL_NORMAL_MS)) == pdTRUE) {
                    // Measure the actual display-update latency and fold into
                    // the EMA used for the next tick's fire-early offset.
                    ui_clock_update();
                    uint32_t measured = ui_clock_last_visible_latency_us();
                    // Guard against pathological samples. Upper bound is high
                    // enough to keep true rollover samples, but rejects major
                    // scheduler stalls. Lower bound rejects "nothing changed"
                    // ticks that would spuriously pull the EMA down.
                    if (!skip_next_measurement &&
                        measured >= 3000 && measured < 80000) {
                        uint8_t digits = ui_clock_last_update_digits();
                        if (digits > 6) digits = 6;
                        // EMA with alpha = 1/8: new = (7/8)*old + (1/8)*measured.
                        // Converges in ~8 ticks, smooths cell-count variance.
                        clock_latency_by_digits[digits] =
                            clock_latency_by_digits[digits]
                            - (clock_latency_by_digits[digits] / 8)
                            + (measured / 8);
                    }
                    skip_next_measurement = false;
                    clock_tick_arm();
                }
                continue;
            }

            case APP_STATE_SETTINGS: {
                // BOOT button toggles settings: a press here returns to clock.
                // Same gesture that opened the menu closes it.
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    app_state = APP_STATE_CLOCK;
                    ui_clock_init();
                    ui_clock_redraw();
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
                    }
                    continue;
                }
                settings_result_t result = ui_settings_update();
                if (result == SETTINGS_RESULT_TIMEZONE) {
                    app_state = APP_STATE_TIMEZONE;
                    ui_timezone_init(stored_tz, true);
                    ui_wait_for_touch_release();
                } else if (result == SETTINGS_RESULT_WIFI) {
                    app_state = APP_STATE_WIFI_SETUP;
                    wifi_setup_from_settings = true;
                    ui_wifi_setup_init(true);
                    ui_wait_for_touch_release();
                } else if (result == SETTINGS_RESULT_NTP) {
                    app_state = APP_STATE_NTP;
                    ui_ntp_init();
                    ui_wait_for_touch_release();
                } else if (result == SETTINGS_RESULT_ABOUT) {
                    app_state = APP_STATE_ABOUT;
                    ui_about_init();
                    ui_wait_for_touch_release();
                } else if (result == SETTINGS_RESULT_DONE) {
                    app_state = APP_STATE_CLOCK;
                    ui_clock_init();
                    ui_clock_redraw();
                    ui_wait_for_touch_release();
                }
                break;
            }

            case APP_STATE_TIMEZONE: {
                tz_select_result_t result = ui_timezone_update();
                if (result == TZ_SELECT_DONE) {
                    // Save and apply new timezone
                    const char *tz = ui_timezone_get_selected();
                    strncpy(stored_tz, tz, sizeof(stored_tz) - 1);
                    stored_tz[sizeof(stored_tz) - 1] = '\0';
                    nvs_config_set_timezone(tz);
                    wifi_set_timezone(tz);
                    ESP_LOGI(TAG, "Timezone set to: %s", ui_timezone_get_name());
                }
                if (result == TZ_SELECT_DONE || result == TZ_SELECT_CANCELLED) {
                    if (initial_setup) {
                        // First boot: go to clock
                        initial_setup = false;
                        app_state = APP_STATE_CLOCK;
                        ui_clock_init();
                        ui_clock_redraw();
                    } else {
                        // From settings: return to settings
                        app_state = APP_STATE_SETTINGS;
                        ui_settings_init();
                    }
                }
                break;
            }

            case APP_STATE_ABOUT: {
                about_result_t result = ui_about_update();
                if (result == ABOUT_RESULT_BACK) {
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                    ui_wait_for_touch_release();
                }
                break;
            }

            case APP_STATE_NTP: {
                ntp_result_t result = ui_ntp_update();
                if (result == NTP_RESULT_BACK) {
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                    ui_wait_for_touch_release();
                }
                break;
            }

            case APP_STATE_NTP_STATS: {
                ntp_stats_result_t result = ui_ntp_stats_update();
                if (result == NTP_STATS_RESULT_BACK) {
                    app_state = APP_STATE_CLOCK;
                    ui_clock_init();
                    ui_clock_redraw();
                    ui_wait_for_touch_release();
                } else if (result == NTP_STATS_RESULT_SETTINGS) {
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
                    }
                }
                break;
            }
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
    }
}
