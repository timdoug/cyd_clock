#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_SCAN_RESULTS 15
#define DEFAULT_NTP_SERVER "pool.ntp.org"

// WiFi network info from scan
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;  // 0 = open, other = secured
} wifi_network_t;

// Initialize WiFi subsystem
void wifi_init(void);

// Scan for available networks
// Returns number of networks found (up to MAX_SCAN_RESULTS)
int wifi_scan(wifi_network_t *networks, int max_networks);

// Non-blocking WiFi scan. Start returns true if the scan was accepted; poll
// returns true only once results are ready and writes the network count.
bool wifi_scan_start_async(void);
bool wifi_scan_poll(wifi_network_t *networks, int max_networks, int *count);
void wifi_scan_cancel(void);

// Connect to a network
// Returns true if connection initiated successfully
bool wifi_connect(const char *ssid, const char *password);

// Check if connected to WiFi
bool wifi_is_connected(void);

// Fire any due background reconnect attempt; call from the main loop
void wifi_poll_reconnect(void);


// Start NTP time sync
void wifi_start_ntp(void);

// NTP server management
const char *wifi_get_custom_ntp_server(void);
void wifi_set_custom_ntp_server(const char *server);

// Set timezone (POSIX TZ format, e.g., "PST8PDT,M3.2.0,M11.1.0")
void wifi_set_timezone(const char *tz);

// Get current IPv4 address as string (returns "0.0.0.0" if not connected)
void wifi_get_ip_str(char *buf, size_t len);

// Get current IPv6 address as string (returns "" if no global address)
void wifi_get_ip6_str(char *buf, size_t len);

// Get resolved NTP server IP as string (returns "" if not yet resolved)
void wifi_get_ntp_server_ip_str(char *buf, size_t len);

// NTP IPv6 preference (manual AAAA resolution before starting SNTP)
bool wifi_get_ntp_prefer_ipv6(void);
void wifi_set_ntp_prefer_ipv6(bool prefer);

// Get WiFi RSSI (signal strength in dBm, returns 0 if not connected)
int8_t wifi_get_rssi(void);

// Get MAC address as string (format: "AA:BB:CC:DD:EE:FF")
void wifi_get_mac_str(char *buf, size_t len);

#endif // WIFI_H
