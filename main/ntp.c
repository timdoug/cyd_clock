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
// backend runs them one at a time (MAX_SESSIONS in tls_mbedtls.c). With
// chrony's default of 4, the 8 records alone took about 79 KB. Plain pools
// use the default.
#define NTS_MAX_SOURCES 2

// Settings changes and network events are applied by a worker task:
// chronyd commands wait for chronyd's loop, which can be inside an NTS-KE
// handshake step, so the UI task and the Wi-Fi event handler only record
// the change. lock guards the fields below and is never held across a
// chronyd command.
static SemaphoreHandle_t lock;
static TaskHandle_t      worker;
static struct {
    bool         started;
    bool         config_pending;
    bool         network_pending;
    bool         network_up;
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

        xSemaphoreTake(lock, portMAX_DELAY);
        bool config = g.config_pending, network = g.network_pending;
        g.config_pending = g.network_pending = false;
        bool up = g.network_up;
        xSemaphoreGive(lock);

        if (config) apply_config();
        // A new source starts online; take it offline if the network is down.
        if (network || (config && !up)) ces_set_online(up);
    }
}

static void request(bool *pending) {
    *pending = true;
    xTaskNotifyGive(worker);
}

void ntp_init(const char *server, ntp_family_t family, bool nts) {
    if (g.started) return;
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    str_copy(g.server, sizeof(g.server), server ? server : "");
    g.family = family;
    g.nts = nts;
    g.network_up = true;  // started once the station has an address
    g.config_pending = true;

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

void ntp_network_changed(bool up) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    g.network_up = up;
    request(&g.network_pending);
    xSemaphoreGive(lock);
}

void ntp_set_server(const char *server) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = strcmp(g.server, server) != 0;
    if (changed) {
        str_copy(g.server, sizeof(g.server), server);
        request(&g.config_pending);
    }
    xSemaphoreGive(lock);
}

void ntp_set_family(ntp_family_t family) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (g.family != family) {
        g.family = family;
        request(&g.config_pending);
    }
    xSemaphoreGive(lock);
}

void ntp_set_nts(bool nts) {
    if (!g.started) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (g.nts != nts) {
        g.nts = nts;
        request(&g.config_pending);
    }
    xSemaphoreGive(lock);
}
