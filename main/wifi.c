#include "wifi.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "config.h"

static const char *TAG = "wifi";

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool wifi_initialized = false;
static int retry_count = 0;

// NTP state grouped together
static struct {
    bool synced;
    time_t last_sync_time;
    uint32_t sync_start_ticks;
    uint32_t sync_count;
    uint32_t interval;
    int64_t last_offset_us;
    char custom_server[64];
    bool prefer_ipv6;
} ntp_state = {
    .synced = false,
    .last_sync_time = 0,
    .sync_start_ticks = 0,
    .sync_count = 0,
    .interval = NTP_DEFAULT_INTERVAL_SEC,
    .custom_server = DEFAULT_NTP_SERVER,
    .prefer_ipv6 = false,
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
            retry_count = 0;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        } else if (event_id == IP_EVENT_GOT_IP6) {
            ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
            ESP_LOGI(TAG, "Got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));
        }
    }
}

static void time_sync_notification_cb(struct timeval *tv) {
    ntp_state.synced = true;
    ntp_state.last_sync_time = tv->tv_sec;
    ntp_state.sync_count++;
    ESP_LOGI(TAG, "NTP time synchronized (sync #%lu, offset %+lldms)",
             (unsigned long)ntp_state.sync_count, (long long)(ntp_state.last_offset_us / 1000));
}

// Override weak sntp_sync_time to capture clock offset before correction
void sntp_sync_time(struct timeval *tv) {
    struct timeval now;
    gettimeofday(&now, NULL);
    ntp_state.last_offset_us = ((int64_t)tv->tv_sec - now.tv_sec) * 1000000LL
                             + (tv->tv_usec - now.tv_usec);

    settimeofday(tv, NULL);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    time_sync_notification_cb(tv);
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

    ESP_LOGI(TAG, "Starting NTP sync (server: %s, interval: %lu sec, ipv6: %s)",
             server, (unsigned long)ntp_state.interval,
             ntp_state.prefer_ipv6 ? "yes" : "no");

    ntp_state.sync_start_ticks = xTaskGetTickCount();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_interval(ntp_state.interval * 1000);  // Convert to ms
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    // Resolve synchronously so the address is immediately visible in the UI
    struct addrinfo hints = {
        .ai_family = ntp_state.prefer_ipv6 ? AF_INET6 : AF_INET,
    };
    struct addrinfo *res = NULL;
    bool resolved = false;
    if (getaddrinfo(server, NULL, &hints, &res) == 0 && res) {
        ip_addr_t sntp_addr = {0};
        if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(&sntp_addr.u_addr.ip6.addr, &sa6->sin6_addr, 16);
            sntp_addr.type = IPADDR_TYPE_V6;
        } else {
            struct sockaddr_in *sa4 = (struct sockaddr_in *)res->ai_addr;
            sntp_addr.u_addr.ip4.addr = sa4->sin_addr.s_addr;
            sntp_addr.type = IPADDR_TYPE_V4;
        }
        esp_sntp_setserver(0, &sntp_addr);
        char addr_str[46];
        ipaddr_ntoa_r(&sntp_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "Resolved %s to %s", server, addr_str);
        resolved = true;
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "DNS resolution failed for %s, using hostname mode", server);
        if (res) freeaddrinfo(res);
    }

    if (!resolved) {
        esp_sntp_setservername(0, server);
    }

    esp_sntp_init();
}


void wifi_set_timezone(const char *tz) {
    ESP_LOGI(TAG, "Setting timezone: %s", tz);
    setenv("TZ", tz, 1);
    tzset();
}

void wifi_get_ntp_stats(ntp_stats_t *stats) {
    stats->synced = ntp_state.synced;
    stats->last_sync_time = ntp_state.last_sync_time;
    stats->sync_count = ntp_state.sync_count;
    stats->sync_interval = ntp_state.interval;

    // Calculate elapsed time since sync started
    if (!ntp_state.synced && ntp_state.sync_start_ticks > 0) {
        stats->sync_elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - ntp_state.sync_start_ticks);
    } else {
        stats->sync_elapsed_ms = 0;
    }

    stats->last_offset_ms = ntp_state.last_offset_us / 1000;

    // Get current server (index 0 is primary)
    // When using esp_sntp_setserver() (IP mode), the name is NULL
    stats->server = esp_sntp_getservername(0);
    if (!stats->server) {
        stats->server = wifi_get_custom_ntp_server();
    }
}

void wifi_set_ntp_interval(uint32_t seconds) {
    if (seconds < NTP_MIN_INTERVAL_SEC) seconds = NTP_MIN_INTERVAL_SEC;
    ntp_state.interval = seconds;

    // Update the running SNTP if initialized
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        esp_sntp_set_sync_interval(seconds * 1000);
        esp_sntp_init();
    }
}

void wifi_force_ntp_sync(void) {
    // Full restart to pick up any new server/interval settings
    if (esp_sntp_enabled()) {
        ntp_state.synced = false;  // Reset so UI shows "Syncing..." state
        esp_sntp_stop();
        wifi_start_ntp();
    }
}


const char *wifi_get_custom_ntp_server(void) {
    return ntp_state.custom_server[0] ? ntp_state.custom_server : DEFAULT_NTP_SERVER;
}

void wifi_set_custom_ntp_server(const char *server) {
    strncpy(ntp_state.custom_server, server, sizeof(ntp_state.custom_server) - 1);
    ntp_state.custom_server[sizeof(ntp_state.custom_server) - 1] = '\0';
}

uint32_t wifi_get_ntp_interval(void) {
    return ntp_state.interval;
}

bool wifi_get_ntp_prefer_ipv6(void) {
    return ntp_state.prefer_ipv6;
}

void wifi_set_ntp_prefer_ipv6(bool prefer) {
    ntp_state.prefer_ipv6 = prefer;
}

void wifi_get_ntp_server_ip_str(char *buf, size_t len) {
    if (!esp_sntp_enabled()) {
        buf[0] = '\0';
        return;
    }
    const ip_addr_t *addr = esp_sntp_getserver(0);
    if (addr && !ip_addr_isany(addr)) {
        ipaddr_ntoa_r(addr, buf, len);
        return;
    }
    buf[0] = '\0';
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
