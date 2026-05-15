#include "ntp.h"
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
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_config.h"

static const char *TAG = "ntp";

// Protocol
#define NTP_PORT             123
#define NTP_VERSION          4
#define NTP_MODE_CLIENT      3
#define NTP_MODE_SERVER      4
#define NTP_PKT_SIZE         48
#define NTP_EPOCH_OFFSET     2208988800UL  // 1900 -> 1970 in seconds
// NTP era 1 starts 2036-02-07 when the 32-bit NTP seconds counter wraps.
// Unix time at that moment: 2^32 - NTP_EPOCH_OFFSET = 2085978496.
#define NTP_ERA1_UNIX_OFFSET 2085978496UL
#define NTP_FILTER_SIZE      8

// Timing
#define MIN_POLL_S           32
#define MAX_POLL_S           1024    // hard ceiling on adaptive poll growth
#define RESPONSE_TIMEOUT_MS  2500
#define STEP_THRESHOLD_US    (128LL * 1000)
#define PANIC_THRESHOLD_S    1000
#define KOD_BACKOFF_MS       (3600UL * 1000)
#define IDLE_WAKE_MS         5000
#define NEW_PEER_HIGHLIGHT_MS 10000

// Discipline gains / guards
#define MAX_FREQ_PPM_X1000   500000       // clamp +/-500 ppm
#define FREQ_KI_SHIFT        5            // 1/32 gain for the crystal drift estimator
#define MAX_FREQ_STEP_PPB    1000         // one NTP sample may move drift estimate by <= 1 ppm
#define FREQ_MAX_OFFSET_US   25000        // larger residuals are usually path asymmetry / spikes
#define FREQ_MAX_JITTER_US   20000        // don't learn crystal drift from very noisy peer sets

// Per RFC 5905: each sample's dispersion grows linearly with time at this rate.
// Makes root_dispersion honestly track uncertainty between polls.
#define PHI_US_PER_SEC       15            // 15 us/s growth

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

// Convert an NTP precision field (log2(seconds), typically negative) to the
// corresponding wall-clock uncertainty in us. Clamps at 1 us on the low end
// (anything finer gets rounded up) and at INT32_MAX on the high end (a broken
// server that sends precision=0 would mean 1 s / >= our filter cap).
static int32_t precision_to_us(int8_t precision) {
    if (precision >= 0) {
        // 2^p seconds = 2^p * 10^6 us
        if (precision >= 12) return INT32_MAX;
        int64_t us = 1000000LL << precision;
        return us > INT32_MAX ? INT32_MAX : (int32_t)us;
    }
    int shift = -(int)precision;
    if (shift >= 20) return 1;   // sub-us rounded up to 1 us
    return 1000000 >> shift;
}

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    int8_t   poll;
    int8_t   precision;
    uint32_t root_delay;       // 16.16 seconds, net order
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
    uint32_t root_delay_raw;    // 16.16 sec (as received)
    uint32_t root_dispersion_raw;

    ntp_sample_t filter[NTP_FILTER_SIZE];
    int      filter_head;

    int32_t  best_offset_us;
    int32_t  best_delay_us;
    int32_t  jitter_us;
    int32_t  dispersion_us;
    uint32_t last_response_ms;

    // Outstanding request state (for match / timeout)
    struct timeval t1;
    uint32_t xmt_sec_net;       // network-order copy of transmit timestamp we sent
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
    uint8_t  consecutive_misses;   // polls since last response; trigger swap at threshold
    uint8_t  falseticker_runs;     // consecutive cycles outside Marzullo intersection
    uint8_t  jittery_runs;         // consecutive cycles substantially noisier than best truechimer
    uint32_t fresh_until_ms;       // mono deadline for the UI-only new-peer highlight
} ntp_peer_t;

static struct {
    char     server[64];
    bool     prefer_ipv6;
    uint32_t current_poll_s;

    ntp_peer_t peers[NTP_MAX_PEERS];
    int      selected_peer;
    uint8_t  stratum;

    bool     first_sync_done;
    time_t   last_sync_time;
    int64_t  last_offset_us;
    uint32_t sync_count;
    uint32_t sync_start_ms;

    int32_t  system_jitter_us;
    int32_t  root_delay_us;
    int32_t  root_dispersion_us;
    int32_t  combined_offset_us;   // dispersion-weighted avg across survivors
    int32_t  freq_ppm_x1000;
    bool     freq_loaded_from_nvs; // freq_ppm_x1000 was restored at boot - usable before any sync
    bool     freq_learned_this_session;
    uint32_t last_freq_apply_ms;
    uint32_t last_freq_sample_ms;
    uint32_t last_discipline_ms;
    uint32_t last_discipline_poll_s;  // current_poll_s at the moment we last disciplined
    uint32_t last_any_response_ms;  // when we last heard from ANY peer
    int8_t   poll_adjust;   // counter: +N grows poll, -N shrinks

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
} g = {
    .selected_peer = -1,
    .stratum = 16,
    .wake_sock = -1,
    .sock4 = -1,
    .sock6 = -1,
};

// ---------- helpers ----------

static uint32_t mono_ms(void) {
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

static void tv_to_ntp(const struct timeval *tv, uint32_t *sec, uint32_t *frac) {
    *sec  = (uint32_t)(tv->tv_sec + NTP_EPOCH_OFFSET);
    *frac = (uint32_t)(((uint64_t)tv->tv_usec << 32) / 1000000ULL);
}

static void ntp_to_tv(uint32_t sec, uint32_t frac, struct timeval *tv) {
    // Handle the 2036 NTP era rollover. Era 0 covers 1900-01-01 through
    // 2036-02-07; past that, the 32-bit NTP seconds counter wraps and the
    // value must be interpreted against era 1's Unix offset. Heuristic: any
    // NTP value below NTP_EPOCH_OFFSET (== Unix 0) represents a time before
    // 1970, which is implausible for this device, so treat it as era 1.
    uint64_t unix_sec;
    if (sec >= NTP_EPOCH_OFFSET) {
        unix_sec = (uint64_t)(sec - NTP_EPOCH_OFFSET);       // era 0, post-1970
    } else {
        unix_sec = (uint64_t)sec + NTP_ERA1_UNIX_OFFSET;     // era 1 (2036+)
    }
    tv->tv_sec  = (time_t)unix_sec;
    tv->tv_usec = (suseconds_t)(((uint64_t)frac * 1000000ULL) >> 32);
}

static int64_t tv_diff_us(const struct timeval *a, const struct timeval *b) {
    return ((int64_t)a->tv_sec - b->tv_sec) * 1000000LL + (a->tv_usec - b->tv_usec);
}

static struct timeval tv_from_us(int64_t total_us) {
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

static int64_t tv_to_us(const struct timeval *tv) {
    return (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ntpfp 16.16 seconds -> microseconds (saturating)
static int32_t fp1616_to_us(uint32_t raw) {
    uint64_t us = ((uint64_t)raw * 1000000ULL) >> 16;
    if (us > 0x7FFFFFFFULL) us = 0x7FFFFFFFULL;
    return (int32_t)us;
}

static void lock_take(void)  { xSemaphoreTake(g.lock, portMAX_DELAY); }
static void lock_give(void)  { xSemaphoreGive(g.lock); }

// Family-aware sockaddr comparison (family + port + address only). Avoids
// padding-byte surprises that a raw memcmp on sockaddr_storage would hit.
static bool sockaddr_matches(const struct sockaddr_storage *a,
                             const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *ai = (const struct sockaddr_in *)a;
        const struct sockaddr_in *bi = (const struct sockaddr_in *)b;
        return ai->sin_port == bi->sin_port &&
               ai->sin_addr.s_addr == bi->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ai = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *bi = (const struct sockaddr_in6 *)b;
        return ai->sin6_port == bi->sin6_port &&
               memcmp(&ai->sin6_addr, &bi->sin6_addr, sizeof(ai->sin6_addr)) == 0;
    }
    return false;
}

// ---------- DNS (multi-record) ----------

// LWIP's getaddrinfo only surfaces the first A/AAAA record, even though
// pool.ntp.org typically returns several. Talk to the system resolver directly
// so we can harvest all records and use them as distinct NTP peers.

#define DNS_PORT           53
#define DNS_TYPE_A         1
#define DNS_TYPE_AAAA      28
#define DNS_CLASS_IN       1
#define DNS_RECV_TIMEOUT_S 2

// Advance past a DNS name. Returns new offset, or -1 on malformed input.
static int dns_skip_name(const uint8_t *buf, int len, int pos) {
    while (pos < len) {
        uint8_t x = buf[pos];
        if (x == 0) return pos + 1;
        if ((x & 0xC0) == 0xC0) return pos + 2;  // compression pointer
        if (x > 63) return -1;
        pos += 1 + x;
    }
    return -1;
}

static int dns_build_query(uint8_t *buf, size_t buflen, uint16_t id,
                           const char *name, uint16_t qtype) {
    if (buflen < 12) return -1;
    buf[0] = id >> 8; buf[1] = id & 0xFF;
    buf[2] = 0x01;    buf[3] = 0x00;       // flags: RD
    buf[4] = 0x00;    buf[5] = 0x01;       // QDCOUNT
    buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;
    int pos = 12;

    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t label_len = dot ? (size_t)(dot - s) : strlen(s);
        if (label_len == 0 || label_len > 63) return -1;
        if ((size_t)(pos + 1 + label_len + 1 + 4) > buflen) return -1;
        buf[pos++] = (uint8_t)label_len;
        memcpy(&buf[pos], s, label_len);
        pos += (int)label_len;
        s += label_len;
        if (*s == '.') s++;
    }
    buf[pos++] = 0;
    buf[pos++] = qtype >> 8; buf[pos++] = qtype & 0xFF;
    buf[pos++] = 0;          buf[pos++] = DNS_CLASS_IN;
    return pos;
}

// Append A/AAAA answers into out[] starting at index `count`, up to `max`.
static int dns_parse_answers(const uint8_t *buf, int len,
                             struct sockaddr_storage *out, int count, int max) {
    if (len < 12) return count;
    uint16_t flags   = (buf[2] << 8) | buf[3];
    if ((flags & 0x000F) != 0) return count;   // rcode != NOERROR
    uint16_t qdcount = (buf[4] << 8) | buf[5];
    uint16_t ancount = (buf[6] << 8) | buf[7];
    int pos = 12;

    for (int i = 0; i < qdcount; i++) {
        pos = dns_skip_name(buf, len, pos);
        if (pos < 0 || pos + 4 > len) return count;
        pos += 4;
    }
    for (int i = 0; i < ancount && count < max; i++) {
        pos = dns_skip_name(buf, len, pos);
        if (pos < 0 || pos + 10 > len) return count;
        uint16_t type  = (buf[pos] << 8) | buf[pos + 1];
        uint16_t rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return count;

        struct sockaddr_storage cand;
        memset(&cand, 0, sizeof(cand));
        bool have = false;
        if (type == DNS_TYPE_A && rdlen == 4) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&cand;
            sin->sin_family = AF_INET;
            sin->sin_port   = htons(NTP_PORT);
            memcpy(&sin->sin_addr, &buf[pos], 4);
            have = true;
        } else if (type == DNS_TYPE_AAAA && rdlen == 16) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&cand;
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port   = htons(NTP_PORT);
            memcpy(&sin6->sin6_addr, &buf[pos], 16);
            have = true;
        }
        pos += rdlen;
        if (!have) continue;

        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (sockaddr_matches(&out[j], &cand)) { dup = true; break; }
        }
        if (!dup) out[count++] = cand;
    }
    return count;
}

static int dns_query_one(int sock, const struct sockaddr_in *dst, const char *host,
                         uint16_t qtype, struct sockaddr_storage *out,
                         int count, int max) {
    uint8_t qbuf[256], rbuf[512];
    uint16_t id = esp_random() & 0xFFFF;
    int qlen = dns_build_query(qbuf, sizeof(qbuf), id, host, qtype);
    if (qlen <= 0) return count;
    if (sendto(sock, qbuf, qlen, 0, (const struct sockaddr *)dst, sizeof(*dst)) != qlen) {
        return count;
    }
    for (int tries = 0; tries < 4; tries++) {
        ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
        if (n < 12) return count;
        uint16_t resp_id = ((uint16_t)rbuf[0] << 8) | rbuf[1];
        uint16_t flags   = ((uint16_t)rbuf[2] << 8) | rbuf[3];
        if (resp_id != id || (flags & 0x8000) == 0) {
            continue;   // stale response from a previous query on this socket
        }
        return dns_parse_answers(rbuf, (int)n, out, count, max);
    }
    return count;
}

static int dns_resolve_all(const char *host, bool prefer_ipv6,
                           struct sockaddr_storage *out, int max) {
    const ip_addr_t *server = dns_getserver(0);
    if (!server || ip_addr_isany(server)) return 0;
    if (!IP_IS_V4(server)) return 0;   // only v4 DNS servers supported

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return 0;

    struct timeval tv = { .tv_sec = DNS_RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_PORT),
    };
    dst.sin_addr.s_addr = ip_addr_get_ip4_u32(server);

    // With the toggle off ("IPv6: Off"), skip AAAA entirely - the user wants
    // A-records only. With it on, prefer AAAA and fall back to A so dual-stack
    // still works when the v6 path is broken.
    int count = 0;
    if (prefer_ipv6) {
        count = dns_query_one(sock, &dst, host, DNS_TYPE_AAAA, out, count, max);
    }
    if (count < max) {
        count = dns_query_one(sock, &dst, host, DNS_TYPE_A, out, count, max);
    }
    close(sock);
    return count;
}

// ---------- peer management ----------

static uint32_t next_global_poll_ms;
static uint32_t next_global_poll_cycle_id = 1;
static uint32_t last_poll_adjust_cycle_id;

static void adaptive_poll_update(void);
static bool try_discipline(uint32_t settled_cycle_id);

static void adaptive_poll_update_once(uint32_t cycle_id) {
    if (cycle_id == 0 || last_poll_adjust_cycle_id == cycle_id) return;
    adaptive_poll_update();
    last_poll_adjust_cycle_id = cycle_id;
}

// next_global_poll_ms value at the last successful try_replace_peer. Used as
// a one-eviction-per-cycle rate limit - equality means we've already swapped
// a peer during this poll tick and should defer the rest. Initialized to a
// sentinel that can't match next_global_poll_ms until the second-ish after
// cold boot, so the first eviction attempt always gets through.
static uint32_t last_evict_tick_ms = UINT32_MAX;

static void peer_reset(ntp_peer_t *p) {
    memset(p, 0, sizeof(*p));
    p->stratum = 16;
    // This is purely a UI affordance: show a newly-installed peer long
    // enough to notice, independent of the adaptive poll interval. Tying the
    // highlight to the next poll tick made replacement peers stay green for
    // minutes once polling had grown to 1024 s.
    p->fresh_until_ms = mono_ms() + NEW_PEER_HIGHLIGHT_MS;
}

static int resolve_peers(void) {
    // DNS is slow (tens to hundreds of ms, up to 2 s on timeout) and was
    // holding the NTP lock the whole time - which blocked UI stats getters
    // and caused ui_clock_update to stall, with visible-on-film lag spikes
    // up to ~100 ms. Snapshot config, release the lock for the network
    // round-trips, then re-acquire before touching shared state.
    char server_copy[sizeof(g.server)];
    strncpy(server_copy, g.server, sizeof(server_copy) - 1);
    server_copy[sizeof(server_copy) - 1] = '\0';
    bool prefer_ipv6_copy = g.prefer_ipv6;
    lock_give();

    struct sockaddr_storage addrs[NTP_MAX_PEERS];
    int n = dns_resolve_all(server_copy, prefer_ipv6_copy, addrs, NTP_MAX_PEERS);

    // Fallback to LWIP getaddrinfo if direct DNS failed (e.g. v6-only resolver).
    if (n == 0) {
        struct addrinfo hints = {
            .ai_family   = prefer_ipv6_copy ? AF_UNSPEC : AF_INET,
            .ai_socktype = SOCK_DGRAM,
            .ai_protocol = IPPROTO_UDP,
        };
        struct addrinfo *res = NULL;
        if (getaddrinfo(server_copy, "123", &hints, &res) == 0) {
            for (struct addrinfo *ai = res; ai && n < NTP_MAX_PEERS; ai = ai->ai_next) {
                if (ai->ai_addrlen > sizeof(addrs[0])) continue;
                memcpy(&addrs[n], ai->ai_addr, ai->ai_addrlen);
                n++;
            }
            if (res) freeaddrinfo(res);
        }
    }

    lock_take();

    if (strcmp(g.server, server_copy) != 0 || g.prefer_ipv6 != prefer_ipv6_copy) {
        ESP_LOGI(TAG, "Discarding stale DNS results for %s", server_copy);
        return 0;
    }

    for (int i = 0; i < NTP_MAX_PEERS; i++) peer_reset(&g.peers[i]);
    g.selected_peer = -1;
    g.stratum = 16;

    if (n == 0) {
        ESP_LOGW(TAG, "DNS failed for %s", server_copy);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        ntp_peer_t *p = &g.peers[i];
        socklen_t alen = (addrs[i].ss_family == AF_INET6)
            ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        memcpy(&p->addr, &addrs[i], alen);
        p->addr_len = alen;
        p->active   = true;
        p->stratum  = 16;
        p->next_poll_ms = mono_ms();
        p->next_poll_cycle_id = next_global_poll_cycle_id;

        const void *src = (addrs[i].ss_family == AF_INET6)
            ? (const void *)&((struct sockaddr_in6 *)&addrs[i])->sin6_addr
            : (const void *)&((struct sockaddr_in  *)&addrs[i])->sin_addr;
        inet_ntop(addrs[i].ss_family, src, p->addr_str, sizeof(p->addr_str));
    }
    ESP_LOGI(TAG, "Resolved %s to %d peer(s)", g.server, n);
    return n;
}

// Replace a single dead peer with a fresh DNS lookup result that isn't already
// in our peer table. Keeps the working peers' filter/reach history intact,
// unlike a full resolve_peers. Returns true if a replacement was installed.
//
// No rate-limiting here: pool.ntp.org's DNS TTL is ~60 s, and steady-state
// polls (MIN_POLL_S = 32 s, typically growing to 64 s+) are already as long
// as the TTL or longer, so a poll-cycle-aligned retry won't query faster
// than the resolver refreshes.
//
// Drops the NTP lock during the DNS query - otherwise a 50-200 ms DNS
// round-trip would block ui_clock_update's stats getters and surface as a
// visible lag spike on the display and LED.

// Return the peer index whose eviction counters are furthest over threshold,
// or -1 if nothing is currently eligible. Severity is just the sum of the
// three counters (all saturate at 255, same magnitude range) - higher means
// further gone. Callers should gate this against last_evict_tick_ms so we
// only swap one peer per poll cycle even when several look bad.
static int find_worst_eligible_peer(void) {
    int      worst          = -1;
    uint32_t worst_severity = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active) continue;
        if (p->consecutive_misses < 4 &&
            p->falseticker_runs   < 8 &&
            p->jittery_runs       < 10) continue;
        uint32_t severity = (uint32_t)p->consecutive_misses +
                            (uint32_t)p->falseticker_runs +
                            (uint32_t)p->jittery_runs;
        if (severity > worst_severity) {
            worst_severity = severity;
            worst          = i;
        }
    }
    return worst;
}

static bool try_replace_peer(int dead_idx) {
    char server_copy[sizeof(g.server)];
    strncpy(server_copy, g.server, sizeof(server_copy) - 1);
    server_copy[sizeof(server_copy) - 1] = '\0';
    bool prefer_ipv6_copy = g.prefer_ipv6;
    lock_give();

    struct sockaddr_storage fresh[NTP_MAX_PEERS];
    int n = dns_resolve_all(server_copy, prefer_ipv6_copy, fresh, NTP_MAX_PEERS);

    lock_take();

    if (g.dirty_config ||
        strcmp(g.server, server_copy) != 0 ||
        g.prefer_ipv6 != prefer_ipv6_copy) {
        return false;
    }
    if (n == 0) return false;

    for (int i = 0; i < n; i++) {
        bool in_use = false;
        for (int j = 0; j < NTP_MAX_PEERS; j++) {
            if (g.peers[j].active && sockaddr_matches(&g.peers[j].addr, &fresh[i])) {
                in_use = true;
                break;
            }
        }
        if (in_use) continue;

        ntp_peer_t *p = &g.peers[dead_idx];
        char old_addr[46];
        strncpy(old_addr, p->addr_str, sizeof(old_addr) - 1);
        old_addr[sizeof(old_addr) - 1] = '\0';

        peer_reset(p);
        socklen_t alen = (fresh[i].ss_family == AF_INET6)
            ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        memcpy(&p->addr, &fresh[i], alen);
        p->addr_len     = alen;
        p->active       = true;
        p->stratum      = 16;
        p->next_poll_ms = mono_ms();
        p->next_poll_cycle_id = next_global_poll_cycle_id;
        const void *src = (fresh[i].ss_family == AF_INET6)
            ? (const void *)&((struct sockaddr_in6 *)&fresh[i])->sin6_addr
            : (const void *)&((struct sockaddr_in  *)&fresh[i])->sin_addr;
        inet_ntop(fresh[i].ss_family, src, p->addr_str, sizeof(p->addr_str));

        ESP_LOGI(TAG, "Peer swap slot %d: %s -> %s", dead_idx, old_addr, p->addr_str);
        return true;
    }
    return false;
}

// ---------- sockets ----------

static bool open_sockets(void) {
    if (g.sock4 < 0) {
        g.sock4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g.sock4 >= 0) {
            struct sockaddr_in local = {
                .sin_family = AF_INET,
                .sin_port   = 0,
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            bind(g.sock4, (struct sockaddr *)&local, sizeof(local));
        }
    }
    if (g.sock6 < 0) {
        g.sock6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (g.sock6 >= 0) {
            struct sockaddr_in6 local = {
                .sin6_family = AF_INET6,
                .sin6_port   = 0,
            };
            bind(g.sock6, (struct sockaddr *)&local, sizeof(local));
        }
    }
    return g.sock4 >= 0 || g.sock6 >= 0;
}

static void close_sockets(void) {
    if (g.sock4 >= 0) { close(g.sock4); g.sock4 = -1; }
    if (g.sock6 >= 0) { close(g.sock6); g.sock6 = -1; }
}

static void close_wake_sock(void) {
    if (g.wake_sock >= 0) {
        close(g.wake_sock);
        g.wake_sock = -1;
        g.wake_port = 0;
    }
}

// ---------- task wake (self-pipe over loopback UDP) ----------

static void open_wake_sock(void) {
    if (g.wake_sock >= 0) return;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return;
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return;
    }
    socklen_t len = sizeof(addr);
    getsockname(s, (struct sockaddr *)&addr, &len);
    g.wake_sock = s;
    g.wake_port = ntohs(addr.sin_port);
}

// Signal the ntp task to break out of select() immediately. Safe to call
// from any task; lwip serializes socket ops internally.
static void wake_task(void) {
    if (g.wake_sock < 0 || g.wake_port == 0) return;
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(g.wake_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    uint8_t byte = 1;
    sendto(g.wake_sock, &byte, 1, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void drain_wake_sock(void) {
    if (g.wake_sock < 0) return;
    uint8_t buf[16];
    while (recv(g.wake_sock, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

// ---------- send ----------

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

static void schedule_after_request(ntp_peer_t *p) {
    uint32_t now = mono_ms();
    uint32_t interval_ms = g.current_poll_s * 1000;
    if ((int32_t)(now - next_global_poll_ms) >= 0) {
        next_global_poll_ms = now + interval_ms;
        next_global_poll_cycle_id++;
        if (next_global_poll_cycle_id == 0) next_global_poll_cycle_id = 1;
    }
    int idx = (int)(p - g.peers);
    uint32_t slot_ms = SPLAY_WINDOW_MS / NTP_MAX_PEERS;
    uint32_t jitter  = esp_random() % slot_ms;
    p->next_poll_ms  = next_global_poll_ms + (uint32_t)idx * slot_ms + jitter;
    p->next_poll_cycle_id = next_global_poll_cycle_id;
}

static bool send_request(ntp_peer_t *p) {
    ntp_pkt_t pkt = {0};
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.precision  = LOCAL_PRECISION;
    uint32_t poll_s = g.current_poll_s ? g.current_poll_s : MIN_POLL_S;
    int8_t poll_exp = 0;
    while ((1U << poll_exp) < poll_s && poll_exp < 15) poll_exp++;
    pkt.poll = poll_exp;

    uint32_t request_cycle_id = p->next_poll_cycle_id;
    if (request_cycle_id == 0) {
        request_cycle_id = next_global_poll_cycle_id ? next_global_poll_cycle_id : 1;
    }

    int sock = (p->addr.ss_family == AF_INET6) ? g.sock6 : g.sock4;
    if (sock < 0) {
        ESP_LOGW(TAG, "No socket for peer=%s family=%d", p->addr_str, p->addr.ss_family);
        if (p->consecutive_misses < 255) p->consecutive_misses++;
        p->reach <<= 1;
        p->cycle_id_when_sent  = request_cycle_id;
        p->last_settle_cycle_id = request_cycle_id;
        schedule_after_request(p);
        try_discipline(request_cycle_id);
        return false;
    }

    // Stamp xmt as close to sendto as we can - compose the packet with a
    // placeholder first, then take t1_pre / sendto / t1_post around the
    // call itself. We use the MIDPOINT of pre and post as the real t1 for
    // offset computation, which halves the stack-transit bias vs capturing
    // only before sendto. The packet's xmt field is composed with t1_pre
    // (for correlation via orig-ts), which is fine since the server just
    // echoes whatever we put there - it has no timing semantics.
    struct timeval t1_pre, t1_post;
    gettimeofday(&t1_pre, NULL);
    uint32_t sec, frac;
    tv_to_ntp(&t1_pre, &sec, &frac);
    frac ^= esp_random();
    pkt.xmt_ts_sec  = htonl(sec);
    pkt.xmt_ts_frac = htonl(frac);

    ssize_t n = sendto(sock, &pkt, sizeof(pkt), 0,
                       (struct sockaddr *)&p->addr, p->addr_len);
    gettimeofday(&t1_post, NULL);
    if (n != sizeof(pkt)) {
        ESP_LOGW(TAG, "sendto %s failed (errno=%d)", p->addr_str, errno);
        // Treat as a miss and advance the schedule so we don't spin-loop
        // retrying the same unroutable peer every tick. After enough misses
        // the per-peer swap logic will rotate it out.
        if (p->consecutive_misses < 255) p->consecutive_misses++;
        p->reach <<= 1;
        p->cycle_id_when_sent  = request_cycle_id;
        p->last_settle_cycle_id = request_cycle_id;
        schedule_after_request(p);
        try_discipline(request_cycle_id);
        return false;
    }

    // t1 = midpoint of pre-sendto and post-sendto gettimeofday.
    int64_t t1_mid_us =
        ((int64_t)t1_pre.tv_sec  * 1000000LL + t1_pre.tv_usec +
         (int64_t)t1_post.tv_sec * 1000000LL + t1_post.tv_usec) / 2;
    p->t1.tv_sec  = (time_t)(t1_mid_us / 1000000);
    p->t1.tv_usec = (suseconds_t)(t1_mid_us % 1000000);
    p->xmt_sec_net  = pkt.xmt_ts_sec;
    p->xmt_frac_net = pkt.xmt_ts_frac;
    p->reach <<= 1;        // new poll starts with bit 0 = 0 until response
    p->request_outstanding = true;
    p->request_sent_ms     = mono_ms();
    p->cycle_id_when_sent  = request_cycle_id;      // wave tag, see ntp_peer_t
    return true;
}

// ---------- filter ----------

static void update_peer_filter(ntp_peer_t *p) {
    int valid = 0;
    for (int i = 0; i < NTP_FILTER_SIZE; i++) {
        if (p->filter[i].valid) valid++;
    }
    if (valid == 0) {
        p->best_offset_us = 0;
        p->best_delay_us  = 0;
        p->jitter_us      = 0;
        p->dispersion_us  = 16000000;
        return;
    }

    // Discipline from the newest sample. Using lowest-delay across the filter
    // window can freeze the correction on an old sample (since newer samples
    // rarely beat an earlier low-delay slot), causing repeated over-correction.
    const ntp_sample_t *newest = &p->filter[p->filter_head];
    p->best_offset_us = newest->offset_us;
    p->best_delay_us  = newest->delay_us;

    if (valid > 1) {
        // Jitter: RMS difference of other samples from the newest offset.
        double sum_sq = 0.0;
        int n = 0;
        for (int i = 0; i < NTP_FILTER_SIZE; i++) {
            if (!p->filter[i].valid || i == p->filter_head) continue;
            double d = (double)(p->filter[i].offset_us - p->best_offset_us);
            sum_sq += d * d;
            n++;
        }
        p->jitter_us = n > 0 ? (int32_t)sqrt(sum_sq / n) : 0;
    } else {
        p->jitter_us = newest->delay_us / 2;
    }
    p->dispersion_us = newest->dispersion_us + p->jitter_us;
}

static int32_t aged_peer_dispersion_us(const ntp_peer_t *p, uint32_t now_ms) {
    int64_t disp = p->dispersion_us;
    if (p->last_response_ms != 0) {
        uint32_t age_ms = now_ms - p->last_response_ms;
        disp += (int64_t)PHI_US_PER_SEC * age_ms / 1000;
    } else {
        disp += 16000000LL;
    }
    if (disp > INT32_MAX) return INT32_MAX;
    return (int32_t)disp;
}

// ---------- early t1/t4 capture (WiFi hooks) ----------
//
// Stamp t4 at the WiFi RX cb (saves lwIP + scheduler latency between radio
// and recvfrom - typically 1-2 ms) and t1 at the WiFi TX-done cb (the radio
// has actually transmitted by then, including DCF backoff + ACK + retries
// - saves 5-30 ms versus stamping pre-sendto). Both keyed by NTP originate
// timestamp (the server echoes our xmt as orig), so process_response can
// look them up by pkt->orig_ts_*. Falls back to the original timestamps on
// any parse miss.
//
// Buffer formats are not the same: the RX cb gets the cooked Ethernet frame
// esp_netif feeds to lwIP, while the TX-done cb gets the over-the-air 802.11
// MPDU. AND: *data_len from the TX cb is the post-MAC-header length (CCMP +
// LLC/SNAP + payload + MIC), even though `data` points to the start of the
// MAC header - bounds-check against (buf + hdr_len + len), not just len.

#define EARLY_RING_SIZE 8

typedef struct {
    uint32_t      ts_sec, ts_frac;     // wire order, matches ntp_pkt_t fields
    int64_t       wall_us;
    bool          valid;
} early_entry_t;

typedef struct {
    early_entry_t ring[EARLY_RING_SIZE];
    portMUX_TYPE  lock;
    int           head;
} early_ring_t;

static early_ring_t s_t1 = { .lock = portMUX_INITIALIZER_UNLOCKED };
static early_ring_t s_t4 = { .lock = portMUX_INITIALIZER_UNLOCKED };
static esp_netif_t *s_sta_netif;

static void stash_early(early_ring_t *r, uint32_t sec, uint32_t frac, int64_t us) {
    portENTER_CRITICAL(&r->lock);
    r->ring[r->head] = (early_entry_t){
        .ts_sec = sec, .ts_frac = frac, .wall_us = us, .valid = true,
    };
    r->head = (r->head + 1) % EARLY_RING_SIZE;
    portEXIT_CRITICAL(&r->lock);
}

static bool consume_early(early_ring_t *r, uint32_t sec, uint32_t frac, int64_t *us) {
    bool found = false;
    portENTER_CRITICAL(&r->lock);
    for (int i = 0; i < EARLY_RING_SIZE; i++) {
        if (r->ring[i].valid && r->ring[i].ts_sec == sec && r->ring[i].ts_frac == frac) {
            *us = r->ring[i].wall_us;
            r->ring[i].valid = false;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&r->lock);
    return found;
}

// Shift every live entry by delta_us. Used by the cold-boot first-sync path
// to drag entries captured pre-step into the post-step frame.
static void shift_early(early_ring_t *r, int64_t delta_us) {
    portENTER_CRITICAL(&r->lock);
    for (int i = 0; i < EARLY_RING_SIZE; i++) {
        if (r->ring[i].valid) r->ring[i].wall_us += delta_us;
    }
    portEXIT_CRITICAL(&r->lock);
}

static inline uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

// Common tail: given a pointer at the start of an IP header and `rem` bytes
// reachable from there, validate IPv4/IPv6 -> UDP, check the port at offset
// `port_off` within the UDP header (0 = src, 2 = dst), and copy the 8-byte
// NTP timestamp at offset `ntp_field_off` (24 for orig_ts, 40 for xmt_ts).
static bool parse_ip_udp_ntp(const uint8_t *ip, size_t rem, uint16_t et,
                             int port_off, int ntp_field_off,
                             uint32_t *sec, uint32_t *frac) {
    const uint8_t *udp;
    if (et == 0x0800) {                                       // IPv4
        if (rem < 20u + 8u + 48u) return false;
        uint8_t ihl = (ip[0] & 0x0f) * 4;
        if (ihl < 20u || rem < (size_t)ihl + 8u + 48u || ip[9] != 17) return false;
        udp = ip + ihl;
    } else if (et == 0x86DD) {                                // IPv6, no ext headers
        if (rem < 40u + 8u + 48u || ip[6] != 17) return false;
        udp = ip + 40;
    } else {
        return false;
    }
    if (rd_be16(udp + port_off) != NTP_PORT) return false;
    const uint8_t *ntp = udp + 8;
    memcpy(sec,  ntp + ntp_field_off,     4);
    memcpy(frac, ntp + ntp_field_off + 4, 4);
    return true;
}

static esp_err_t ntp_wifi_rxcb(void *buffer, uint16_t len, void *eb) {
    struct timeval tv;
    gettimeofday(&tv, NULL);                                  // capture asap

    const uint8_t *buf = buffer;
    uint32_t sec, frac;
    if (len >= 14u + 20u + 8u + 48u &&
        parse_ip_udp_ntp(buf + 14, len - 14u, rd_be16(buf + 12),
                         /*src port*/ 0, /*orig_ts*/ 24, &sec, &frac)) {
        stash_early(&s_t4, sec, frac,
                    (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec);
    }
    return esp_netif_receive(s_sta_netif, buffer, len, eb);
}

static void ntp_wifi_tx_done_cb(uint8_t ifidx, uint8_t *data,
                                 uint16_t *data_len, bool txStatus) {
    (void)ifidx;
    if (!txStatus || !data || !data_len || *data_len < 4) return;
    uint16_t len = *data_len;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint8_t fc0 = data[0], fc1 = data[1];
    if ((fc0 & 0x0c) != 0x08) return;                         // type != Data
    uint8_t subtype = (fc0 & 0xf0) >> 4;
    if (subtype != 0 && subtype != 8) return;                 // Data / QoS Data only
    size_t hdr_len    = 24
                      + (subtype == 8           ? 2 : 0)      // QoS Control
                      + ((fc1 & 0x03) == 0x03   ? 6 : 0);     // 4-address (rare)
    size_t cipher_hdr = (fc1 & 0x40) ? 8 : 0;                 // CCMP/TKIP if Protected
    if ((size_t)len < cipher_hdr + 8u + 20u + 8u + 48u) return;

    static const uint8_t llc_snap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
    const uint8_t *p = data + hdr_len + cipher_hdr;
    if (memcmp(p, llc_snap, 6) != 0) return;

    uint32_t sec, frac;
    if (parse_ip_udp_ntp(p + 8, len - cipher_hdr - 8u, rd_be16(p + 6),
                         /*dst port*/ 2, /*xmt_ts*/ 40, &sec, &frac)) {
        stash_early(&s_t1, sec, frac,
                    (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec);
    }
}

void ntp_install_wifi_rx_hook(void) {
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) { ESP_LOGW(TAG, "STA netif not found"); return; }
    esp_err_t err = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ntp_wifi_rxcb);
    if (err != ESP_OK) { ESP_LOGW(TAG, "rxcb reg failed: %d", err); return; }
    err = esp_wifi_set_tx_done_cb(ntp_wifi_tx_done_cb);
    if (err != ESP_OK) ESP_LOGW(TAG, "tx_done_cb reg failed: %d", err);
}

// ---------- response processing ----------

static bool process_response(ntp_peer_t *p, const ntp_pkt_t *pkt,
                             const struct timeval *t4) {
    uint8_t mode = pkt->li_vn_mode & 0x07;
    uint8_t vn   = (pkt->li_vn_mode >> 3) & 0x07;
    uint8_t li   = (pkt->li_vn_mode >> 6) & 0x03;

    if (mode != NTP_MODE_SERVER || vn < 3) return false;

    if (pkt->stratum == 0) {
        char code[5] = {0};
        memcpy(code, &pkt->ref_id, 4);
        ESP_LOGW(TAG, "KoD from %s: %.4s", p->addr_str, code);
        p->stratum = 16;
        p->reach   = 0;
        if (!memcmp(code, "DENY", 4) || !memcmp(code, "RSTR", 4)) {
            p->active = false;
        } else {
            p->kod_until_ms = mono_ms() + KOD_BACKOFF_MS;
        }
        return false;
    }
    if (li == 3) return false;   // server's clock unsync

    // Match originate timestamp to the one we sent
    if (pkt->orig_ts_sec != p->xmt_sec_net ||
        pkt->orig_ts_frac != p->xmt_frac_net) {
        ESP_LOGW(TAG, "orig-ts mismatch from %s", p->addr_str);
        return false;
    }

    // Reject packets with a zero transmit timestamp (server hasn't set clock)
    if (pkt->xmt_ts_sec == 0) return false;

    struct timeval t2, t3;
    ntp_to_tv(ntohl(pkt->recv_ts_sec), ntohl(pkt->recv_ts_frac), &t2);
    ntp_to_tv(ntohl(pkt->xmt_ts_sec),  ntohl(pkt->xmt_ts_frac),  &t3);

    // Defensive sanity checks on server-side fields:
    //  * t3 (server transmit) must not precede t2 (server receive) - that
    //    would imply the server processed in negative time.
    //  * root_delay and root_dispersion are 16.16 seconds; anything > 16s
    //    is nonsense for a real stratum-N server.
    if (tv_diff_us(&t3, &t2) < 0) return false;
    if (fp1616_to_us(ntohl(pkt->root_delay))      > 16000000 ||
        fp1616_to_us(ntohl(pkt->root_dispersion)) > 16000000) return false;

    // Prefer the WiFi-hook timestamps (t4 from the RX cb, t1 from the TX-done
    // cb) when present. Skip early_t1 on the cold-boot path: the shift loop
    // below mutates each peer's stored t1, and reconciling that with an
    // independent early value is more bookkeeping than it's worth for an
    // event that fires at most once per boot.
    struct timeval t1_local = p->t1;
    struct timeval t4_local = *t4;
    int64_t early_us;
    if (consume_early(&s_t4, pkt->orig_ts_sec, pkt->orig_ts_frac, &early_us)) {
        t4_local.tv_sec  = (time_t)(early_us / 1000000);
        t4_local.tv_usec = (suseconds_t)(early_us % 1000000);
    }
    if (g.first_sync_done) {
        if (consume_early(&s_t1, pkt->orig_ts_sec, pkt->orig_ts_frac, &early_us)) {
            t1_local.tv_sec  = (time_t)(early_us / 1000000);
            t1_local.tv_usec = (suseconds_t)(early_us % 1000000);
        }
    }

    // Cold boot: system time is at epoch, server is decades ahead. Step the
    // clock by the standard NTP offset ((t2-t1)+(t3-t4))/2 - NOT directly to
    // t3. A naive `settimeofday(&t3)` lands our clock at the moment the
    // server SENT the response, ignoring the inbound network delay we
    // already spent receiving it; that leaves us trailing by ~d_in (~ RTT/2,
    // so 10-20 ms for typical pool peers) and every subsequent sample from
    // every peer shows up biased by that amount until the first slew. Using
    // the offset formula symmetrically splits the RTT and lands within us.
    if (!g.first_sync_done) {
        int64_t step_us = (tv_diff_us(&t2, &p->t1) + tv_diff_us(&t3, &t4_local)) / 2;

        // Every peer in this burst has t1 captured in the pre-step clock
        // frame; shift ALL outstanding ones (including self) forward by
        // the same step so their offsets compute correctly in the new
        // frame. Without this, peers 1-3 would hit the panic threshold
        // and get tossed, and peer 0 (self) would produce a nonsense sample.
        // Same applies to the early_t1/t4 hook rings - entries stashed
        // before the step are in the pre-step frame.
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *q = &g.peers[i];
            if (!q->request_outstanding) continue;
            q->t1 = tv_from_us(tv_to_us(&q->t1) + step_us);
        }
        shift_early(&s_t1, step_us);
        shift_early(&s_t4, step_us);

        // Step the local clock by step_us: target = now + step_us.
        struct timeval now_pre, target;
        gettimeofday(&now_pre, NULL);
        target = tv_from_us(tv_to_us(&now_pre) + step_us);
        settimeofday(&target, NULL);
        // Preserve the actual response-arrival timestamp. Refreshing this
        // with gettimeofday() after settimeofday() would add parser/logging
        // latency to the first sample; translating the captured t4 keeps all
        // four NTP timestamps in the same post-step clock frame.
        t4_local = tv_from_us(tv_to_us(&t4_local) + step_us);

        g.first_sync_done      = true;
        g.sync_count++;
        g.last_sync_time       = t4_local.tv_sec;
        g.last_offset_us       = 0;
        g.last_any_response_ms = mono_ms();
        // Treat the step as "just disciplined" so the rate-limit check in
        // handle_socket_readable suppresses a second discipline when the
        // rest of this poll burst's peers arrive. Otherwise sync_count
        // would increment twice on a single cold-boot cycle.
        g.last_discipline_ms     = mono_ms();
        g.last_discipline_poll_s = g.current_poll_s;
        ESP_LOGI(TAG, "Initial time set from %s (stratum %d)",
                 p->addr_str, pkt->stratum);
        // Refresh t1_local from the now-shifted p->t1 so the offset math
        // below sees this peer's t1 in the same post-step frame as t2/t3/t4.
        // (Skipping the early_t1 override here; see comment above.)
        t1_local = p->t1;
        // Fall through: compute offset/delay for this peer against the
        // post-step frame and feed into the normal filter path.
    }

    int64_t offset = (tv_diff_us(&t2, &t1_local) + tv_diff_us(&t3, &t4_local)) / 2;
    int64_t delay  = tv_diff_us(&t4_local, &t1_local) - tv_diff_us(&t3, &t2);

    if (offset >  (int64_t)PANIC_THRESHOLD_S * 1000000LL ||
        offset < -(int64_t)PANIC_THRESHOLD_S * 1000000LL) {
        ESP_LOGW(TAG, "Offset %lld us from %s exceeds panic threshold",
                 (long long)offset, p->addr_str);
        return false;
    }
    if (delay < 0) delay = 0;
    if (delay > 0x7FFFFFFF) delay = 0x7FFFFFFF;
    if (offset > 0x7FFFFFFF) offset = 0x7FFFFFFF;
    if (offset < -0x7FFFFFFF) offset = -0x7FFFFFFF;

    // Huff-n-puff: when the current RTT is noticeably higher than the minimum
    // we've seen recently from this peer, the excess is almost certainly in
    // ONE direction (uplink saturation, ISP shaping, WiFi rate adaptation) -
    // NTP's offset formula assumes symmetric paths, so asymmetry biases the
    // offset by up to half the excess. Move offset toward zero by that amount
    // (bounded so we don't overshoot zero). Uses the filter's 8-sample sliding
    // window as the "recently good" reference.
    //
    // Guardrails (conservative - over-correcting is worse than no correction):
    //   * Require >=3 prior samples so min_delay isn't a one-shot outlier.
    //   * Only correct when delay > 1.5 * min_delay (ratio, not absolute) -
    //     this triggers on any scale of path.
    //   * Skip correction when delay > 3 * min_delay - a spike that large is
    //     almost certainly a transient (packet loss, retransmit, burst), not
    //     steady-state asymmetry; inferring direction from it is unreliable.
    int32_t min_delay = INT32_MAX;
    int valid_samples = 0;
    for (int i = 0; i < NTP_FILTER_SIZE; i++) {
        if (!p->filter[i].valid) continue;
        valid_samples++;
        if (p->filter[i].delay_us < min_delay) min_delay = p->filter[i].delay_us;
    }
    if (valid_samples >= 3 && min_delay > 0 &&
        (int64_t)delay > (int64_t)min_delay * 3 / 2 &&
        (int64_t)delay < (int64_t)min_delay * 3) {
        int32_t excess     = (int32_t)delay - min_delay;
        int32_t correction = excess / 2;
        int64_t before     = offset;
        if (offset >  correction)      offset -= correction;
        else if (offset < -correction) offset += correction;
        else                           offset = 0;
        ESP_LOGD(TAG, "HNP %s delay=%ldus min=%ldus excess=%ldus offset %+lldus -> %+lldus",
                 p->addr_str, (long)delay, (long)min_delay, (long)excess,
                 (long long)before, (long long)offset);
    }

    // Per RFC 5905 section 5: per-sample dispersion epsilon = rho_local + rho_server + PHI*(T4-T1).
    // rho terms are the clock precisions of each end (2^precision seconds each),
    // and PHI*(T4-T1) accounts for frequency uncertainty across the measurement
    // window. Sum is floored to avoid under-reporting when a stratum-1 peer
    // advertises sub-us precision that our transport can't actually deliver.
    int64_t rtt_us  = tv_diff_us(&t4_local, &t1_local);
    int32_t eps_phi = (rtt_us > 0)
                      ? (int32_t)((uint64_t)PHI_US_PER_SEC * rtt_us / 1000000)
                      : 0;
    int32_t sample_disp = precision_to_us(LOCAL_PRECISION) +
                          precision_to_us(pkt->precision) +
                          eps_phi;
    if (sample_disp < SAMPLE_DISP_FLOOR_US) sample_disp = SAMPLE_DISP_FLOOR_US;

    p->filter_head = (p->filter_head + 1) % NTP_FILTER_SIZE;
    p->filter[p->filter_head].offset_us     = (int32_t)offset;
    p->filter[p->filter_head].delay_us      = (int32_t)delay;
    p->filter[p->filter_head].dispersion_us = sample_disp;
    p->filter[p->filter_head].valid         = true;

    p->stratum         = pkt->stratum;
    p->precision       = pkt->precision;
    p->root_delay_raw      = ntohl(pkt->root_delay);
    p->root_dispersion_raw = ntohl(pkt->root_dispersion);
    p->last_response_ms = mono_ms();
    p->reach |= 1;
    p->consecutive_misses = 0;
    g.last_any_response_ms = mono_ms();
    return true;
}

// ---------- selection (Marzullo intersection + lowest-jitter survivor) ----------

static void select_system_peer(void) {
    struct { int idx; int32_t lo; int32_t hi; } c[NTP_MAX_PEERS];
    uint32_t now = mono_ms();
    int n = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active || p->reach == 0) continue;
        if (p->stratum == 0 || p->stratum >= 16) continue;
        // Marzullo half-width. Full textbook delay/2 is too wide for WiFi +
        // public pool peers: a 60 ms RTT turns every interval into +/-30 ms,
        // which lets stable-but-wrong peers survive. But using zero delay
        // allowance is too tight: honest peers separated by normal path
        // asymmetry can fail to overlap and selection collapses to whichever
        // single-peer mask appears first. Use a bounded delay term so consensus
        // has room for real network uncertainty without letting RTT dominate.
        int32_t delay_allowance = p->best_delay_us / 4;
        if (delay_allowance > 10000) delay_allowance = 10000;
        int32_t unc = delay_allowance + p->jitter_us + aged_peer_dispersion_us(p, now);
        c[n].idx = i;
        c[n].lo  = p->best_offset_us - unc;
        c[n].hi  = p->best_offset_us + unc;
        n++;
    }
    if (n == 0) {
        g.selected_peer = -1;
        g.stratum = 16;
        return;
    }

    // Largest subset whose intervals pairwise overlap (max_lo <= min_hi).
    // n <= 4, so brute-force 2^n - 1 masks.
    int best_count = 0;
    int best_mask  = 0;
    for (int mask = 1; mask < (1 << n); mask++) {
        int count = __builtin_popcount(mask);
        if (count < best_count) continue;
        int32_t lo = INT32_MIN, hi = INT32_MAX;
        for (int i = 0; i < n; i++) {
            if (!(mask & (1 << i))) continue;
            if (c[i].lo > lo) lo = c[i].lo;
            if (c[i].hi < hi) hi = c[i].hi;
        }
        if (lo <= hi && count > best_count) {
            best_count = count;
            best_mask  = mask;
        }
    }
    if (best_count == 0) {
        g.selected_peer = -1;
        g.stratum = 16;
        return;
    }

    int chosen = -1;
    int32_t best_jitter = INT32_MAX;
    for (int i = 0; i < n; i++) {
        ntp_peer_t *pp = &g.peers[c[i].idx];
        if (best_mask & (1 << i)) {
            // Inside the Marzullo intersection - truechimer this round.
            pp->falseticker_runs = 0;
            if (pp->jitter_us < best_jitter) {
                best_jitter = pp->jitter_us;
                chosen = c[i].idx;
            }
        } else {
            // Candidate whose interval didn't overlap the consensus.
            // Count up; the timeout/response path will swap us out if
            // we're persistently wrong.
            if (pp->falseticker_runs < 255) pp->falseticker_runs++;
        }
    }
    if (chosen < 0) { g.selected_peer = -1; g.stratum = 16; return; }

    // Wide-jitter gate: a truechimer whose filter noise is substantially
    // worse than the cleanest truechimer's is clogging the survivor set -
    // its Marzullo interval is wide enough to keep it inside consensus, but
    // it drags up the combined variance and the noise level of the
    // discipline input. The reference is best_jitter (not a hardcoded
    // floor), so this self-calibrates to network conditions: on a crappy
    // WiFi link where every peer runs at 40 ms, nothing churns; on a clean
    // link where the best peer runs at 2 ms, anything above ~8 ms stands
    // out. Count runs; the dispatch loop swaps at threshold, same path as
    // falseticker eviction.
    const int32_t JITTER_REL_X = 4;
    for (int i = 0; i < n; i++) {
        ntp_peer_t *pp = &g.peers[c[i].idx];
        bool wide = (best_mask & (1 << i)) &&
                    pp->jitter_us > JITTER_REL_X * best_jitter;
        if (wide) {
            if (pp->jittery_runs < 255) pp->jittery_runs++;
        } else {
            pp->jittery_runs = 0;
        }
    }

    ntp_peer_t *sp = &g.peers[chosen];
    g.selected_peer     = chosen;
    g.stratum           = (sp->stratum < 15) ? sp->stratum + 1 : 15;
    g.root_delay_us     = fp1616_to_us(sp->root_delay_raw) + sp->best_delay_us;

    // Peer combining: inverse-variance-weighted average of all Marzullo
    // survivors, not just the lowest-jitter one. A clean peer dominates the
    // average via its small dispersion; noisier survivors contribute
    // proportionally less. Gives ~1/sqrt(N) noise reduction when peers have
    // comparable quality and degrades gracefully when one peer is clearly
    // better.
    //
    // Weights are 1/dispersion^2 (not 1/dispersion). This is the textbook
    // minimum-variance unbiased combination of independent estimates: with
    // sigma_i ~ dispersion_i, the optimal weight is 1/sigma_i^2. Cleaner peers get
    // *quadratically* more weight than noisier ones, so a single good peer
    // can dominate a survivor set that includes wider-jitter peers - which
    // is exactly what we want.
    //
    // System jitter uses the variance-of-weighted-mean formula so it
    // reflects the actual uncertainty of the combined estimate (which is
    // reduced by the combining), not just the typical peer's jitter.
    //   combined      = sum(w*x) / sumw            with w = 1/dispersion^2
    //   Var(combined) = sum(w^2*sigma^2) / (sumw)^2
    //   sigma_combined    = sqrt(sum(w^2*sigma^2)) / sumw
    // Doubles throughout so w^2*sigma^2 can't overflow with small dispersion.
    {
        double num_off = 0.0, num_jit_var = 0.0, denom = 0.0;
        for (int i = 0; i < n; i++) {
            if (!(best_mask & (1 << i))) continue;
            ntp_peer_t *pp = &g.peers[c[i].idx];
            int32_t aged_disp_us = aged_peer_dispersion_us(pp, now);
            double disp = aged_disp_us > 1 ? aged_disp_us : 1;
            double w    = 1.0 / (disp * disp);
            num_off     += (double)pp->best_offset_us * w;
            num_jit_var += w * w * (double)pp->jitter_us * pp->jitter_us;
            denom       += w;
        }
        if (denom > 0) {
            g.combined_offset_us = (int32_t)(num_off / denom);
            g.system_jitter_us   = (int32_t)(sqrt(num_jit_var) / denom);
        } else {
            g.combined_offset_us = sp->best_offset_us;
            g.system_jitter_us   = sp->jitter_us;
        }
    }
    g.root_dispersion_us = fp1616_to_us(sp->root_dispersion_raw) + sp->dispersion_us;
}

// ---------- discipline ----------

static void discipline_clock(int32_t offset_us) {
    bool step = !g.first_sync_done ||
                offset_us >  STEP_THRESHOLD_US ||
                offset_us < -STEP_THRESHOLD_US;

    if (step) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        tv = tv_from_us(tv_to_us(&tv) + offset_us);
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Clock stepped %+ld us", (long)offset_us);
    } else {
        // Merge with any outstanding slew so we don't overwrite it.
        struct timeval outstanding = {0};
        adjtime(NULL, &outstanding);
        struct timeval delta = tv_from_us(tv_to_us(&outstanding) + offset_us);
        adjtime(&delta, NULL);

        // Integrate crystal frequency slowly from the residual phase error.
        // The residual contains both real oscillator drift and network path
        // asymmetry. Treat it as a noisy measurement, use the actual elapsed
        // time between discipline samples, and cap each update so one WiFi /
        // pool-server outlier cannot move the displayed "Drift" by 10-20 ppm.
        uint32_t now_ms = mono_ms();
        int32_t freq_step = 0;
        bool learned_freq = false;
        if (g.last_freq_sample_ms != 0 &&
            g.system_jitter_us <= FREQ_MAX_JITTER_US &&
            offset_us < FREQ_MAX_OFFSET_US &&
            offset_us > -FREQ_MAX_OFFSET_US) {
            uint32_t elapsed_ms = now_ms - g.last_freq_sample_ms;
            if (elapsed_ms >= (MIN_POLL_S / 2) * 1000) {
                int64_t measured_ppb = ((int64_t)offset_us * 1000000LL) / elapsed_ms;
                freq_step = (int32_t)(measured_ppb >> FREQ_KI_SHIFT);
                freq_step = clamp_i32(freq_step, -MAX_FREQ_STEP_PPB, MAX_FREQ_STEP_PPB);
                g.freq_ppm_x1000 += freq_step;
                g.freq_ppm_x1000 = clamp_i32(g.freq_ppm_x1000,
                                             -MAX_FREQ_PPM_X1000,
                                              MAX_FREQ_PPM_X1000);
                g.freq_learned_this_session = true;
                learned_freq = true;
            }
        }
        g.last_freq_sample_ms = now_ms;

        ESP_LOGI(TAG, "Clock slewed %+ld us (freq est %+ld ppb%s%+ld)",
                 (long)offset_us, (long)g.freq_ppm_x1000,
                 learned_freq ? " step " : " no freq step ",
                 (long)freq_step);

        // Persist the freq estimate to NVS so cold boots don't need to
        // re-converge from 0 ppm. Throttled to once per 30 minutes AND only
        // if the estimate has shifted by >= 0.1 ppm since the last write -
        // keeps flash wear trivial (a handful of writes per day at most)
        // while still tracking slow temperature drift.
        static uint32_t last_freq_save_ms;
        static int32_t  last_saved_freq    = INT32_MIN;
        const  uint32_t SAVE_INTERVAL_MS   = 30 * 60 * 1000;
        const  int32_t  SAVE_DELTA_PPB     = 100;     // 0.1 ppm
        int32_t  freq_delta = g.freq_ppm_x1000 - last_saved_freq;
        if (freq_delta < 0) freq_delta = -freq_delta;
        bool freq_known = g.freq_loaded_from_nvs || g.freq_learned_this_session;
        if (freq_known &&
            ((last_saved_freq == INT32_MIN) ||
             ((now_ms - last_freq_save_ms) >= SAVE_INTERVAL_MS &&
              freq_delta >= SAVE_DELTA_PPB))) {
            nvs_config_set_freq_ppm_x1000(g.freq_ppm_x1000);
            last_saved_freq    = g.freq_ppm_x1000;
            last_freq_save_ms  = now_ms;
        }
    }

    g.first_sync_done = true;
    g.last_offset_us  = offset_us;
    struct timeval now;
    gettimeofday(&now, NULL);
    g.last_sync_time = now.tv_sec;
    g.sync_count++;
}

// ---------- adaptive poll ----------

static void adaptive_poll_update(void) {
    if (!g.first_sync_done) {
        g.current_poll_s = MIN_POLL_S;
        g.poll_adjust = 0;
        return;
    }
    if (g.current_poll_s == 0) g.current_poll_s = MIN_POLL_S;

    // Count consecutive good polls. "Good" = got a response (reach bit 0 set)
    // from the selected peer with bounded jitter. Grow after GOOD_RUN good
    // polls, shrink after BAD_RUN bad ones.
    const int8_t GOOD_RUN = 6;
    const int8_t BAD_RUN  = 2;
    const int32_t JITTER_MAX_US = 100 * 1000;   // 100 ms upper sanity bound

    ntp_peer_t *sp = (g.selected_peer >= 0) ? &g.peers[g.selected_peer] : NULL;
    bool good = sp && (sp->reach & 0x01) && g.system_jitter_us < JITTER_MAX_US;

    if (good) {
        if (g.poll_adjust < 0) g.poll_adjust = 0;
        g.poll_adjust++;
        if (g.poll_adjust >= GOOD_RUN && g.current_poll_s < MAX_POLL_S) {
            g.current_poll_s *= 2;
            g.poll_adjust = 0;
        }
    } else {
        if (g.poll_adjust > 0) g.poll_adjust = 0;
        g.poll_adjust--;
        if (g.poll_adjust <= -BAD_RUN && g.current_poll_s > MIN_POLL_S) {
            g.current_poll_s /= 2;
            g.poll_adjust = 0;
        }
    }

    if (g.current_poll_s < MIN_POLL_S) g.current_poll_s = MIN_POLL_S;
    if (g.current_poll_s > MAX_POLL_S) g.current_poll_s = MAX_POLL_S;
}

// ---------- frequency correction ----------

// Apply accumulated frequency error as a continuous adjtime slew so the clock
// stays close to truth between polls. Called once per main task iteration.
static void apply_freq_correction(void) {
    uint32_t now = mono_ms();
    if (g.last_freq_apply_ms == 0) { g.last_freq_apply_ms = now; return; }
    uint32_t elapsed_ms = now - g.last_freq_apply_ms;
    if (elapsed_ms < 1000) return;   // Apply at most once per second

    // delta_us = (freq_ppm_x1000 / 1000) ppm * (elapsed_ms / 1000) s * 1 us/(ppm*s)
    //          = freq_ppm_x1000 * elapsed_ms / 1_000_000
    int64_t delta_us = ((int64_t)g.freq_ppm_x1000 * elapsed_ms) / 1000000;
    if (delta_us == 0) return;   // Keep last_freq_apply_ms; let rounding accumulate

    // Merge with any outstanding slew so we don't overwrite it.
    struct timeval outstanding = {0};
    adjtime(NULL, &outstanding);
    struct timeval merged_tv = tv_from_us(tv_to_us(&outstanding) + delta_us);
    adjtime(&merged_tv, NULL);
    g.last_freq_apply_ms = now;
}

// ---------- main task ----------

// Discipline gate. Fires when:
//   1. The basic poll interval has elapsed since the last discipline.
//   2. Every reachable peer has settled this wave (responded successfully or
//      timed out) - so combined_offset_us reflects fresh samples from all of
//      them, not one fresh sample plus three from the previous wave (which
//      at MAX_POLL_S is ~17 minutes stale). Peers in KoD or with several
//      consecutive misses don't count toward "reachable" - we don't want
//      one chronically dead peer to block discipline forever.
//   3. At least one of those reachable peers actually responded.
//
// Safety net: if we've been stuck waiting past `threshold + GRACE` (one
// full splay window + response timeout + a small margin), force-fire
// anyway, but only if this wave produced at least one response. Combined data
// may be slightly stale in that case, but better than not disciplining at all.
//
// Called from the response and timeout handlers - both are the only
// places that update last_settle_cycle_id, so they're the only places
// where the gate can transition to "fireable".
static bool try_discipline(uint32_t settled_cycle_id) {
    if (settled_cycle_id == 0 || g.selected_peer < 0) return false;

    uint32_t now = mono_ms();
    uint32_t ref_poll_s = g.last_discipline_poll_s ?
                          g.last_discipline_poll_s : g.current_poll_s;
    int32_t  threshold_ms = ((int32_t)ref_poll_s - 3) * 1000;
    int32_t  since_disc = (int32_t)(now - g.last_discipline_ms);
    bool basic_due = (g.last_discipline_ms == 0) || since_disc >= threshold_ms;
    if (!basic_due) return false;

    bool all_settled = true;
    int  responded   = 0;
    int  reachable   = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *q = &g.peers[i];
        if (!q->active) continue;
        if (q->stratum == 0 || q->stratum >= 16) continue;       // KoD or unsynced
        if (q->kod_until_ms && (int32_t)(now - q->kod_until_ms) < 0) continue;
        if (q->consecutive_misses >= 2) continue;                 // chronically silent
        if (q->cycle_id_when_sent != settled_cycle_id &&
            q->next_poll_cycle_id != settled_cycle_id) continue;  // not part of this wave
        reachable++;
        if (q->cycle_id_when_sent != settled_cycle_id ||
            q->last_settle_cycle_id != settled_cycle_id) {
            all_settled = false;
        }
        if (q->last_settle_cycle_id == settled_cycle_id && (q->reach & 0x01)) responded++;
    }

    const int32_t GRACE_MS = (int32_t)SPLAY_WINDOW_MS + RESPONSE_TIMEOUT_MS + 1000;
    bool overdue = since_disc >= threshold_ms + GRACE_MS;
    bool good    = reachable > 0 && all_settled && responded > 0;
    bool force   = overdue && responded > 0;
    if (!good && !force) return false;

    g.last_discipline_ms     = now;
    g.last_discipline_poll_s = g.current_poll_s;   // captures pre-adaptive value
    discipline_clock(g.combined_offset_us);
    adaptive_poll_update_once(settled_cycle_id);
    return true;
}

static void handle_socket_readable(int sock) {
    // RFC 5905 header is 48 bytes, followed by optional RFC 7822 extension
    // fields and a trailing MAC (for authenticated / NTS packets). We don't
    // validate extensions or MAC, but the buffer is sized generously so a
    // pool peer that sends an extension-laden response doesn't get truncated
    // to an unreadable prefix by a tight buffer.
    uint8_t buf[256];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT,
                        (struct sockaddr *)&from, &fromlen);
    struct timeval t4;
    gettimeofday(&t4, NULL);
    if (n < (ssize_t)sizeof(ntp_pkt_t)) return;

    ntp_peer_t *p = NULL;
    int peer_idx = -1;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *q = &g.peers[i];
        if (!q->active || !q->request_outstanding) continue;
        if (sockaddr_matches(&q->addr, &from)) {
            p = q;
            peer_idx = i;
            break;
        }
    }
    if (!p) return;

    ntp_pkt_t pkt;
    memcpy(&pkt, buf, sizeof(pkt));
    bool ok = process_response(p, &pkt, &t4);
    p->request_outstanding = false;
    p->last_settle_cycle_id = p->cycle_id_when_sent;   // resolved this wave
    uint32_t settled_cycle_id = p->cycle_id_when_sent;
    if (g.selected_peer == peer_idx &&
        (!p->active || p->stratum == 0 || p->stratum >= 16)) {
        g.selected_peer = -1;
        g.stratum = 16;
    }
    if (p->active) {
        schedule_after_request(p);
    }

    if (ok) {
        update_peer_filter(p);
        select_system_peer();

        // Eviction for falseticker / wide-jitter peers is event-driven: the
        // counters we check only move inside select_system_peer, so checking
        // here means one try_replace_peer attempt per peer per poll cycle -
        // naturally cycle-aligned. On failure, the counters stay elevated
        // and we retry at the next cycle's response burst. (consecutive_misses
        // eviction lives in the timeout handler for the same reason.)
        //
        // Thresholds: falseticker = 8 cycles of "outside Marzullo consensus"
        // (systematic wrong-offset peer); wide-jitter = 10 cycles of "jitter
        // > 4* best truechimer". The wide-jitter threshold deliberately
        // exceeds NTP_FILTER_SIZE (8): a single 100 ms+ outlier sample
        // inflates per-peer jitter for the full 8 cycles it takes to age
        // out of the ring, so any threshold <= 8 would evict a peer for one
        // bad sample. 10 requires the high-jitter condition to persist
        // past the filter's self-healing window, confirming it's a real
        // noisy peer rather than transient pathology.
        //
        // At most one eviction per poll cycle, and when multiple peers are
        // eligible we kick the *worst* one (highest summed counter) - not
        // whichever happens to be lowest-index. Bouncing all three at once
        // would fire three DNS queries back-to-back and leave us running
        // with one good peer + three fresh unknowns; spacing them out also
        // gives us a chance to watch whether the first replacement settles
        // the rest (often a noisy selection round also makes decent peers
        // briefly look worse than they are). Any remaining eligible peers
        // stay marked; they'll be picked up on subsequent cycles.
        if (last_evict_tick_ms != next_global_poll_ms) {
            int worst = find_worst_eligible_peer();
            if (worst >= 0 && try_replace_peer(worst)) {
                last_evict_tick_ms = next_global_poll_ms;
                if (g.selected_peer == worst) g.selected_peer = -1;
            }
        }

        try_discipline(settled_cycle_id);
    } else {
        try_discipline(settled_cycle_id);
    }
}

static void ntp_task(void *arg) {
    (void)arg;
    g.sync_start_ms = mono_ms();
    g.current_poll_s = MIN_POLL_S;
    open_wake_sock();

    while (g.running) {
        lock_take();

        apply_freq_correction();

        // Staleness watchdog: if we've heard from no peer for several poll
        // cycles, shrink the poll interval back toward MIN_POLL_S and re-resolve
        // DNS. Handles the case where all cached pool IPs went away (network
        // change, pool rotation, etc.) - otherwise we'd keep retrying the same
        // dead addresses on a grown poll cap forever.
        if (g.last_any_response_ms != 0) {
            uint32_t dead_ms = mono_ms() - g.last_any_response_ms;
            uint32_t threshold_ms = g.current_poll_s * 4 * 1000;
            if (dead_ms > threshold_ms) {
                ESP_LOGW(TAG, "No peer responses in %lus; re-resolving, poll %lus -> %ds",
                         (unsigned long)(dead_ms / 1000),
                         (unsigned long)g.current_poll_s, MIN_POLL_S);
                g.dirty_config    = true;
                g.current_poll_s  = MIN_POLL_S;
                g.last_discipline_poll_s = 0;  // fall back to current_poll_s next check
                g.last_freq_sample_ms = 0;      // don't learn drift across a no-response gap
                g.poll_adjust     = 0;
                g.last_any_response_ms = mono_ms();  // avoid immediate retrigger
            }
        }

        if (g.dirty_config) {
            g.dirty_config = false;
            next_global_poll_ms = 0;
            next_global_poll_cycle_id++;
            if (next_global_poll_cycle_id == 0) next_global_poll_cycle_id = 1;
            last_evict_tick_ms = UINT32_MAX;
            last_poll_adjust_cycle_id = 0;
            resolve_peers();
            // Arm the staleness watchdog against this resolution: even before
            // any peer responds, we'll retrigger after threshold_ms if the new
            // set of IPs is also silent (e.g. DNS gave stale results).
            g.last_any_response_ms = mono_ms();
            // Reset adaptive poll so the new peer set gets a fresh ramp.
            // Crystal drift estimate (freq_ppm_x1000) is hardware-intrinsic
            // and stays, so we don't lose inter-poll accuracy during re-sync.
            g.current_poll_s         = MIN_POLL_S;
            g.last_discipline_poll_s = 0;
            g.last_freq_sample_ms    = 0;       // skip first freq update on the new peer set
            g.poll_adjust            = 0;
            // Reset sync accounting so the drilldown doesn't display a stale
            // Syncs count / Age value tied to the previous server. Keep
            // first_sync_done so we don't re-step the clock on the first
            // response from the new server - the local clock is already set.
            g.sync_count             = 0;
            g.last_sync_time         = 0;
        }
        if (!open_sockets()) {
            lock_give();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = mono_ms();
        uint32_t next_wake = now + IDLE_WAKE_MS;

        // Dispatch due requests; compute soonest deadline.
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *p = &g.peers[i];
            if (!p->active) continue;

            // Honor KoD cooldown
            if (p->kod_until_ms && (int32_t)(now - p->kod_until_ms) < 0) {
                if ((int32_t)(p->kod_until_ms - next_wake) < 0) next_wake = p->kod_until_ms;
                continue;
            }

            // Timeout outstanding requests
            if (p->request_outstanding &&
                (int32_t)(now - p->request_sent_ms) >= RESPONSE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "TIMEOUT peer=%s reach=%02x", p->addr_str, p->reach);
                uint32_t settled_cycle_id = p->cycle_id_when_sent;
                p->request_outstanding   = false;
                p->last_settle_cycle_id  = settled_cycle_id;  // resolved (badly)
                if (p->consecutive_misses < 255) p->consecutive_misses++;
                schedule_after_request(p);
                // Swap out chronically-bad peers. Trigger is event-driven
                // (this peer just crossed its miss threshold), but the peer
                // we actually evict is the WORST currently-eligible one - it
                // might be another peer with higher falseticker/jittery
                // runs than this one's misses. Gated by last_evict_tick_ms
                // so we only swap one peer per poll tick (see the same gate
                // in handle_socket_readable).
                if (p->consecutive_misses >= 4 &&
                    last_evict_tick_ms != next_global_poll_ms) {
                    int worst = find_worst_eligible_peer();
                    if (worst >= 0 && try_replace_peer(worst)) {
                        last_evict_tick_ms = next_global_poll_ms;
                        if (g.selected_peer == worst) g.selected_peer = -1;
                    }
                }
                // Re-evaluate the discipline gate: this timeout may have
                // been the last unresolved peer of the wave (the response
                // path won't fire again for it).
                bool disciplined = try_discipline(settled_cycle_id);
                // If no discipline fired, still treat a selected-peer timeout
                // as a bad poll-adjust event so the interval can shrink.
                if (!disciplined && g.selected_peer == i) {
                    adaptive_poll_update_once(settled_cycle_id);
                }
            }

            bool due = g.force_sync || (int32_t)(now - p->next_poll_ms) >= 0;
            if (due && !p->request_outstanding) {
                if (send_request(p)) {
                    uint32_t deadline = p->request_sent_ms + RESPONSE_TIMEOUT_MS;
                    if ((int32_t)(deadline - next_wake) < 0) next_wake = deadline;
                } else {
                    if ((int32_t)(p->next_poll_ms - next_wake) < 0) next_wake = p->next_poll_ms;
                }
            } else if (p->request_outstanding) {
                uint32_t deadline = p->request_sent_ms + RESPONSE_TIMEOUT_MS;
                if ((int32_t)(deadline - next_wake) < 0) next_wake = deadline;
            } else {
                if ((int32_t)(p->next_poll_ms - next_wake) < 0) next_wake = p->next_poll_ms;
            }
        }
        g.force_sync = false;

        // Wait for responses up to next_wake.
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (g.sock4 >= 0)     { FD_SET(g.sock4, &rfds);     if (g.sock4 > maxfd)     maxfd = g.sock4; }
        if (g.sock6 >= 0)     { FD_SET(g.sock6, &rfds);     if (g.sock6 > maxfd)     maxfd = g.sock6; }
        if (g.wake_sock >= 0) { FD_SET(g.wake_sock, &rfds); if (g.wake_sock > maxfd) maxfd = g.wake_sock; }

        uint32_t wait_ms = (int32_t)(next_wake - now) > 0 ? (next_wake - now) : 0;
        if (wait_ms > IDLE_WAKE_MS) wait_ms = IDLE_WAKE_MS;

        lock_give();

        struct timeval tv = {
            .tv_sec  = wait_ms / 1000,
            .tv_usec = (wait_ms % 1000) * 1000,
        };
        int sr = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (sr > 0) {
            lock_take();
            if (g.sock4 >= 0     && FD_ISSET(g.sock4, &rfds))     handle_socket_readable(g.sock4);
            if (g.sock6 >= 0     && FD_ISSET(g.sock6, &rfds))     handle_socket_readable(g.sock6);
            if (g.wake_sock >= 0 && FD_ISSET(g.wake_sock, &rfds)) drain_wake_sock();
            lock_give();
        }
    }

    close_sockets();
    close_wake_sock();
    g.task = NULL;
    vTaskDelete(NULL);
}

// ---------- public API ----------

void ntp_init(const char *server, bool prefer_ipv6) {
    if (g.running) ntp_stop();
    if (!g.lock) g.lock = xSemaphoreCreateMutex();

    strncpy(g.server, server ? server : "pool.ntp.org", sizeof(g.server) - 1);
    g.server[sizeof(g.server) - 1] = '\0';
    g.prefer_ipv6    = prefer_ipv6;
    g.current_poll_s = MIN_POLL_S;
    g.first_sync_done = false;
    g.sync_count     = 0;
    g.selected_peer  = -1;
    g.stratum        = 16;
    g.dirty_config   = true;
    g.force_sync     = false;
    g.last_sync_time = 0;
    g.last_offset_us = 0;
    g.system_jitter_us = 0;
    g.root_delay_us = 0;
    g.root_dispersion_us = 0;
    g.combined_offset_us = 0;
    g.last_freq_apply_ms = 0;
    g.last_freq_sample_ms = 0;
    g.last_discipline_ms = 0;
    g.last_discipline_poll_s = 0;
    g.last_any_response_ms = 0;
    g.poll_adjust = 0;
    next_global_poll_ms = 0;
    next_global_poll_cycle_id++;
    if (next_global_poll_cycle_id == 0) next_global_poll_cycle_id = 1;
    last_evict_tick_ms = UINT32_MAX;
    last_poll_adjust_cycle_id = 0;
    // Restore the persisted crystal-drift estimate so we start near-converged
    // instead of the ~30 minutes the PI loop normally needs to settle on the
    // hardware-intrinsic value from a cold 0 ppm seed.
    int32_t saved_freq = 0;
    g.freq_ppm_x1000 = 0;
    g.freq_loaded_from_nvs = false;
    g.freq_learned_this_session = false;
    if (nvs_config_get_freq_ppm_x1000(&saved_freq) &&
        saved_freq >  -MAX_FREQ_PPM_X1000 &&
        saved_freq <   MAX_FREQ_PPM_X1000) {
        g.freq_ppm_x1000       = saved_freq;
        g.freq_loaded_from_nvs = true;
        ESP_LOGI(TAG, "Restored freq estimate: %+ld ppb", (long)saved_freq);
    }

    g.running = true;
    if (xTaskCreate(ntp_task, "ntp", 4096, NULL, 5, &g.task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NTP task");
        g.running = false;
        g.task = NULL;
    }
}

void ntp_stop(void) {
    if (!g.running) return;
    g.running = false;
    wake_task();
    while (g.task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ntp_set_server(const char *server) {
    if (!g.lock || !server) return;
    lock_take();
    strncpy(g.server, server, sizeof(g.server) - 1);
    g.server[sizeof(g.server) - 1] = '\0';
    g.dirty_config = true;
    g.force_sync   = true;
    lock_give();
    wake_task();
}

void ntp_set_prefer_ipv6(bool prefer) {
    if (!g.lock) return;
    lock_take();
    bool changed = (g.prefer_ipv6 != prefer);
    if (changed) {
        g.prefer_ipv6 = prefer;
        g.dirty_config = true;
        g.force_sync   = true;
    }
    lock_give();
    if (changed) wake_task();
}

void ntp_get_sys_stats(ntp_sys_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g.lock) { out->stratum = 16; out->selected_peer = 0xFF; return; }

    lock_take();
    out->synced         = g.first_sync_done;
    out->last_sync_time = g.last_sync_time;
    out->sync_count     = g.sync_count;
    out->current_poll_s = g.current_poll_s;
    out->sync_elapsed_ms = g.first_sync_done ? 0 : (mono_ms() - g.sync_start_ms);
    out->last_offset_us = g.last_offset_us;
    out->system_jitter_us    = g.system_jitter_us;
    out->root_delay_us       = g.root_delay_us;
    // Age dispersion by PHI * seconds since we last heard from the selected
    // peer so the reported +/- bound grows honestly between polls.
    out->root_dispersion_us  = g.root_dispersion_us;
    if (g.selected_peer >= 0) {
        ntp_peer_t *sp = &g.peers[g.selected_peer];
        out->root_dispersion_us = fp1616_to_us(sp->root_dispersion_raw) +
                                  aged_peer_dispersion_us(sp, mono_ms());
    }
    out->freq_ppm_x1000      = g.freq_ppm_x1000;
    out->freq_known          = g.freq_loaded_from_nvs || g.freq_learned_this_session;
    out->stratum        = g.stratum;
    out->selected_peer  = (g.selected_peer < 0) ? 0xFF : (uint8_t)g.selected_peer;
    strncpy(out->server, g.server, sizeof(out->server) - 1);
    out->server[sizeof(out->server) - 1] = '\0';
    lock_give();
}

bool ntp_get_peer_stats(int idx, ntp_peer_stats_t *out) {
    if (!out || idx < 0 || idx >= NTP_MAX_PEERS || !g.lock) return false;
    lock_take();
    ntp_peer_t *p = &g.peers[idx];
    if (!p->active) { lock_give(); return false; }

    memset(out, 0, sizeof(*out));
    out->active    = true;
    out->selected  = (g.selected_peer == idx);
    out->stratum   = p->stratum;
    out->reach     = p->reach;
    out->offset_us     = p->best_offset_us;
    out->delay_us      = p->best_delay_us;
    out->jitter_us     = p->jitter_us;
    out->dispersion_us = aged_peer_dispersion_us(p, mono_ms());
    out->last_response_ms = p->last_response_ms
        ? (mono_ms() - p->last_response_ms)
        : UINT32_MAX;
    out->fresh = (int32_t)(mono_ms() - p->fresh_until_ms) < 0;
    strncpy(out->addr_str, p->addr_str, sizeof(out->addr_str) - 1);
    lock_give();
    return true;
}

void ntp_get_primary_addr_str(char *buf, size_t len) {
    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!g.lock) return;
    lock_take();
    int idx = g.selected_peer;
    if (idx < 0) {
        // Fall back to first active peer so the UI shows something while resolving.
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            if (g.peers[i].active) { idx = i; break; }
        }
    }
    if (idx >= 0) {
        strncpy(buf, g.peers[idx].addr_str, len - 1);
        buf[len - 1] = '\0';
    }
    lock_give();
}
