#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Buffer sizes including the terminating NUL. 802.11 SSIDs are up to 32 bytes.
#define WIFI_SSID_BUF_LEN 33
// WPA2 passphrases are 8..63 characters, which fit. Note the buffer includes
// the NUL, so only 63 usable bytes: a raw 64-hex-char PSK (the pre-shared key
// entered directly instead of a passphrase) cannot be stored. That is a rare
// configuration and expanding the buffer to 65 to allow it is left out here.
#define MAX_PASSWORD_LEN 64
#define MAX_TIMEZONE_LEN 48

void nvs_config_init(void);

bool nvs_config_get_wifi(char *ssid, char *password);
void nvs_config_set_wifi(const char *ssid, const char *password);

bool nvs_config_get_timezone(char *tz);
void nvs_config_set_timezone(const char *tz);

// Display name ("Region/City (UTC+X, DST)") of the picked zone. The POSIX
// string above is what actually takes effect; the name only disambiguates
// zones that share one POSIX string (e.g. Paris and Ceuta) so the picker
// can restore the city the user actually chose.
#define MAX_TIMEZONE_NAME_LEN 64
bool nvs_config_get_timezone_name(char *name);
void nvs_config_set_timezone_name(const char *name);

bool nvs_config_get_brightness(uint8_t *brightness);
void nvs_config_set_brightness(uint8_t brightness);

#define MAX_NTP_SERVER_LEN 64
bool nvs_config_get_custom_ntp_server(char *server);
void nvs_config_set_custom_ntp_server(const char *server);

#define MAX_OTA_URL_LEN 256
bool nvs_config_get_ota_url(char *url);
void nvs_config_set_ota_url(const char *url);

// ntp_family_t (ntp.h) and chrony's "nts" source option
bool nvs_config_get_ntp_family(uint8_t *family);
void nvs_config_set_ntp_family(uint8_t family);

bool nvs_config_get_nts(bool *nts);
void nvs_config_set_nts(bool nts);

bool nvs_config_get_rotation(bool *rotated);
void nvs_config_set_rotation(bool rotated);

// UI language, persisted as the language code string ("de", "zh_hant").
#define NVS_LANG_CODE_LEN 16
bool nvs_config_get_language(char *code, size_t len);
void nvs_config_set_language(const char *code);

bool nvs_config_get_led_brightness(uint8_t *brightness);
void nvs_config_set_led_brightness(uint8_t brightness);

#endif
