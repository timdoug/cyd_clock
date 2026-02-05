#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

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
    nvs_commit(handle);
    nvs_close(handle);
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

    size_t ssid_len = MAX_SSID_LEN;
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

    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "password", password));
    nvs_commit_and_close(handle);
    ESP_LOGI(TAG, "Saved WiFi credentials for SSID: %s", ssid);
}

void nvs_config_clear_wifi(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;

    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "password");
    nvs_commit_and_close(handle);
    ESP_LOGI(TAG, "Cleared WiFi credentials");
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

    ESP_ERROR_CHECK(nvs_set_str(handle, "timezone", tz));
    nvs_commit_and_close(handle);
    ESP_LOGI(TAG, "Saved timezone: %s", tz);
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

    ESP_ERROR_CHECK(nvs_set_u8(handle, "brightness", brightness));
    nvs_commit_and_close(handle);
}

bool nvs_config_get_ntp_interval(uint32_t *interval) {
    nvs_handle_t handle;
    if (!nvs_open_read(&handle)) {
        return false;
    }

    esp_err_t err = nvs_get_u32(handle, "ntp_interval", interval);
    nvs_close(handle);
    return err == ESP_OK;
}

void nvs_config_set_ntp_interval(uint32_t interval) {
    nvs_handle_t handle;
    if (!nvs_open_write(&handle)) return;

    ESP_ERROR_CHECK(nvs_set_u32(handle, "ntp_interval", interval));
    nvs_commit_and_close(handle);
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

    ESP_ERROR_CHECK(nvs_set_str(handle, "ntp_custom", server));
    nvs_commit_and_close(handle);
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

    ESP_ERROR_CHECK(nvs_set_u8(handle, "rotation", rotated ? 1 : 0));
    nvs_commit_and_close(handle);
}
