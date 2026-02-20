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

// NTP statistics
typedef struct {
    bool synced;              // Whether time has been synced at least once
    time_t last_sync_time;    // Unix timestamp of last successful sync
    uint32_t sync_count;      // Total number of successful syncs
    uint32_t sync_interval;   // Current sync interval in seconds
    uint32_t sync_elapsed_ms; // Milliseconds since sync attempt started (when not synced)
    int64_t last_offset_ms;   // Clock offset at last sync in milliseconds
    const char *server;       // Current NTP server name
} ntp_stats_t;

// Initialize WiFi subsystem
void wifi_init(void);

// Scan for available networks
// Returns number of networks found (up to MAX_SCAN_RESULTS)
int wifi_scan(wifi_network_t *networks, int max_networks);

// Connect to a network
// Returns true if connection initiated successfully
bool wifi_connect(const char *ssid, const char *password);

// Check if connected to WiFi
bool wifi_is_connected(void);


// Start NTP time sync
void wifi_start_ntp(void);


// Get NTP statistics
void wifi_get_ntp_stats(ntp_stats_t *stats);

// Set NTP sync interval (in seconds, minimum 15)
void wifi_set_ntp_interval(uint32_t seconds);

// Get NTP sync interval
uint32_t wifi_get_ntp_interval(void);

// Force an immediate NTP sync
void wifi_force_ntp_sync(void);

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
