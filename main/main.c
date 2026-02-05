#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display.h"
#include "touch.h"
#include "wifi.h"
#include "nvs_config.h"
#include "ui_clock.h"
#include "ui_wifi_setup.h"
#include "ui_timezone.h"
#include "ui_settings.h"
#include "ui_about.h"

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
} app_state_t;

static app_state_t app_state = APP_STATE_INIT;
static char stored_ssid[MAX_SSID_LEN];
static char stored_password[MAX_PASSWORD_LEN];
static char stored_tz[MAX_TIMEZONE_LEN];
static bool ntp_started = false;

static void show_splash(void) {
    display_fill(COLOR_BLACK);
    display_string(110, 100, "CYD Clock", COLOR_CYAN, COLOR_BLACK);
    display_string(100, 130, "Initializing...", COLOR_GRAY, COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void try_connect_stored_credentials(void) {
    app_state = APP_STATE_CONNECTING;

    display_fill(COLOR_BLACK);
    display_string(100, 100, "Connecting to", COLOR_WHITE, COLOR_BLACK);
    display_string((DISPLAY_WIDTH - strlen(stored_ssid) * 8) / 2, 130, stored_ssid, COLOR_CYAN, COLOR_BLACK);

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

    // Load and apply saved brightness (minimum 32)
    uint8_t brightness;
    if (nvs_config_get_brightness(&brightness) && brightness >= 32) {
        display_set_backlight(brightness);
    }

    show_splash();

    // Load timezone (default to UTC)
    if (!nvs_config_get_timezone(stored_tz)) {
        strcpy(stored_tz, "UTC0");
    }
    wifi_set_timezone(stored_tz);

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
                    ui_clock_init();
                    ui_clock_redraw();

                    // Start NTP if not already started
                    if (!ntp_started) {
                        wifi_start_ntp();
                        ntp_started = true;
                    }
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
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    continue;
                }

                // Get current time
                struct timeval tv;
                gettimeofday(&tv, NULL);
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);

                // Update display when second changes
                if (timeinfo.tm_sec != last_sec) {
                    ui_clock_set_synced(wifi_time_is_synced());
                    ui_clock_update();
                    last_sec = timeinfo.tm_sec;
                }

                // Poll faster near second boundary for responsiveness
                int ms_in_sec = tv.tv_usec / 1000;
                if (ms_in_sec > 950) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                continue;
            }

            case APP_STATE_SETTINGS: {
                settings_result_t result = ui_settings_update();
                if (result == SETTINGS_RESULT_TIMEZONE) {
                    app_state = APP_STATE_TIMEZONE;
                    ui_timezone_init();
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                } else if (result == SETTINGS_RESULT_WIFI) {
                    app_state = APP_STATE_WIFI_SETUP;
                    ui_wifi_setup_init();
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                } else if (result == SETTINGS_RESULT_ABOUT) {
                    app_state = APP_STATE_ABOUT;
                    ui_about_init();
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                } else if (result == SETTINGS_RESULT_DONE) {
                    app_state = APP_STATE_CLOCK;
                    ui_clock_init();
                    ui_clock_redraw();
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                break;
            }

            case APP_STATE_TIMEZONE: {
                tz_select_result_t result = ui_timezone_update();
                if (result == TZ_SELECT_DONE) {
                    // Save and apply new timezone
                    const char *tz = ui_timezone_get_selected();
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
                    while (touch_is_pressed()) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                break;
            }
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
