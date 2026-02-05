#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "wifi";

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool wifi_initialized = false;
static bool time_synced = false;
static int retry_count = 0;
#define MAX_RETRY 5

// NTP server (custom only, default to pool.ntp.org)
static char custom_ntp_server[64] = "pool.ntp.org";

// NTP statistics
static time_t last_sync_time = 0;
static uint32_t sync_count = 0;
static uint32_t ntp_interval = 21600;  // Default 6 hours

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                if (retry_count < MAX_RETRY) {
                    esp_wifi_connect();
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)", retry_count, MAX_RETRY);
                } else {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void time_sync_notification_cb(struct timeval *tv) {
    time_synced = true;
    last_sync_time = tv->tv_sec;
    sync_count++;
    ESP_LOGI(TAG, "NTP time synchronized (sync #%lu)", (unsigned long)sync_count);
}

void wifi_init(void) {
    if (wifi_initialized) return;

    ESP_LOGI(TAG, "Initializing WiFi");

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
}

int wifi_scan(wifi_network_t *networks, int max_networks) {
    if (!wifi_initialized) {
        wifi_init();
    }

    ESP_LOGI(TAG, "Starting WiFi scan");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    if (ap_count == 0) {
        ESP_LOGI(TAG, "No networks found");
        return 0;
    }

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        return 0;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    int count = 0;
    for (int i = 0; i < ap_count && count < max_networks; i++) {
        // Skip empty SSIDs
        if (ap_records[i].ssid[0] == '\0') continue;

        // Skip duplicates
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(networks[j].ssid, (char *)ap_records[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        strncpy(networks[count].ssid, (char *)ap_records[i].ssid, 32);
        networks[count].ssid[32] = '\0';
        networks[count].rssi = ap_records[i].rssi;
        networks[count].authmode = (ap_records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        count++;
    }

    free(ap_records);
    ESP_LOGI(TAG, "Found %d networks", count);
    return count;
}

bool wifi_connect(const char *ssid, const char *password) {
    if (!wifi_initialized) {
        wifi_init();
    }

    ESP_LOGI(TAG, "Connecting to %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Clear previous state
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    retry_count = 0;

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        return true;
    } else {
        ESP_LOGW(TAG, "Failed to connect to %s", ssid);
        return false;
    }
}

bool wifi_is_connected(void) {
    if (!wifi_event_group) return false;
    return (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_disconnect(void) {
    esp_wifi_disconnect();
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
}

void wifi_start_ntp(void) {
    const char *server = custom_ntp_server[0] ? custom_ntp_server : "pool.ntp.org";

    ESP_LOGI(TAG, "Starting NTP sync (server: %s, interval: %lu sec)",
             server, (unsigned long)ntp_interval);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_set_sync_interval(ntp_interval * 1000);  // Convert to ms
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

bool wifi_time_is_synced(void) {
    return time_synced;
}

void wifi_set_timezone(const char *tz) {
    ESP_LOGI(TAG, "Setting timezone: %s", tz);
    setenv("TZ", tz, 1);
    tzset();
}

void wifi_get_ntp_stats(ntp_stats_t *stats) {
    stats->synced = time_synced;
    stats->last_sync_time = last_sync_time;
    stats->sync_count = sync_count;
    stats->sync_interval = ntp_interval;

    // Get current server (index 0 is primary)
    stats->server = esp_sntp_getservername(0);
    if (!stats->server) {
        stats->server = "N/A";
    }
}

void wifi_set_ntp_interval(uint32_t seconds) {
    if (seconds < 15) seconds = 15;  // SNTP minimum
    ntp_interval = seconds;

    // Update the running SNTP if initialized
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        esp_sntp_set_sync_interval(seconds * 1000);
        esp_sntp_init();
    }
}

void wifi_force_ntp_sync(void) {
    if (esp_sntp_enabled()) {
        esp_sntp_restart();
    }
}

void wifi_restart_ntp(void) {
    // Restart NTP with current server if running
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        wifi_start_ntp();
    }
}

const char *wifi_get_custom_ntp_server(void) {
    return custom_ntp_server;
}

void wifi_set_custom_ntp_server(const char *server) {
    strncpy(custom_ntp_server, server, sizeof(custom_ntp_server) - 1);
    custom_ntp_server[sizeof(custom_ntp_server) - 1] = '\0';
}

uint32_t wifi_get_ntp_interval(void) {
    return ntp_interval;
}
