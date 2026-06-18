#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "ntp.h"

#define MAX_SCAN_RESULTS 15
#define DEFAULT_NTP_SERVER "pool.ntp.org"

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_network_t;

void wifi_init(void);

int wifi_scan(wifi_network_t *networks, int max_networks);

// Non-blocking WiFi scan. Start returns true if the scan was accepted; poll
// returns true only once results are ready and writes the network count.
bool wifi_scan_start_async(void);
bool wifi_scan_poll(wifi_network_t *networks, int max_networks, int *count);
void wifi_scan_cancel(void);

bool wifi_connect(const char *ssid, const char *password);

bool wifi_is_connected(void);

void wifi_poll_reconnect(void);


void wifi_start_ntp(void);

const char *wifi_get_custom_ntp_server(void);
void wifi_set_custom_ntp_server(const char *server);

void wifi_set_timezone(const char *tz);

void wifi_get_ip_str(char *buf, size_t len);

void wifi_get_ip6_str(char *buf, size_t len);

void wifi_get_ntp_server_ip_str(char *buf, size_t len);

bool wifi_get_ntp_prefer_ipv6(void);
void wifi_set_ntp_prefer_ipv6(bool prefer);

nts_mode_t wifi_get_nts_mode(void);
void wifi_set_nts_mode(nts_mode_t mode);

int8_t wifi_get_rssi(void);

void wifi_get_mac_str(char *buf, size_t len);

#endif
