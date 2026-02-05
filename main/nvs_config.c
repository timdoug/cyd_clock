#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_config";
static const char *NVS_NAMESPACE = "cyd_clock";

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
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No stored WiFi credentials");
        return false;
    }

    size_t ssid_len = MAX_SSID_LEN;
    size_t pass_len = MAX_PASSWORD_LEN;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, "password", password, &pass_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded WiFi credentials for SSID: %s", ssid);
    return true;
}

void nvs_config_set_wifi(const char *ssid, const char *password) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));

    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "password", password));
    ESP_ERROR_CHECK(nvs_commit(handle));

    nvs_close(handle);
    ESP_LOGI(TAG, "Saved WiFi credentials for SSID: %s", ssid);
}

void nvs_config_clear_wifi(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;

    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "password");
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Cleared WiFi credentials");
}

bool nvs_config_get_timezone(char *tz) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t tz_len = MAX_TIMEZONE_LEN;
    err = nvs_get_str(handle, "timezone", tz, &tz_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "Loaded timezone: %s", tz);
    return true;
}

void nvs_config_set_timezone(const char *tz) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));

    ESP_ERROR_CHECK(nvs_set_str(handle, "timezone", tz));
    ESP_ERROR_CHECK(nvs_commit(handle));

    nvs_close(handle);
    ESP_LOGI(TAG, "Saved timezone: %s", tz);
}

bool nvs_config_get_brightness(uint8_t *brightness) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_u8(handle, "brightness", brightness);
    nvs_close(handle);

    return err == ESP_OK;
}

void nvs_config_set_brightness(uint8_t brightness) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));

    ESP_ERROR_CHECK(nvs_set_u8(handle, "brightness", brightness));
    ESP_ERROR_CHECK(nvs_commit(handle));

    nvs_close(handle);
}
