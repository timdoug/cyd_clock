#include "nvs_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_config";
static const char *NVS_NAMESPACE = "cyd_clock";

// Helper functions to reduce boilerplate
static bool nvs_open_read(nvs_handle_t *handle) {
    return nvs_open(NVS_NAMESPACE, NVS_READONLY, handle) == ESP_OK;
}

static bool nvs_open_write(nvs_handle_t *handle) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Commit and close. Returns the commit result so setters only report success
// on ESP_OK rather than logging "Saved ..." unconditionally.
static esp_err_t nvs_commit_and_close(nvs_handle_t handle) {
    esp_err_t err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    return err;
}

static bool nvs_set_ok(esp_err_t err, const char *key) {
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "Failed to write NVS key '%s': %s", key, esp_err_to_name(err));
    return false;
}

// Generic single-key getters: open readonly, read one key, close. Return true
// only on a hit. `len` is the destination buffer size (bytes) for strings.
static bool nvs_get_str_key(const char *key, char *out, size_t len) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) return false;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_get_u8_key(const char *key, uint8_t *out) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) return false;
    esp_err_t err = nvs_get_u8(handle, key, out);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_get_i32_key(const char *key, int32_t *out) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) return false;
    esp_err_t err = nvs_get_i32(handle, key, out);
    nvs_close(handle);
    return err == ESP_OK;
}

// Generic single-key setters: open r/w, write one key, commit+close. Return
// ESP_OK only if the value was both written and committed. Write and commit
// failures are logged inside the helpers, so silent loss is impossible.
static esp_err_t nvs_set_str_key(const char *key, const char *value) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return ESP_FAIL;
    if (!nvs_set_ok(nvs_set_str(handle, key, value ? value : ""), key)) {
        nvs_close(handle);
        return ESP_FAIL;
    }
    return nvs_commit_and_close(handle);
}

static esp_err_t nvs_set_u8_key(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return ESP_FAIL;
    if (!nvs_set_ok(nvs_set_u8(handle, key, value), key)) {
        nvs_close(handle);
        return ESP_FAIL;
    }
    return nvs_commit_and_close(handle);
}

static esp_err_t nvs_set_i32_key(const char *key, int32_t value) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return ESP_FAIL;
    if (!nvs_set_ok(nvs_set_i32(handle, key, value), key)) {
        nvs_close(handle);
        return ESP_FAIL;
    }
    return nvs_commit_and_close(handle);
}

void nvs_config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");
}

bool nvs_config_get_wifi(char *ssid, char *password) {
    // Two keys under one handle; kept custom (the generic single-key helpers
    // would open the namespace twice) and it carries its own logging.
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        ESP_LOGW(TAG, "No stored WiFi credentials");
        return false;
    }

    size_t ssid_len = WIFI_SSID_BUF_LEN;
    size_t pass_len = MAX_PASSWORD_LEN;

    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &pass_len);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "Loaded WiFi credentials for SSID: %s", ssid);
    return true;
}

void nvs_config_set_wifi(const char *ssid, const char *password) {
    // Two keys under one handle; kept custom. Log success only once both
    // writes committed.
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    esp_err_t err = ESP_FAIL;
    if (nvs_set_ok(nvs_set_str(handle, "ssid", ssid ? ssid : ""), "ssid") &&
        nvs_set_ok(nvs_set_str(handle, "password", password ? password : ""), "password")) {
        err = nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved WiFi credentials for SSID: %s", ssid ? ssid : "");
    } else {
        ESP_LOGE(TAG, "Failed to save WiFi credentials for SSID: %s", ssid ? ssid : "");
    }
}

bool nvs_config_get_timezone(char *tz) {
    if (!nvs_get_str_key("timezone", tz, MAX_TIMEZONE_LEN)) return false;
    ESP_LOGI(TAG, "Loaded timezone: %s", tz);
    return true;
}

void nvs_config_set_timezone(const char *tz) {
    if (nvs_set_str_key("timezone", tz) == ESP_OK) {
        ESP_LOGI(TAG, "Saved timezone: %s", tz ? tz : "");
    } else {
        ESP_LOGE(TAG, "Failed to save timezone: %s", tz ? tz : "");
    }
}

bool nvs_config_get_timezone_name(char *name) {
    return nvs_get_str_key("tz_name", name, MAX_TIMEZONE_NAME_LEN);
}

void nvs_config_set_timezone_name(const char *name) {
    nvs_set_str_key("tz_name", name);
}

bool nvs_config_get_brightness(uint8_t *brightness) {
    return nvs_get_u8_key("brightness", brightness);
}

void nvs_config_set_brightness(uint8_t brightness) {
    nvs_set_u8_key("brightness", brightness);
}

bool nvs_config_get_custom_ntp_server(char *server) {
    return nvs_get_str_key("ntp_custom", server, MAX_NTP_SERVER_LEN);
}

void nvs_config_set_custom_ntp_server(const char *server) {
    nvs_set_str_key("ntp_custom", server);
}

bool nvs_config_get_ota_url(char *url) {
    return nvs_get_str_key("ota_url", url, MAX_OTA_URL_LEN);
}

void nvs_config_set_ota_url(const char *url) {
    nvs_set_str_key("ota_url", url);
}

bool nvs_config_get_ntp_ipv6(bool *prefer) {
    uint8_t value;
    if (!nvs_get_u8_key("ntp_ipv6", &value)) return false;
    *prefer = (value != 0);
    return true;
}

void nvs_config_set_ntp_ipv6(bool prefer) {
    nvs_set_u8_key("ntp_ipv6", prefer ? 1 : 0);
}

bool nvs_config_get_nts_mode(uint8_t *mode) {
    return nvs_get_u8_key("nts_mode", mode);
}

void nvs_config_set_nts_mode(uint8_t mode) {
    nvs_set_u8_key("nts_mode", mode);
}

bool nvs_config_get_rotation(bool *rotated) {
    uint8_t value;
    if (!nvs_get_u8_key("rotation", &value)) return false;
    *rotated = (value != 0);
    return true;
}

void nvs_config_set_rotation(bool rotated) {
    nvs_set_u8_key("rotation", rotated ? 1 : 0);
}

// The language setting is persisted as the BCP 47-style code string
// ("de", "zh_hant"), so lang_t values carry no meaning across builds
// and main/i18n/lang_list.inc can stay sorted. Unknown codes fall back
// to English at the call site.
bool nvs_config_get_language(char *code, size_t len) {
    return nvs_get_str_key("lang", code, len);
}

void nvs_config_set_language(const char *code) {
    nvs_set_str_key("lang", code);
}

bool nvs_config_get_led_brightness(uint8_t *brightness) {
    return nvs_get_u8_key("led_bright", brightness);
}

void nvs_config_set_led_brightness(uint8_t brightness) {
    nvs_set_u8_key("led_bright", brightness);
}

bool nvs_config_get_freq_ppm_x1000(int32_t *value) {
    return nvs_get_i32_key("freq_ppm", value);
}

void nvs_config_set_freq_ppm_x1000(int32_t value) {
    nvs_set_i32_key("freq_ppm", value);
}
