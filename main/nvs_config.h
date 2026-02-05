#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_SSID_LEN     32
#define MAX_PASSWORD_LEN 64
#define MAX_TIMEZONE_LEN 48

// Initialize NVS storage
void nvs_config_init(void);

// WiFi credentials
bool nvs_config_get_wifi(char *ssid, char *password);
void nvs_config_set_wifi(const char *ssid, const char *password);
void nvs_config_clear_wifi(void);

// Timezone
bool nvs_config_get_timezone(char *tz);
void nvs_config_set_timezone(const char *tz);

// Brightness
bool nvs_config_get_brightness(uint8_t *brightness);
void nvs_config_set_brightness(uint8_t brightness);

#endif // NVS_CONFIG_H
