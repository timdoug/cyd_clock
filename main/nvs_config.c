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

static void nvs_commit_and_close(nvs_handle_t handle) {
    esp_err_t err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static bool nvs_set_ok(esp_err_t err, const char *key) {
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "Failed to write NVS key '%s': %s", key, esp_err_to_name(err));
    return false;
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
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        ESP_LOGW(TAG, "No stored WiFi credentials");
        return false;
    }

    size_t ssid_len = WIFI_SSID_BUF_LEN;
    size_t pass_len = MAX_PASSWORD_LEN;

    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, "password", password, &pass_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "Loaded WiFi credentials for SSID: %s", ssid);
    return true;
}

void nvs_config_set_wifi(const char *ssid, const char *password) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_str(handle, "ssid", ssid ? ssid : ""), "ssid") &&
        nvs_set_ok(nvs_set_str(handle, "password", password ? password : ""), "password")) {
        nvs_commit_and_close(handle);
        ESP_LOGI(TAG, "Saved WiFi credentials for SSID: %s", ssid ? ssid : "");
    } else {
        nvs_close(handle);
    }
}


bool nvs_config_get_timezone(char *tz) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    size_t tz_len = MAX_TIMEZONE_LEN;
    esp_err_t err = nvs_get_str(handle, "timezone", tz, &tz_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "Loaded timezone: %s", tz);
    return true;
}

void nvs_config_set_timezone(const char *tz) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_str(handle, "timezone", tz ? tz : ""), "timezone")) {
        nvs_commit_and_close(handle);
        ESP_LOGI(TAG, "Saved timezone: %s", tz ? tz : "");
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_brightness(uint8_t *brightness) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    esp_err_t err = nvs_get_u8(handle, "brightness", brightness);
    nvs_close(handle);
    return err == ESP_OK;
}

void nvs_config_set_brightness(uint8_t brightness) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_u8(handle, "brightness", brightness), "brightness")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_custom_ntp_server(char *server) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    size_t len = MAX_NTP_SERVER_LEN;
    esp_err_t err = nvs_get_str(handle, "ntp_custom", server, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

void nvs_config_set_custom_ntp_server(const char *server) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_str(handle, "ntp_custom", server ? server : ""), "ntp_custom")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_ota_url(char *url) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    size_t len = MAX_OTA_URL_LEN;
    esp_err_t err = nvs_get_str(handle, "ota_url", url, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

void nvs_config_set_ota_url(const char *url) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_str(handle, "ota_url", url ? url : ""), "ota_url")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_ntp_ipv6(bool *prefer) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    uint8_t value;
    esp_err_t err = nvs_get_u8(handle, "ntp_ipv6", &value);
    nvs_close(handle);

    if (err == ESP_OK) {
        *prefer = (value != 0);
        return true;
    }
    return false;
}

void nvs_config_set_ntp_ipv6(bool prefer) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_u8(handle, "ntp_ipv6", prefer ? 1 : 0), "ntp_ipv6")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_rotation(bool *rotated) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    uint8_t value;
    esp_err_t err = nvs_get_u8(handle, "rotation", &value);
    nvs_close(handle);

    if (err == ESP_OK) {
        *rotated = (value != 0);
        return true;
    }
    return false;
}

void nvs_config_set_rotation(bool rotated) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_u8(handle, "rotation", rotated ? 1 : 0), "rotation")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_led_brightness(uint8_t *brightness) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    esp_err_t err = nvs_get_u8(handle, "led_bright", brightness);
    nvs_close(handle);

    return (err == ESP_OK);
}

void nvs_config_set_led_brightness(uint8_t brightness) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    if (nvs_set_ok(nvs_set_u8(handle, "led_bright", brightness), "led_bright")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}

bool nvs_config_get_freq_ppm_x1000(int32_t *value) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }
    esp_err_t err = nvs_get_i32(handle, "freq_ppm", value);
    nvs_close(handle);
    return (err == ESP_OK);
}

void nvs_config_set_freq_ppm_x1000(int32_t value) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;
    if (nvs_set_ok(nvs_set_i32(handle, "freq_ppm", value), "freq_ppm")) {
        nvs_commit_and_close(handle);
    } else {
        nvs_close(handle);
    }
}
