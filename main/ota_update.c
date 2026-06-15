#include "ota_update.h"
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "ota_update";

static SemaphoreHandle_t status_mutex;
static ota_update_status_t current_status = {
    .state = OTA_UPDATE_IDLE,
    .message = "Idle",
};
static char pending_url[MAX_OTA_URL_LEN];

static void status_set(ota_update_state_t state, const char *message, uint32_t bytes_read) {
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    current_status.state = state;
    str_copy(current_status.message, sizeof(current_status.message), message);
    current_status.bytes_read = bytes_read;
    if (status_mutex) xSemaphoreGive(status_mutex);
}

static void ota_task(void *arg) {
    (void)arg;

    char url[MAX_OTA_URL_LEN];
    str_copy(url, sizeof(url), pending_url);

    ESP_LOGI(TAG, "Starting OTA from %s", url);
    status_set(OTA_UPDATE_RUNNING, "Connecting", 0);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        status_set(OTA_UPDATE_FAILED, esp_err_to_name(err), 0);
        vTaskDelete(NULL);
    }

    status_set(OTA_UPDATE_RUNNING, "Downloading", 0);
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        status_set(OTA_UPDATE_RUNNING, "Downloading",
                   (uint32_t)esp_https_ota_get_image_len_read(handle));
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint32_t bytes_read = (uint32_t)esp_https_ota_get_image_len_read(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        status_set(OTA_UPDATE_FAILED, esp_err_to_name(err), bytes_read);
        vTaskDelete(NULL);
    }

    status_set(OTA_UPDATE_RUNNING, "Verifying", bytes_read);
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        status_set(OTA_UPDATE_FAILED, esp_err_to_name(err), bytes_read);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "OTA complete; restarting");
    status_set(OTA_UPDATE_SUCCESS, "Restarting", bytes_read);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void ota_update_init(void) {
    if (!status_mutex) {
        status_mutex = xSemaphoreCreateMutex();
    }
}

bool ota_update_start(const char *url, char *err_buf, size_t err_len) {
    if (!url || url[0] == '\0') {
        str_copy(err_buf, err_len, "Missing URL");
        status_set(OTA_UPDATE_FAILED, "Missing URL", 0);
        return false;
    }
    if (!wifi_is_connected()) {
        str_copy(err_buf, err_len, "WiFi offline");
        status_set(OTA_UPDATE_FAILED, "WiFi offline", 0);
        return false;
    }
    if (ota_update_is_running()) {
        str_copy(err_buf, err_len, "Already running");
        status_set(OTA_UPDATE_FAILED, "Already running", 0);
        return false;
    }

    str_copy(pending_url, sizeof(pending_url), url);
    status_set(OTA_UPDATE_RUNNING, "Queued", 0);
    BaseType_t ok = xTaskCreate(ota_task, "ota_update", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        status_set(OTA_UPDATE_FAILED, "No memory", 0);
        str_copy(err_buf, err_len, "No memory");
        return false;
    }
    str_copy(err_buf, err_len, "");
    return true;
}

void ota_update_get_status(ota_update_status_t *status) {
    if (!status) return;
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    *status = current_status;
    if (status_mutex) xSemaphoreGive(status_mutex);
}

bool ota_update_is_running(void) {
    ota_update_status_t status;
    ota_update_get_status(&status);
    return status.state == OTA_UPDATE_RUNNING;
}
