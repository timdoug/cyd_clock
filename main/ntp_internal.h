#ifndef CYD_NTP_INTERNAL_H
#define CYD_NTP_INTERNAL_H

#include "ntp.h"
#include "ntp_nts.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_config.h"
#include "util.h"

extern const char *NTP_TAG;
#define TAG NTP_TAG

#define NTP_PORT             123
#define NTP_VERSION          4
#define NTP_MODE_CLIENT      3
#define NTP_MODE_SERVER      4
#define NTP_PKT_SIZE         48
#define NTP_EPOCH_OFFSET     2208988800UL  // 1900 -> 1970 in seconds
// NTP wire timestamps carry seconds mod 2^32 - one ~136-year "era". This
// fold constant maps a wire value to Unix seconds mod 2^32 with a single
// wrapping add: era 0 (1900-2036) wants sec - NTP_EPOCH_OFFSET and era 1+
// wants sec + (2^32 - NTP_EPOCH_OFFSET), which mod 2^32 are the same
// operation. ntp_to_tv then resolves the absolute era against the anchor
// year (util.h).
#define NTP_UNIX_FOLD        2085978496UL  // 2^32 - NTP_EPOCH_OFFSET
#define NTP_FILTER_SIZE      8

// Optimize for the common case: public NTP over WiFi, where route/server
// bias and queueing noise dominate. Quiet LAN references still benefit from
// the same low-delay sample selection and conservative drift learning.
#define ROBUST_SAMPLE_MAX_AGE_S 3600
#define ROBUST_COMBINE_SPREAD_US 2000
#define ROBUST_FREQ_MAX_JITTER_US 2500
#define ROBUST_FREQ_MAX_OFFSET_US 8000

#define MIN_POLL_S           32
#define MAX_POLL_S           512
#define RESPONSE_TIMEOUT_MS  2500
#define STEP_THRESHOLD_US    (128LL * 1000)
#define PANIC_THRESHOLD_S    1000
#define PANIC_AGREE_US       500000LL  // panic offsets within 0.5 s corroborate
#define IDLE_WAKE_MS         5000
#define NEW_PEER_HIGHLIGHT_MS 10000

#define MAX_FREQ_PPM_X1000   500000
// Each discipline corrects a fraction poll/TAU of the frequency error, so the
// loop has a ~TAU time constant at any poll interval.
#define FREQ_TAU_S           4096
#define MAX_FREQ_STEP_PPB    1000
#define FREQ_MAX_OFFSET_US   25000

// Per RFC 5905: each sample's dispersion grows linearly with time at this rate.
// Makes root_dispersion honestly track uncertainty between polls.
#define PHI_US_PER_SEC       15

// Our local clock precision as the NTP log2(seconds) field. -18 ~ 4 us, which
// is conservative for the ESP32's microsecond-granular gettimeofday. We both
// advertise this to peers (pkt.precision below) AND use it as one of the
// inputs to each sample's per-sample dispersion, so a single constant keeps
// the advertised and internally-accounted precision consistent.
#define LOCAL_PRECISION      (-18)

// Floor on per-sample dispersion - guards against a server that lies about
// its precision (or a stratum-1 that legitimately advertises sub-us) driving
// the sample's uncertainty estimate unreasonably low. 100 us is about what
// WiFi + LAN + ESP32 capture jitter can actually deliver in the best case.
#define SAMPLE_DISP_FLOOR_US 100

// Shared "next poll tick" all peers align to. Without this, each peer scheduled
// independently from its own response time, and RTT / timeout / swap events
// caused their poll phases to drift apart - on a 32 s interval you could see
// up to 20 s of age difference between peers. With alignment, every peer fires
// within a known splay window of the same shared tick.
// Total splay window across all peers, chosen so each peer hits a different
// moment of the WiFi / pool-peer / upstream-router timeline (a transient bad
// second on one peer's slot doesn't poison the others' samples). Per-peer
// slot is SPLAY_WINDOW_MS / NTP_MAX_PEERS, with random jitter inside the
// slot so we don't phase-lock with any periodic network event.
//
// Sized comfortably under (MIN_POLL_S - RESPONSE_TIMEOUT - margin) so even
// the last-scheduled peer's response lands before the discipline threshold.
#define SPLAY_WINDOW_MS  8000

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    int8_t   poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t recv_ts_sec;
    uint32_t recv_ts_frac;
    uint32_t xmt_ts_sec;
    uint32_t xmt_ts_frac;
} ntp_pkt_t;

typedef struct {
    int32_t  offset_us;
    int32_t  delay_us;
    int32_t  dispersion_us;
    uint32_t received_ms;
    bool     valid;
} ntp_sample_t;

typedef struct {
    bool     active;
    struct   sockaddr_storage addr;
    socklen_t addr_len;
    char     addr_str[46];
    uint8_t  stratum;
    uint8_t  reach;
    int8_t   precision;
    uint32_t root_delay_raw;
    uint32_t root_dispersion_raw;

    ntp_sample_t filter[NTP_FILTER_SIZE];
    int      filter_head;

    int32_t  best_offset_us;
    int32_t  best_delay_us;
    int32_t  jitter_us;
    int32_t  dispersion_us;
    uint32_t best_sample_ms;
    uint32_t last_response_ms;

    struct timeval t1;
    uint32_t xmt_sec_net;
    uint32_t xmt_frac_net;
    bool     request_outstanding;
    uint32_t request_sent_ms;

    // Wave bookkeeping for "discipline only when all peers have settled this
    // wave". next_poll_cycle_id is assigned when the next poll is scheduled,
    // then copied into cycle_id_when_sent at send time. After the peer
    // responds, times out, or the send fails, last_settle_cycle_id is set to
    // the same tag. Matching sent/settled tags mean "this peer's most recent
    // request is resolved", regardless of which way it resolved.
    uint32_t next_poll_cycle_id;
    uint32_t cycle_id_when_sent;
    uint32_t last_settle_cycle_id;

    uint32_t next_poll_ms;
    uint32_t kod_until_ms;
    uint8_t  consecutive_misses;
    uint8_t  falseticker_runs;
    uint8_t  jittery_runs;
    uint32_t quality_cycle_id;     // wave id of the last falseticker/jittery increment:
                                   // select_system_peer runs once per RESPONSE (up to
                                   // 4x per wave), but these counters must move at most
                                   // once per poll cycle or the eviction thresholds
                                   // fire ~4x faster than designed
    uint8_t  panic_runs;
    int64_t  panic_offset_us;
    uint32_t fresh_until_ms;

    // NTS (RFC 8915): when nts is set this peer authenticates every exchange
    // against the shared g.nts context. uid holds the Unique Identifier sent
    // with the outstanding request, echoed by the server for matching.
    bool     nts;
    uint8_t  uid[NTS_UID_LEN];
} ntp_peer_t;

typedef struct {
    char     server[64];
    bool     prefer_ipv6;
    nts_mode_t nts_mode;
    uint32_t current_poll_s;

    ntp_peer_t peers[NTP_MAX_PEERS];
    int      selected_peer;
    uint8_t  stratum;

    // Shared NTS context for the configured host: keys + cookie pool from one
    // KE handshake, drawn on by all of the host's peers. nts_rebind is set by
    // the KE task when a fresh context is ready so the NTP task re-binds peers.
    ntp_nts_ctx_t nts;
    bool          nts_rebind;

    bool     first_sync_done;
    time_t   last_sync_time;
    int64_t  last_offset_us;
    uint32_t sync_count;
    uint32_t sync_start_ms;

    int32_t  system_jitter_us;
    int32_t  root_delay_us;
    int32_t  root_dispersion_us;
    int32_t  combined_offset_us;
    bool     select_spread_wide;
    int32_t  freq_ppm_x1000;
    bool     freq_loaded_from_nvs;
    bool     freq_learned_this_session;
    uint32_t last_freq_apply_ms;
    int64_t  freq_apply_residual;
    uint32_t last_freq_sample_ms;
    int32_t  freq_jitter_floor;    // adaptive estimate of the clean-wave system-jitter
                                   // level; freq learns only from waves at or near it
    uint32_t last_discipline_ms;
    uint32_t last_discipline_poll_s;
    uint32_t last_any_response_ms;
    int8_t   poll_adjust;
    int8_t   poll_bad_sign;

    int      sock4;
    int      sock6;

    // Loopback UDP socket used as a self-pipe to break the select() sleep
    // when config changes from another task. Any byte received is drained.
    int      wake_sock;
    uint16_t wake_port;

    TaskHandle_t task;
    SemaphoreHandle_t lock;
    bool     running;
    bool     force_sync;
    bool     dirty_config;
} ntp_state_t;

typedef struct early_ring early_ring_t;

extern ntp_state_t g;
extern uint32_t next_global_poll_ms;
extern uint32_t next_global_poll_cycle_id;
extern uint32_t last_poll_adjust_cycle_id;
extern uint32_t last_evict_tick_ms;

static inline int32_t precision_to_us(int8_t precision) {
    if (precision >= 0) {
        if (precision >= 12) return INT32_MAX;
        int64_t us = 1000000LL << precision;
        return us > INT32_MAX ? INT32_MAX : (int32_t)us;
    }
    int shift = -(int)precision;
    if (shift >= 20) return 1;
    return 1000000 >> shift;
}

static inline uint32_t mono_ms(void) {
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

static inline void lock_take(void)  { xSemaphoreTake(g.lock, portMAX_DELAY); }
static inline void lock_give(void)  { xSemaphoreGive(g.lock); }

void ntp_to_tv(uint32_t sec, uint32_t frac, struct timeval *tv);
static inline int64_t tv_diff_us(const struct timeval *a, const struct timeval *b) {
    return ((int64_t)a->tv_sec - b->tv_sec) * 1000000LL + (a->tv_usec - b->tv_usec);
}

static inline struct timeval tv_from_us(int64_t total_us) {
    struct timeval tv = {
        .tv_sec  = (time_t)(total_us / 1000000),
        .tv_usec = (suseconds_t)(total_us % 1000000),
    };
    if (tv.tv_usec < 0) {
        tv.tv_sec--;
        tv.tv_usec += 1000000;
    }
    return tv;
}

static inline int64_t tv_to_us(const struct timeval *tv) {
    return (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
}

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int32_t fp1616_to_us(uint32_t raw) {
    uint64_t us = ((uint64_t)raw * 1000000ULL) >> 16;
    if (us > 0x7FFFFFFFULL) us = 0x7FFFFFFFULL;
    return (int32_t)us;
}

void step_clock(int64_t step_us);

bool sockaddr_matches(const struct sockaddr_storage *a, const struct sockaddr_storage *b);
int resolve_peers(void);
void maybe_evict_worst_peer(void);

// NTS-KE orchestration (ntp_peers.c); the KE handshake runs in its own
// large-stack task.
void nts_start_ke_if_needed(void);
void nts_rebind_peers(void);
void nts_drop_context_and_fallback(void);

bool open_sockets(void);
void close_sockets(void);
void close_wake_sock(void);
void open_wake_sock(void);
void wake_task(void);
void drain_wake_sock(void);
void schedule_after_request(ntp_peer_t *p);
bool send_request(ntp_peer_t *p);
void handle_socket_readable(int sock, const struct timeval *t4);

void update_peer_filter(ntp_peer_t *p);
int32_t aged_peer_dispersion_us(const ntp_peer_t *p, uint32_t now_ms);
void shift_filters(int32_t offset_us);
void select_system_peer(void);

bool consume_early(early_ring_t *ring, uint32_t sec, uint32_t frac, int64_t *us);
void shift_early(early_ring_t *ring, int64_t delta_us);
early_ring_t *early_t1_ring(void);
early_ring_t *early_t4_ring(void);

void adaptive_poll_update_once(uint32_t cycle_id);
void apply_freq_correction(void);
bool try_discipline(uint32_t settled_cycle_id);

#endif
