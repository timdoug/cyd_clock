#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "nvs_config.h"
#include "ui_common.h"
#include "ui_clock.h"
#include "ui_wifi_setup.h"
#include "ui_timezone.h"
#include "ui_settings.h"
#include "ui_about.h"
#include "ui_ntp.h"

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
} app_state_t;

static app_state_t app_state = APP_STATE_INIT;
static bool wifi_setup_from_settings = false;
static char stored_ssid[MAX_SSID_LEN];
static char stored_password[MAX_PASSWORD_LEN];
static char stored_tz[MAX_TIMEZONE_LEN];
static bool ntp_started = false;

static void show_splash(void) {
    display_fill(COLOR_BLACK);
    display_string((DISPLAY_WIDTH - 15 * CHAR_WIDTH) / 2, 85, "Domaine Nyquist", COLOR_GRAY, COLOR_BLACK);
    display_string((DISPLAY_WIDTH - 13 * CHAR_WIDTH) / 2, 110, "The CYD Clock", COLOR_CYAN, COLOR_BLACK);
    display_string((DISPLAY_WIDTH - 14 * CHAR_WIDTH) / 2, 140, "Initializing...", COLOR_GRAY, COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

static void try_connect_stored_credentials(void) {
    app_state = APP_STATE_CONNECTING;

    display_fill(COLOR_BLACK);
    display_string((DISPLAY_WIDTH - 13 * CHAR_WIDTH) / 2, 100, "Connecting to", COLOR_WHITE, COLOR_BLACK);
    display_string((DISPLAY_WIDTH - strlen(stored_ssid) * CHAR_WIDTH) / 2, 130, stored_ssid, COLOR_CYAN, COLOR_BLACK);

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
        ui_wifi_setup_init();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "CYD Clock starting");

    // Initialize hardware
    nvs_config_init();
    display_init();
    touch_init();
    led_init();

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
    uint32_t ntp_interval;
    if (nvs_config_get_ntp_interval(&ntp_interval)) {
        wifi_set_ntp_interval(ntp_interval);
    }

    // Check for stored WiFi credentials
    if (nvs_config_get_wifi(stored_ssid, stored_password)) {
        try_connect_stored_credentials();
    } else {
        app_state = APP_STATE_WIFI_SETUP;
        ui_wifi_setup_init();
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
                    char ssid[33], password[64];
                    ui_wifi_setup_get_credentials(ssid, password);
                    nvs_config_set_wifi(ssid, password);

                    // Switch to clock
                    app_state = APP_STATE_CLOCK;
                    wifi_setup_from_settings = false;
                    ui_clock_init();
                    ui_clock_redraw();

                    // Start NTP if not already started
                    if (!ntp_started) {
                        wifi_start_ntp();
                        ntp_started = true;
                    }
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
                static int last_sec = -1;

                // Check for touch input first
                clock_touch_zone_t zone = ui_clock_check_touch();
                if (zone == CLOCK_TOUCH_SETTINGS) {
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                    last_sec = -1;  // Reset on return
                    ui_wait_for_touch_release();
                    continue;
                }

                // Get current time
                struct timeval tv;
                gettimeofday(&tv, NULL);
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);

                // Update display when second changes
                if (timeinfo.tm_sec != last_sec) {
                    ui_clock_update();
                    last_sec = timeinfo.tm_sec;
                }

                // Poll faster near second boundary for responsiveness
                int ms_in_sec = tv.tv_usec / 1000;
                if (ms_in_sec > POLL_THRESHOLD_MS) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_FAST_MS));
                } else if (ms_in_sec > 900) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_MID_MS));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(POLL_NORMAL_MS));
                }
                continue;
            }

            case APP_STATE_SETTINGS: {
                settings_result_t result = ui_settings_update();
                if (result == SETTINGS_RESULT_TIMEZONE) {
                    app_state = APP_STATE_TIMEZONE;
                    ui_timezone_init(stored_tz);
                    ui_wait_for_touch_release();
                } else if (result == SETTINGS_RESULT_WIFI) {
                    app_state = APP_STATE_WIFI_SETUP;
                    wifi_setup_from_settings = true;
                    ui_wifi_setup_init();
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

                    // Return to settings
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
                } else if (result == TZ_SELECT_CANCELLED) {
                    // Return to settings without changing
                    app_state = APP_STATE_SETTINGS;
                    ui_settings_init();
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
                } else if (result == NTP_RESULT_SYNCED) {
                    app_state = APP_STATE_CLOCK;
                    ui_clock_init();
                    ui_clock_redraw();
                }
                break;
            }
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
    }
}
