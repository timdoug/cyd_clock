#include "ntp.h"
#include <stdio.h>
#include <string.h>
#include "chronyd_esp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "util.h"

static const char *TAG = "ntp";

#define CONFIG_STACK_BYTES 4096
// An NTS pool takes two addresses. Each pool record costs about 10 KB of
// heap from the start (a pool holds 2 * maxsources records, unresolved ones
// included), and each NTS-KE session about 31 KB more while it runs; the TLS
// backend runs at most two at once (MAX_SESSIONS in tls_mbedtls.c). With 2,
// the heap low-water mark measured about 54 KB; with chrony's default of 4,
// the 8 records alone took about 79 KB. Plain pools use the default.
#define NTS_MAX_SOURCES 2

// Settings changes are applied by a worker task: ces_add_source() and
// ces_remove_source() wait for chronyd's loop, which can be inside an NTS-KE
// handshake step, so the UI task only records the change. lock guards the
// fields below and is never held across a chronyd command.
static SemaphoreHandle_t lock;
static TaskHandle_t      worker;
static struct {
    bool         started;
    char         server[64];
    ntp_family_t family;
    bool         nts;
    bool         have_group;
    uint32_t     group;
} g;

static void apply_config(void) {
    char server[sizeof(g.server)];
    ntp_family_t family;
    bool nts, had_group;
    uint32_t old_group;

    xSemaphoreTake(lock, portMAX_DELAY);
    str_copy(server, sizeof(server), g.server);
    family = g.family;
    nts = g.nts;
    had_group = g.have_group;
    old_group = g.group;
    g.have_group = false;
    xSemaphoreGive(lock);

    if (had_group) ces_remove_source(old_group);

    if (!ces_valid_host(server)) {
        ESP_LOGW(TAG, "Not a host name or address: \"%s\"", server);
        return;
    }

    char directive[128], maxsources[16] = "";
    if (nts) snprintf(maxsources, sizeof(maxsources), " maxsources %d", NTS_MAX_SOURCES);
    snprintf(directive, sizeof(directive), "pool %s iburst%s%s%s", server, maxsources,
             nts ? " nts" : "",
             family == NTP_FAMILY_IPV4 ? " ipv4" : family == NTP_FAMILY_IPV6 ? " ipv6" : "");
    uint32_t group;
    if (!ces_add_source(directive, &group)) return;
    ESP_LOGI(TAG, "%s", directive);

    xSemaphoreTake(lock, portMAX_DELAY);
    g.have_group = true;
    g.group = group;
    xSemaphoreGive(lock);
}

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        apply_config();
    }
}

void ntp_init(const char *server, ntp_family_t family, bool nts) {
    if (g.started) return;
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    str_copy(g.server, sizeof(g.server), server ? server : "");
    g.family = family;
    g.nts = nts;

    if (!ces_start()) {
        ESP_LOGE(TAG, "Failed to start chronyd");
        return;
    }
    if (xTaskCreatePinnedToCore(worker_task, "ntp", CONFIG_STACK_BYTES, NULL, 4,
                                &worker, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start NTP config task");
        return;
    }
    g.started = true;
    xTaskNotifyGive(worker);
}

void ntp_set_server(const char *server) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = strcmp(g.server, server) != 0;
    if (changed) str_copy(g.server, sizeof(g.server), server);
    xSemaphoreGive(lock);
    if (changed) xTaskNotifyGive(worker);
}

void ntp_set_family(ntp_family_t family) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = g.family != family;
    g.family = family;
    xSemaphoreGive(lock);
    if (changed) xTaskNotifyGive(worker);
}

void ntp_set_nts(bool nts) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = g.nts != nts;
    g.nts = nts;
    xSemaphoreGive(lock);
    if (changed) xTaskNotifyGive(worker);
}
