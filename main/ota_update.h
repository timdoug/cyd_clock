#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Default OTA source: the latest release asset for whichever board this was
// built for. The asset name comes from the board profile, so each build points
// at its own binary.
#define DEFAULT_OTA_URL \
    "https://github.com/timdoug/cyd_clock/releases/latest/download/" BOARD_OTA_ASSET

typedef enum {
    OTA_UPDATE_IDLE,
    OTA_UPDATE_RUNNING,
    OTA_UPDATE_SUCCESS,
    OTA_UPDATE_FAILED,
    OTA_UPDATE_CANCELLED,
    // The served image is the version already running. Not an error: the
    // UI offers a forced reflash (e.g. to repair a suspect flash) by
    // starting again with force = true.
    OTA_UPDATE_SAME_VERSION,
} ota_update_state_t;

typedef struct {
    ota_update_state_t state;
    char message[64];
    uint32_t bytes_read;
    uint32_t total_bytes;
} ota_update_status_t;

void ota_update_init(void);

// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, a freshly OTA'd image boots
// as PENDING_VERIFY and the bootloader rolls back to the previous slot if
// the device reboots before this is called. Call once the app has reached a
// known-good state (main loop running); no-op on non-OTA boots.
void ota_update_mark_boot_valid(void);

// force skips the same-version check so an image identical to the running
// one can be reflashed anyway; the project-name identity check always
// applies.
bool ota_update_start(const char *url, bool force, char *err_buf, size_t err_len);
bool ota_update_cancel(void);
void ota_update_get_status(ota_update_status_t *status);
bool ota_update_is_running(void);

#endif
