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

// Disconnect from WiFi
void wifi_disconnect(void);

// Start NTP time sync
void wifi_start_ntp(void);

// Check if time is synchronized
bool wifi_time_is_synced(void);

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
void wifi_restart_ntp(void);

// Set timezone (POSIX TZ format, e.g., "PST8PDT,M3.2.0,M11.1.0")
void wifi_set_timezone(const char *tz);

// Get current IP address as string (returns "0.0.0.0" if not connected)
void wifi_get_ip_str(char *buf, size_t len);

#endif // WIFI_H
