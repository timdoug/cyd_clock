#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Buffer sizes including the terminating NUL. 802.11 SSIDs are up to 32 bytes.
#define WIFI_SSID_BUF_LEN 33
#define MAX_PASSWORD_LEN 64
#define MAX_TIMEZONE_LEN 48

void nvs_config_init(void);

bool nvs_config_get_wifi(char *ssid, char *password);
void nvs_config_set_wifi(const char *ssid, const char *password);

bool nvs_config_get_timezone(char *tz);
void nvs_config_set_timezone(const char *tz);

bool nvs_config_get_brightness(uint8_t *brightness);
void nvs_config_set_brightness(uint8_t brightness);

#define MAX_NTP_SERVER_LEN 64
bool nvs_config_get_custom_ntp_server(char *server);
void nvs_config_set_custom_ntp_server(const char *server);

#define MAX_OTA_URL_LEN 256
bool nvs_config_get_ota_url(char *url);
void nvs_config_set_ota_url(const char *url);

bool nvs_config_get_ntp_ipv6(bool *prefer);
void nvs_config_set_ntp_ipv6(bool prefer);

bool nvs_config_get_rotation(bool *rotated);
void nvs_config_set_rotation(bool rotated);

bool nvs_config_get_led_brightness(uint8_t *brightness);
void nvs_config_set_led_brightness(uint8_t brightness);

// NTP frequency-correction estimate (ppm * 1000). Persisted across reboots
// so the PI loop doesn't have to re-converge from 0 ppm on every cold boot.
bool nvs_config_get_freq_ppm_x1000(int32_t *value);
void nvs_config_set_freq_ppm_x1000(int32_t value);

#endif
