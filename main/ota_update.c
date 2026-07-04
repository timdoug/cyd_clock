#include "ota_update.h"
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
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
static bool pending_force = false;
static bool cancel_requested = false;

typedef struct {
    int last_status_code;
    int redirects;
} ota_http_debug_t;

static void status_set(ota_update_state_t state,
                       const char *message,
                       uint32_t bytes_read,
                       uint32_t total_bytes) {
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    current_status.state = state;
    str_copy(current_status.message, sizeof(current_status.message), message);
    current_status.bytes_read = bytes_read;
    current_status.total_bytes = total_bytes;
    if (status_mutex) xSemaphoreGive(status_mutex);
}

static bool cancel_is_requested(void) {
    bool requested = false;
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    requested = cancel_requested;
    if (status_mutex) xSemaphoreGive(status_mutex);
    return requested;
}

static uint32_t ota_bytes_read(esp_https_ota_handle_t handle) {
    int len = esp_https_ota_get_image_len_read(handle);
    return len > 0 ? (uint32_t)len : 0;
}

static void status_set_err(ota_update_state_t state,
                           const char *phase,
                           esp_err_t err,
                           uint32_t bytes_read,
                           uint32_t total_bytes) {
    char message[64];
    snprintf(message, sizeof(message), "%s: %s", phase, esp_err_to_name(err));
    status_set(state, message, bytes_read, total_bytes);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    ota_http_debug_t *debug = (ota_http_debug_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP connected");
            break;
        case HTTP_EVENT_ON_STATUS_CODE:
            if (evt->data && evt->data_len == sizeof(int)) {
                int status = *(int *)evt->data;
                if (debug) debug->last_status_code = status;
                ESP_LOGI(TAG, "HTTP status %d", status);
            }
            break;
        case HTTP_EVENT_REDIRECT:
            if (debug) debug->redirects++;
            ESP_LOGI(TAG, "HTTP redirect");
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP client error event");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP disconnected");
            break;
        default:
            break;
    }

    return ESP_OK;
}

static void ota_task(void *arg) {
    (void)arg;

    char url[MAX_OTA_URL_LEN];
    str_copy(url, sizeof(url), pending_url);
    bool force = pending_force;

    ESP_LOGI(TAG, "Starting OTA from %s%s", url, force ? " (forced)" : "");
    status_set(OTA_UPDATE_RUNNING, "Connecting", 0, 0);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (running) {
        ESP_LOGI(TAG, "Running partition: %s @ 0x%" PRIx32,
                 running->label, running->address);
    }
    if (next) {
        ESP_LOGI(TAG, "Next OTA partition: %s @ 0x%" PRIx32,
                 next->label, next->address);
    } else {
        ESP_LOGE(TAG, "No next OTA partition available");
    }

    ota_http_debug_t http_debug = {
        .last_status_code = 0,
        .redirects = 0,
    };
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &http_debug,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s status=%d redirects=%d",
                 esp_err_to_name(err), http_debug.last_status_code,
                 http_debug.redirects);
        status_set_err(OTA_UPDATE_FAILED, "Begin", err, 0, 0);
        vTaskDelete(NULL);
    }

    if (cancel_is_requested()) {
        uint32_t bytes_read = ota_bytes_read(handle);
        ESP_LOGI(TAG, "OTA cancelled after begin bytes=%lu",
                 (unsigned long)bytes_read);
        esp_https_ota_abort(handle);
        status_set(OTA_UPDATE_CANCELLED, "Cancelled", 0, 0);
        vTaskDelete(NULL);
    }

    // Identity and version gate. TLS only proves we are talking to the
    // configured host; the app descriptor proves the served file is this
    // project's firmware for THIS board (project_name carries the board
    // suffix - a wrong asset URL used to flash blind, including the other
    // board's image) and lets us skip re-flashing the version that is
    // already running.
    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        status_set_err(OTA_UPDATE_FAILED, "Describe", err, 0, 0);
        vTaskDelete(NULL);
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    if (strncmp(new_desc.project_name, cur->project_name,
                sizeof(new_desc.project_name)) != 0) {
        ESP_LOGE(TAG, "OTA image is '%.*s', not '%s'; refusing",
                 (int)sizeof(new_desc.project_name), new_desc.project_name,
                 cur->project_name);
        esp_https_ota_abort(handle);
        status_set(OTA_UPDATE_FAILED, "Wrong image", 0, 0);
        vTaskDelete(NULL);
    }
    if (!force &&
        strncmp(new_desc.version, cur->version, sizeof(new_desc.version)) == 0) {
        ESP_LOGI(TAG, "OTA image version '%s' is already running", cur->version);
        esp_https_ota_abort(handle);
        // The UI turns this state into "tap Update again to reflash anyway"
        // (a deliberate repair path for a suspect flash).
        status_set(OTA_UPDATE_SAME_VERSION, "Same version; tap to force", 0, 0);
        vTaskDelete(NULL);
    }

    int image_size = esp_https_ota_get_image_size(handle);
    uint32_t total_bytes = image_size > 0 ? (uint32_t)image_size : 0;

    status_set(OTA_UPDATE_RUNNING, "Downloading", 0, total_bytes);
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        uint32_t bytes_read = ota_bytes_read(handle);
        if (cancel_is_requested()) {
            ESP_LOGI(TAG, "OTA cancelled during download bytes=%lu",
                     (unsigned long)bytes_read);
            esp_https_ota_abort(handle);
            status_set(OTA_UPDATE_CANCELLED, "Cancelled", 0, 0);
            vTaskDelete(NULL);
        }
        status_set(OTA_UPDATE_RUNNING, "Downloading", bytes_read, total_bytes);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint32_t bytes_read = ota_bytes_read(handle);
    if (cancel_is_requested()) {
        ESP_LOGI(TAG, "OTA cancelled before finish bytes=%lu",
                 (unsigned long)bytes_read);
        esp_https_ota_abort(handle);
        status_set(OTA_UPDATE_CANCELLED, "Cancelled", 0, 0);
        vTaskDelete(NULL);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s bytes=%lu status=%d redirects=%d",
                 esp_err_to_name(err), (unsigned long)bytes_read,
                 http_debug.last_status_code, http_debug.redirects);
        esp_https_ota_abort(handle);
        status_set_err(OTA_UPDATE_FAILED, "Download", err, bytes_read, total_bytes);
        vTaskDelete(NULL);
    }

    status_set(OTA_UPDATE_RUNNING, "Verifying", bytes_read, total_bytes);
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s bytes=%lu status=%d redirects=%d",
                 esp_err_to_name(err), (unsigned long)bytes_read,
                 http_debug.last_status_code, http_debug.redirects);
        status_set_err(OTA_UPDATE_FAILED, "Finish", err, bytes_read, total_bytes);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "OTA complete; restarting");
    status_set(OTA_UPDATE_SUCCESS, "Restarting", bytes_read, total_bytes);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void ota_update_init(void) {
    if (!status_mutex) {
        status_mutex = xSemaphoreCreateMutex();
    }
}

void ota_update_mark_boot_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    if (running &&
        esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Marked OTA image valid: %s", esp_err_to_name(err));
    }
}

bool ota_update_start(const char *url, bool force, char *err_buf, size_t err_len) {
    // Check-and-claim under the mutex, and WITHOUT touching current_status
    // on refusal: overwriting the RUNNING state the in-flight task owns
    // would make is_running()/cancel() no-ops and let a second start pass
    // the guard and write the same partition concurrently. The precondition
    // refusals (missing URL, offline) write FAILED only after confirming no
    // task is RUNNING, and inline the write while holding the mutex - so they
    // can never stomp a running task's status.
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    if (current_status.state == OTA_UPDATE_RUNNING) {
        if (status_mutex) xSemaphoreGive(status_mutex);
        str_copy(err_buf, err_len, "Already running");
        return false;
    }
    const char *precheck_err = NULL;
    if (!url || url[0] == '\0') {
        precheck_err = "Missing URL";
    } else if (!wifi_is_connected()) {
        precheck_err = "Wi-Fi offline";
    }
    if (precheck_err) {
        current_status.state = OTA_UPDATE_FAILED;
        str_copy(current_status.message, sizeof(current_status.message), precheck_err);
        current_status.bytes_read = 0;
        current_status.total_bytes = 0;
        if (status_mutex) xSemaphoreGive(status_mutex);
        str_copy(err_buf, err_len, precheck_err);
        return false;
    }
    current_status.state = OTA_UPDATE_RUNNING;
    str_copy(current_status.message, sizeof(current_status.message), "Queued");
    current_status.bytes_read = 0;
    current_status.total_bytes = 0;
    cancel_requested = false;
    if (status_mutex) xSemaphoreGive(status_mutex);

    str_copy(pending_url, sizeof(pending_url), url);
    pending_force = force;
    // Pin to core 0 (network core): the HTTPS download + flash writes must not
    // compete with the OTA progress screen the main task renders on core 1.
    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota_update", 8192, NULL,
                                            tskIDLE_PRIORITY + 1, NULL, 0);
    if (ok != pdPASS) {
        status_set(OTA_UPDATE_FAILED, "No memory", 0, 0);
        str_copy(err_buf, err_len, "No memory");
        return false;
    }
    str_copy(err_buf, err_len, "");
    return true;
}

bool ota_update_cancel(void) {
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    bool running = current_status.state == OTA_UPDATE_RUNNING;
    if (running) {
        cancel_requested = true;
        str_copy(current_status.message, sizeof(current_status.message), "Cancelling");
    }
    if (status_mutex) xSemaphoreGive(status_mutex);
    return running;
}

void ota_update_get_status(ota_update_status_t *status) {
    if (!status) return;
    if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY);
    *status = current_status;
    if (status_mutex) xSemaphoreGive(status_mutex);
}
