#include "wifi.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_LWIP_IPV6_DHCP6
#include "lwip/dhcp6.h"
#include "lwip/netif.h"
#endif
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "config.h"
#include "ntp.h"
#include "util.h"

static const char *TAG = "wifi";

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool wifi_initialized = false;
static int retry_count = 0;
static bool ignore_next_disconnect = false;

// Background reconnect: a wall clock must never stop trying. The blocking
// wifi_connect() stops WAITING after WIFI_MAX_RETRY fast retries so the
// caller gets a timely verdict (boot fallback, setup-screen feedback), but
// the disconnect handler keeps scheduling retries forever with capped
// exponential backoff - otherwise a router reboot at 3am stranded the
// clock offline until a power cycle, silently drifting on the crystal.
// The retry is a deadline polled from the watchdogged main loop rather
// than an esp_timer: task-dispatch timers have been observed to stall
// (see led.c), and this path must outlive everything.
#define WIFI_RECONNECT_MIN_MS 1000
#define WIFI_RECONNECT_MAX_MS 30000
static volatile bool     reconnect_pending = false;
static volatile uint32_t reconnect_at_ms;
static uint32_t reconnect_delay_ms = WIFI_RECONNECT_MIN_MS;

static uint32_t mono_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void schedule_reconnect(void) {
    reconnect_at_ms = mono_ms() + reconnect_delay_ms;
    reconnect_pending = true;
    reconnect_delay_ms *= 2;
    if (reconnect_delay_ms > WIFI_RECONNECT_MAX_MS) {
        reconnect_delay_ms = WIFI_RECONNECT_MAX_MS;
    }
}

void wifi_poll_reconnect(void) {
    if (!reconnect_pending) return;
    // Signed difference handles the ~49-day mono_ms wrap.
    if ((int32_t)(mono_ms() - reconnect_at_ms) < 0) return;
    reconnect_pending = false;
    ESP_LOGI(TAG, "Background reconnect attempt");
    // On immediate failure no DISCONNECTED event will re-schedule, so
    // re-schedule here; the backoff cap bounds the retry rate.
    if (esp_wifi_connect() != ESP_OK) schedule_reconnect();
}

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
                xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
                if (ignore_next_disconnect) {
                    ignore_next_disconnect = false;
                    break;
                }
                if (retry_count < WIFI_MAX_RETRY) {
                    esp_wifi_connect();
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)", retry_count, WIFI_MAX_RETRY);
                } else {
                    // Unblock any wifi_connect() waiting on a verdict...
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                    // ...but never actually give up.
                    schedule_reconnect();
                }
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
            reconnect_delay_ms = WIFI_RECONNECT_MIN_MS;
            reconnect_pending = false;
            xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
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
#if CONFIG_LWIP_IPV6_DHCP6
    struct netif *lwip_netif = esp_netif_get_netif_impl(netif);
    if (lwip_netif) {
        err_t err = dhcp6_enable_stateless(lwip_netif);
        if (err != ERR_OK) {
            ESP_LOGW(TAG, "DHCPv6 stateless start failed: %d", err);
        }
    }
#endif

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

    // IDF defaults to WIFI_PS_MIN_MODEM, where the radio sleeps between DTIM
    // beacons and the AP buffers inbound unicast until the next wake - up to
    // ~100 ms (AP-dependent) of INBOUND-ONLY delay on NTP responses. That
    // one-directional delay is exactly the path asymmetry that biases the
    // NTP offset by half the buffering time. We're wall-powered; keep the
    // receiver on.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // RX hook install moved to the IP_EVENT_STA_GOT_IP handler - wifi-netif
    // registers its own STA rxcb on the connect event, which would clobber
    // ours if we installed here. TX-done cb is global and unaffected, but
    // we register both from the same place to keep them in sync.

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
}

int wifi_scan(wifi_network_t *networks, int max_networks) {
    if (!networks || max_networks <= 0) return 0;
    if (!wifi_initialized) {
        wifi_init();
    }

    ESP_LOGI(TAG, "Starting WiFi scan");

    // Pause any pending background reconnect for the scan window - a
    // connect attempt in flight makes esp_wifi_scan_start fail.
    bool resume_reconnect = reconnect_pending;
    reconnect_pending = false;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed to start: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan count failed: %s", esp_err_to_name(err));
        return 0;
    }

    if (ap_count == 0) {
        ESP_LOGI(TAG, "No networks found");
        return 0;
    }

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        return 0;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan records failed: %s", esp_err_to_name(err));
        free(ap_records);
        return 0;
    }

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

        str_copy(networks[count].ssid, sizeof(networks[count].ssid), (char *)ap_records[i].ssid);
        networks[count].rssi = ap_records[i].rssi;
        networks[count].authmode = (ap_records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        count++;
    }

    free(ap_records);
    ESP_LOGI(TAG, "Found %d networks", count);
    if (resume_reconnect && !wifi_is_connected()) schedule_reconnect();
    return count;
}

bool wifi_connect(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) return false;
    if (!password) password = "";
    if (!wifi_initialized) {
        wifi_init();
    }

    ESP_LOGI(TAG, "Connecting to %s", ssid);

    wifi_config_t wifi_config = {0};
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(wifi_config.sta.ssid)) ssid_len = sizeof(wifi_config.sta.ssid);
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    str_copy((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    bool was_connected = wifi_is_connected();

    // Clear previous state; this attempt supersedes any background retry.
    reconnect_pending = false;
    reconnect_delay_ms = WIFI_RECONNECT_MIN_MS;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    retry_count = 0;

    ignore_next_disconnect = was_connected;
    esp_err_t disc_err = esp_wifi_disconnect();
    if (disc_err != ESP_OK) {
        ignore_next_disconnect = false;
    }
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
        // Stop WAITING, not trying: the disconnect handler keeps fast
        // retries and then capped-backoff retries running autonomously
        // (tearing the attempt down here also risked a lingering
        // ignore_next_disconnect eating the next real disconnect event and
        // breaking the retry chain). For the boot path the active config is
        // the stored, previously proven credentials; after a failed setup
        // attempt the retries burn harmlessly until the user fixes them,
        // and any new wifi_connect() supersedes the timer.
        ESP_LOGW(TAG, "No connection to %s within %d ms; retrying in background",
                 ssid, WIFI_CONNECT_TIMEOUT_MS);
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
    str_copy(ntp_cfg.custom_server, sizeof(ntp_cfg.custom_server), server);
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
    if (!buf || len == 0) return;
    if (!ntp_cfg.started) { buf[0] = '\0'; return; }
    ntp_get_primary_addr_str(buf, len);
}

void wifi_get_ip_str(char *buf, size_t len) {
    if (!buf || len == 0) return;
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
    if (!buf || len == 0) return;
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
    if (!buf || len == 0) return;
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(buf, len, "??:??:??:??:??:??");
    }
}
