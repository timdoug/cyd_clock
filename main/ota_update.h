#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_OTA_URL "https://github.com/timdoug/cyd_clock/releases/latest/download/cyd_clock.bin"

typedef enum {
    OTA_UPDATE_IDLE,
    OTA_UPDATE_RUNNING,
    OTA_UPDATE_SUCCESS,
    OTA_UPDATE_FAILED,
} ota_update_state_t;

typedef struct {
    ota_update_state_t state;
    char message[64];
    uint32_t bytes_read;
} ota_update_status_t;

void ota_update_init(void);
bool ota_update_start(const char *url, char *err_buf, size_t err_len);
void ota_update_get_status(ota_update_status_t *status);
bool ota_update_is_running(void);

#endif // OTA_UPDATE_H
