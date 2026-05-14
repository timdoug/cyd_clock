#include "wifi.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "config.h"
#include "ntp.h"

static const char *TAG = "wifi";

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool wifi_initialized = false;
static int retry_count = 0;

// Config tracked here; runtime NTP state lives in ntp.c
static struct {
    char custom_server[64];
    bool prefer_ipv6;
    bool started;
} ntp_cfg = {
    .custom_server = DEFAULT_NTP_SERVER,
    .prefer_ipv6 = false,
    .started = false,
};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                if (retry_count < WIFI_MAX_RETRY) {
                    esp_wifi_connect();
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)", retry_count, WIFI_MAX_RETRY);
                } else {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IPv4: " IPSTR, IP2STR(&event->ip_info.ip));
            // wifi-netif registers its STA rxcb on the connect event, AFTER
            // esp_wifi_start. Install/re-install ours here so we sit on top
            // of theirs (and so reconnects also re-install).
            ntp_install_wifi_rx_hook();
            retry_count = 0;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        } else if (event_id == IP_EVENT_GOT_IP6) {
            ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
            ESP_LOGI(TAG, "Got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));
        }
    }
}

void wifi_init(void) {
    if (wifi_initialized) return;

    ESP_LOGI(TAG, "Initializing WiFi");

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_ip6_linklocal(netif);

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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_GOT_IP6,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // RX hook install moved to the IP_EVENT_STA_GOT_IP handler - wifi-netif
    // registers its own STA rxcb on the connect event, which would clobber
    // ours if we installed here. TX-done cb is global and unaffected, but
    // we register both from the same place to keep them in sync.

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
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

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


void wifi_start_ntp(void) {
    const char *server = wifi_get_custom_ntp_server();
    ESP_LOGI(TAG, "Starting NTP (server: %s, ipv6: %s)",
             server, ntp_cfg.prefer_ipv6 ? "yes" : "no");

    if (ntp_cfg.started) {
        ntp_set_server(server);
        ntp_set_prefer_ipv6(ntp_cfg.prefer_ipv6);
    } else {
        ntp_init(server, ntp_cfg.prefer_ipv6);
        ntp_cfg.started = true;
    }
}


void wifi_set_timezone(const char *tz) {
    ESP_LOGI(TAG, "Setting timezone: %s", tz);
    setenv("TZ", tz, 1);
    tzset();
}


const char *wifi_get_custom_ntp_server(void) {
    return ntp_cfg.custom_server[0] ? ntp_cfg.custom_server : DEFAULT_NTP_SERVER;
}

void wifi_set_custom_ntp_server(const char *server) {
    strncpy(ntp_cfg.custom_server, server, sizeof(ntp_cfg.custom_server) - 1);
    ntp_cfg.custom_server[sizeof(ntp_cfg.custom_server) - 1] = '\0';
    if (ntp_cfg.started) ntp_set_server(wifi_get_custom_ntp_server());
}

bool wifi_get_ntp_prefer_ipv6(void) {
    return ntp_cfg.prefer_ipv6;
}

void wifi_set_ntp_prefer_ipv6(bool prefer) {
    ntp_cfg.prefer_ipv6 = prefer;
    if (ntp_cfg.started) ntp_set_prefer_ipv6(prefer);
}

void wifi_get_ntp_server_ip_str(char *buf, size_t len) {
    if (!ntp_cfg.started) { if (len) buf[0] = '\0'; return; }
    ntp_get_primary_addr_str(buf, len);
}

void wifi_get_ip_str(char *buf, size_t len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && wifi_is_connected()) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }
    snprintf(buf, len, "0.0.0.0");
}

void wifi_get_ip6_str(char *buf, size_t len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && wifi_is_connected()) {
        esp_ip6_addr_t ip6_addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int count = esp_netif_get_all_ip6(netif, ip6_addrs);
        for (int i = 0; i < count; i++) {
            // Skip link-local (fe80::) addresses, prefer global
            if (!ip6_addr_islinklocal(&ip6_addrs[i])) {
                snprintf(buf, len, IPV6STR, IPV62STR(ip6_addrs[i]));
                return;
            }
        }
        // Fall back to link-local if no global address
        if (count > 0) {
            snprintf(buf, len, IPV6STR, IPV62STR(ip6_addrs[0]));
            return;
        }
    }
    buf[0] = '\0';
}

int8_t wifi_get_rssi(void) {
    if (!wifi_is_connected()) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void wifi_get_mac_str(char *buf, size_t len) {
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(buf, len, "??:??:??:??:??:??");
    }
}
