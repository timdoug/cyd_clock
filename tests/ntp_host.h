// Host adapter for the production NTP sources. No network or real clock mutations.
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <sys/time.h>

static uint32_t test_ms;
static int64_t wall_us, pending_us;
static int step_count, save_count;
static int32_t saved_freq;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdTICKS_TO_MS(x) (x)
#define xTaskGetTickCount() test_ms
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#include "ntp_internal_host.h"

static int test_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    *tv = tv_from_us(wall_us);
    return 0;
}
static int test_settimeofday(const struct timeval *tv, const void *tz) {
    (void)tz;
    wall_us = tv_to_us(tv);
    pending_us = 0;
    step_count++;
    return 0;
}
static int test_adjtime(const struct timeval *delta, struct timeval *out) {
    if (out) *out = tv_from_us(pending_us);
    if (delta) pending_us = tv_to_us(delta);
    return 0;
}
static void nvs_config_set_freq_ppm_x1000(int32_t freq) { saved_freq = freq; save_count++; }
static uint32_t esp_random(void) { return 123; }
#define gettimeofday test_gettimeofday
#define settimeofday test_settimeofday
#define adjtime test_adjtime

ntp_state_t g;
const char *NTP_TAG = "test";
uint32_t next_global_poll_ms, next_global_poll_cycle_id, last_poll_adjust_cycle_id;
uint32_t last_evict_tick_ms, replace_backoff_until_ms;
bool poll_reset_pending;
void led_set_pps_enabled(bool enabled) { (void)enabled; }
void ntp_wifi_stamp_set_step_in_progress(bool b) { (void)b; }
bool consume_early(early_ring_t *r, uint32_t s, uint32_t f, int64_t *u) {
    (void)r; (void)s; (void)f; (void)u; return false;
}
void shift_early(early_ring_t *r, int64_t d) { (void)r; (void)d; }
early_ring_t *early_t1_ring(void) { return NULL; }
early_ring_t *early_t4_ring(void) { return NULL; }
void maybe_evict_worst_peer(void) {}
void nts_drop_context_and_fallback(void) {}
void nts_request_rekey(void) {}
#ifndef NTP_TEST_RESOLVER
bool sockaddr_matches(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}
#endif
bool ntp_nts_add_ef(uint8_t *b, size_t *l, size_t c, ntp_nts_ctx_t *n, uint8_t u[NTS_UID_LEN]) {
    (void)b; (void)l; (void)c; (void)n; (void)u; return false;
}
bool ntp_nts_check_response(const uint8_t *b, size_t l, ntp_nts_ctx_t *n, const uint8_t u[NTS_UID_LEN]) {
    (void)b; (void)l; (void)n; (void)u; return true;
}
bool ntp_nts_response_uid_matches(const uint8_t *b, size_t l, const uint8_t u[NTS_UID_LEN]) {
    (void)b; (void)l; (void)u; return true;
}

#include "../main/ntp_time.c"
#include "../main/ntp_filter.c"
#include "../main/ntp_discipline.c"
#include "../main/ntp_io.c"

static void reset(void) {
    memset(&g, 0, sizeof(g));
    g.selected_peer = -1;
    g.current_poll_s = 32;
    test_ms = 100000;
    wall_us = (util_anchor_epoch() + 20000000) * 1000000;
    pending_us = 0;
    step_count = 0;
    save_count = 0;
    next_global_poll_cycle_id = 10;
    last_poll_adjust_cycle_id = 0;
}
static ntp_pkt_t packet(int stratum, int64_t t2, int64_t t3) {
    ntp_pkt_t p = {.li_vn_mode = 0x24, .stratum = stratum, .precision = -20};
    p.recv_ts_sec = htonl((uint32_t)(t2 / 1000000 + NTP_EPOCH_OFFSET));
    p.recv_ts_frac = htonl((uint32_t)(((uint64_t)(t2 % 1000000) << 32) / 1000000));
    p.xmt_ts_sec = htonl((uint32_t)(t3 / 1000000 + NTP_EPOCH_OFFSET));
    p.xmt_ts_frac = htonl((uint32_t)(((uint64_t)(t3 % 1000000) << 32) / 1000000));
    return p;
}
static ntp_peer_t *make_peer(int idx) {
    ntp_peer_t *p = &g.peers[idx];
    p->active = true;
    p->stratum = 2;
    p->reach = 255;
    p->t1 = tv_from_us(wall_us);
    p->request_outstanding = true;
    return p;
}
static void sample(ntp_peer_t *p, int slot, int offset, int delay, uint32_t when) {
    p->filter[slot] = (ntp_sample_t){.offset_us=offset, .delay_us=delay,
        .dispersion_us=100, .received_ms=when, .valid=true};
    p->filter_head = slot;
}

