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
#include "esp_random.h"
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

// Discipline gains (integer shifts, i.e. powers of two)
#define PLL_KI_SHIFT         6            // freq integrator gain at MIN_POLL_S
#define MAX_FREQ_PPM_X1000   500000       // clamp +/-500 ppm

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
        if (precision >= 31) return INT32_MAX;
        return (int32_t)(1000000LL << precision);
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

    uint32_t next_poll_ms;
    uint32_t kod_until_ms;
    uint8_t  consecutive_misses;   // polls since last response; trigger swap at threshold
    uint8_t  falseticker_runs;     // consecutive cycles outside Marzullo intersection
    uint8_t  jittery_runs;         // consecutive cycles substantially noisier than best truechimer
    uint32_t fresh_until_ms;       // mono deadline; UI paints row green until this time
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
    uint32_t last_freq_apply_ms;
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
    int qlen = dns_build_query(qbuf, sizeof(qbuf), esp_random() & 0xFFFF, host, qtype);
    if (qlen <= 0) return count;
    if (sendto(sock, qbuf, qlen, 0, (const struct sockaddr *)dst, sizeof(*dst)) != qlen) {
        return count;
    }
    ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
    if (n < 12) return count;
    return dns_parse_answers(rbuf, (int)n, out, count, max);
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

// Shared "next poll tick" all peers align to (see schedule_after_request for
// the rationale). Declared up here because peer_reset reads it to anchor the
// per-peer fresh-window expiry.
static uint32_t next_global_poll_ms;

// next_global_poll_ms value at the last successful try_replace_peer. Used as
// a one-eviction-per-cycle rate limit - equality means we've already swapped
// a peer during this poll tick and should defer the rest. Initialized to a
// sentinel that can't match next_global_poll_ms until the second-ish after
// cold boot, so the first eviction attempt always gets through.
static uint32_t last_evict_tick_ms = UINT32_MAX;

static void peer_reset(ntp_peer_t *p) {
    memset(p, 0, sizeof(*p));
    p->stratum = 16;
    // Paint the UI row green until the next global poll tick (end of the
    // cycle we're installed into). If the tick hasn't been set yet (cold
    // boot, before any response) fall back to one full poll from now. We
    // snapshot the value here so subsequent cycle advances don't keep
    // extending the window - a peer installed mid-cycle should only be
    // green for the REMAINDER of that cycle.
    uint32_t now = mono_ms();
    if (next_global_poll_ms != 0 &&
        (int32_t)(next_global_poll_ms - now) > 0) {
        p->fresh_until_ms = next_global_poll_ms;
    } else {
        p->fresh_until_ms = now + g.current_poll_s * 1000;
    }
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
// at (roughly) the same instant and their ages stay within a few hundred ms.
// (Declared up by peer_reset - that function captures the current tick as
// the per-peer fresh-window expiry at install time.)

static void schedule_after_request(ntp_peer_t *p) {
    uint32_t now = mono_ms();
    uint32_t interval_ms = g.current_poll_s * 1000;
    // If the shared tick has already passed (or hasn't been set yet), advance
    // it to one interval from now. Subsequent peers scheduling in the same
    // cycle will see it still in the future and use it as-is.
    if ((int32_t)(now - next_global_poll_ms) >= 0) {
        next_global_poll_ms = now + interval_ms;
    }
    // Stagger each peer by a fixed offset so we don't send all packets in a
    // burst. 250 ms * peer_index gives ~750 ms spread for 4 peers - still
    // rounds to the same second in the age display, but the sends are
    // comfortably far apart to avoid any batching in the WiFi/TCP-IP stack.
    int idx = (int)(p - g.peers);
    p->next_poll_ms = next_global_poll_ms + (uint32_t)idx * 250;
}

static void send_request(ntp_peer_t *p) {
    ntp_pkt_t pkt = {0};
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.poll       = 6;
    pkt.precision  = LOCAL_PRECISION;

    int sock = (p->addr.ss_family == AF_INET6) ? g.sock6 : g.sock4;
    if (sock < 0) return;

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
        schedule_after_request(p);
        return;
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

    // Mutable copy of t4 so the first-sync branch can refresh it post-step.
    struct timeval t4_local = *t4;

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
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *q = &g.peers[i];
            if (!q->request_outstanding) continue;
            int64_t t1_us = (int64_t)q->t1.tv_sec * 1000000LL + q->t1.tv_usec + step_us;
            q->t1.tv_sec  = (time_t)(t1_us / 1000000);
            q->t1.tv_usec = (suseconds_t)(t1_us % 1000000);
            if (q->t1.tv_usec < 0) { q->t1.tv_sec--; q->t1.tv_usec += 1000000; }
        }

        // Step the local clock by step_us: target = now + step_us.
        struct timeval now_pre, target;
        gettimeofday(&now_pre, NULL);
        int64_t target_us = (int64_t)now_pre.tv_sec * 1000000LL +
                            now_pre.tv_usec + step_us;
        target.tv_sec  = (time_t)(target_us / 1000000);
        target.tv_usec = (suseconds_t)(target_us % 1000000);
        if (target.tv_usec < 0) { target.tv_sec--; target.tv_usec += 1000000; }
        settimeofday(&target, NULL);
        // Refresh t4 to the post-step clock frame so the offset math below
        // lands in the same frame as the shifted t1/t2/t3.
        gettimeofday(&t4_local, NULL);

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
        // Fall through: compute offset/delay for this peer against the
        // post-step frame and feed into the normal filter path.
    }

    int64_t offset = (tv_diff_us(&t2, &p->t1) + tv_diff_us(&t3, &t4_local)) / 2;
    int64_t delay  = tv_diff_us(&t4_local, &p->t1) - tv_diff_us(&t3, &t2);

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
    int64_t rtt_us  = tv_diff_us(&t4_local, &p->t1);
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
    int n = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active || p->reach == 0) continue;
        if (p->stratum == 0 || p->stratum >= 16) continue;
        int32_t unc = p->best_delay_us / 2 + p->jitter_us + p->dispersion_us;
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
            double disp = pp->dispersion_us > 1 ? pp->dispersion_us : 1;
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
        int64_t total_us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec + offset_us;
        tv.tv_sec  = (time_t)(total_us / 1000000);
        tv.tv_usec = (suseconds_t)(total_us % 1000000);
        if (tv.tv_usec < 0) { tv.tv_sec--; tv.tv_usec += 1000000; }
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Clock stepped %+ld us", (long)offset_us);
    } else {
        // Merge with any outstanding slew so we don't overwrite it.
        struct timeval outstanding = {0};
        adjtime(NULL, &outstanding);
        int64_t merged_us = (int64_t)outstanding.tv_sec * 1000000LL +
                            outstanding.tv_usec + offset_us;
        struct timeval delta = {
            .tv_sec  = (time_t)(merged_us / 1000000),
            .tv_usec = (suseconds_t)(merged_us % 1000000),
        };
        if (delta.tv_usec < 0) { delta.tv_sec--; delta.tv_usec += 1000000; }
        adjtime(&delta, NULL);

        // Integrate frequency error: offset accumulated over poll interval.
        // Scale the gain shift with poll so the effective time constant stays
        // roughly constant in wall-clock seconds (~2^PLL_KI_SHIFT * MIN_POLL_S).
        // At poll=MIN_POLL_S we use the baseline 1/64; each doubling of poll
        // cuts the shift by 1, so at poll=MAX_POLL_S=1024 the shift is 1
        // (gain 1/2) - still correct since samples come 32* less often.
        int32_t poll_s = g.current_poll_s ? (int32_t)g.current_poll_s : MIN_POLL_S;
        int shift = PLL_KI_SHIFT;
        for (int32_t p = poll_s; p > MIN_POLL_S && shift > 1; p >>= 1) shift--;
        int32_t inc = (offset_us * 1000) / poll_s;       // ppb drift rate
        g.freq_ppm_x1000 += inc >> shift;
        if (g.freq_ppm_x1000 >  MAX_FREQ_PPM_X1000) g.freq_ppm_x1000 =  MAX_FREQ_PPM_X1000;
        if (g.freq_ppm_x1000 < -MAX_FREQ_PPM_X1000) g.freq_ppm_x1000 = -MAX_FREQ_PPM_X1000;

        ESP_LOGI(TAG, "Clock slewed %+ld us (freq est %+ld ppb)",
                 (long)offset_us, (long)g.freq_ppm_x1000);

        // Persist the freq estimate to NVS so cold boots don't need to
        // re-converge from 0 ppm. Throttled to once per 30 minutes AND only
        // if the estimate has shifted by >= 0.1 ppm since the last write -
        // keeps flash wear trivial (a handful of writes per day at most)
        // while still tracking slow temperature drift.
        static uint32_t last_freq_save_ms;
        static int32_t  last_saved_freq    = INT32_MIN;
        const  uint32_t SAVE_INTERVAL_MS   = 30 * 60 * 1000;
        const  int32_t  SAVE_DELTA_PPB     = 100;     // 0.1 ppm
        uint32_t now_ms     = mono_ms();
        int32_t  freq_delta = g.freq_ppm_x1000 - last_saved_freq;
        if (freq_delta < 0) freq_delta = -freq_delta;
        if ((last_saved_freq == INT32_MIN) ||
            ((now_ms - last_freq_save_ms) >= SAVE_INTERVAL_MS &&
             freq_delta >= SAVE_DELTA_PPB)) {
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
    int64_t merged = (int64_t)outstanding.tv_sec * 1000000LL +
                     outstanding.tv_usec + delta_us;
    struct timeval merged_tv = {
        .tv_sec  = (time_t)(merged / 1000000),
        .tv_usec = (suseconds_t)(merged % 1000000),
    };
    if (merged_tv.tv_usec < 0) { merged_tv.tv_sec--; merged_tv.tv_usec += 1000000; }
    adjtime(&merged_tv, NULL);
    g.last_freq_apply_ms = now;
}

// ---------- main task ----------

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
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *q = &g.peers[i];
        if (!q->active || !q->request_outstanding) continue;
        if (sockaddr_matches(&q->addr, &from)) { p = q; break; }
    }
    if (!p) return;

    ntp_pkt_t pkt;
    memcpy(&pkt, buf, sizeof(pkt));
    bool ok = process_response(p, &pkt, &t4);
    p->request_outstanding = false;
    schedule_after_request(p);

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

        // Discipline once per poll cycle from the dispersion-weighted
        // combined offset (g.combined_offset_us is refreshed by every call
        // to select_system_peer). Gating on "this peer happens to be the
        // selected one post-update" is too fragile with combining: when all
        // survivors have similar jitter, each peer's own fresh sample often
        // bumps its jitter just enough that select picks a different peer -
        // which could leave a whole cycle with no discipline firing at all.
        //
        // Rate limit: require since_disc to be within a few seconds of the
        // full poll interval so only cycle-boundary responses trigger (a
        // looser half-cycle gate would also let off-schedule responses
        // through - e.g. try_replace_peer's immediate mid-cycle poll).
        //
        // Reference the poll that was in effect at the previous discipline,
        // not the current one - schedule_after_request (above) locked in the
        // upcoming cycle length using the pre-adaptive value, so the interval
        // that just elapsed is last_discipline_poll_s, not the (possibly
        // already-doubled) current_poll_s.
        uint32_t now = mono_ms();
        uint32_t ref_poll_s = g.last_discipline_poll_s ?
                              g.last_discipline_poll_s : g.current_poll_s;
        int32_t  threshold_ms = ((int32_t)ref_poll_s - 3) * 1000;
        bool due = (g.last_discipline_ms == 0) ||
                   (int32_t)(now - g.last_discipline_ms) >= threshold_ms;
        if (g.selected_peer >= 0 && due) {
            g.last_discipline_ms     = now;
            g.last_discipline_poll_s = g.current_poll_s;   // captures pre-adaptive value
            discipline_clock(g.combined_offset_us);
            adaptive_poll_update();
        }
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
                g.poll_adjust     = 0;
                g.last_any_response_ms = mono_ms();  // avoid immediate retrigger
            }
        }

        if (g.dirty_config) {
            g.dirty_config = false;
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
                p->request_outstanding = false;
                if (p->consecutive_misses < 255) p->consecutive_misses++;
                schedule_after_request(p);
                // If the selected peer timed out, treat as a "bad" poll-adjust
                // event so the poll interval has a chance to shrink.
                if (g.selected_peer == i) {
                    adaptive_poll_update();
                }
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
            }

            bool due = g.force_sync || (int32_t)(now - p->next_poll_ms) >= 0;
            if (due && !p->request_outstanding) {
                send_request(p);
                uint32_t deadline = p->request_sent_ms + RESPONSE_TIMEOUT_MS;
                if ((int32_t)(deadline - next_wake) < 0) next_wake = deadline;
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
    // Restore the persisted crystal-drift estimate so we start near-converged
    // instead of the ~30 minutes the PI loop normally needs to settle on the
    // hardware-intrinsic value from a cold 0 ppm seed.
    int32_t saved_freq = 0;
    if (nvs_config_get_freq_ppm_x1000(&saved_freq) &&
        saved_freq >  -MAX_FREQ_PPM_X1000 &&
        saved_freq <   MAX_FREQ_PPM_X1000) {
        g.freq_ppm_x1000 = saved_freq;
        ESP_LOGI(TAG, "Restored freq estimate: %+ld ppb", (long)saved_freq);
    }

    g.running = true;
    xTaskCreate(ntp_task, "ntp", 4096, NULL, 5, &g.task);
}

void ntp_stop(void) {
    if (!g.running) return;
    g.running = false;
    // The task polls `running` after each select cycle.
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
        uint32_t last = g.peers[g.selected_peer].last_response_ms;
        if (last != 0) {
            uint32_t age_ms = mono_ms() - last;
            out->root_dispersion_us += (int32_t)((uint64_t)PHI_US_PER_SEC * age_ms / 1000);
        }
    }
    out->freq_ppm_x1000      = g.freq_ppm_x1000;
    out->stratum        = g.stratum;
    out->selected_peer  = (g.selected_peer < 0) ? 0xFF : (uint8_t)g.selected_peer;
    out->server         = g.server;
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
    out->dispersion_us = p->dispersion_us;
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
