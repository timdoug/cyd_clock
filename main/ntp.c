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

static const char *TAG = "ntp";

// Protocol
#define NTP_PORT             123
#define NTP_VERSION          4
#define NTP_MODE_CLIENT      3
#define NTP_MODE_SERVER      4
#define NTP_PKT_SIZE         48
#define NTP_EPOCH_OFFSET     2208988800UL  // 1900 -> 1970 in seconds
#define NTP_FILTER_SIZE      8

// Timing
#define MIN_POLL_S           32
#define RESPONSE_TIMEOUT_MS  2500
#define STEP_THRESHOLD_US    (128LL * 1000)
#define PANIC_THRESHOLD_S    1000
#define KOD_BACKOFF_MS       (3600UL * 1000)
#define IDLE_WAKE_MS         60000

// Discipline gains (integer shifts, i.e. powers of two)
#define PLL_KI_SHIFT         6            // freq integrator gain = 1/64
#define MAX_FREQ_PPM_X1000   500000       // clamp +/-500 ppm

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
} ntp_peer_t;

static struct {
    char     server[64];
    bool     prefer_ipv6;
    uint32_t max_interval_s;
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
    int32_t  freq_ppm_x1000;
    uint32_t last_freq_apply_ms;
    uint32_t last_discipline_ms;
    int8_t   poll_adjust;   // counter: +N grows poll, -N shrinks

    int      sock4;
    int      sock6;

    TaskHandle_t task;
    SemaphoreHandle_t lock;
    bool     running;
    bool     force_sync;
    bool     dirty_config;
} g = {
    .selected_peer = -1,
    .stratum = 16,
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
    tv->tv_sec  = (time_t)(sec - NTP_EPOCH_OFFSET);
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

// ---------- DNS (multi-record) ----------

// LWIP's getaddrinfo only surfaces the first A/AAAA record, even though
// pool.ntp.org typically returns several. Talk to the system resolver directly
// so we can harvest all records and use them as distinct NTP peers.

#define DNS_PORT           53
#define DNS_TYPE_A         1
#define DNS_TYPE_AAAA      28
#define DNS_CLASS_IN       1
#define DNS_RECV_TIMEOUT_S 2

static bool sockaddr_matches(const struct sockaddr_storage *a,
                             const struct sockaddr_storage *b);

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

    int count = 0;
    uint16_t first  = prefer_ipv6 ? DNS_TYPE_AAAA : DNS_TYPE_A;
    uint16_t second = prefer_ipv6 ? DNS_TYPE_A    : DNS_TYPE_AAAA;
    count = dns_query_one(sock, &dst, host, first,  out, count, max);
    if (count < max) {
        count = dns_query_one(sock, &dst, host, second, out, count, max);
    }
    close(sock);
    return count;
}

// ---------- peer management ----------

static void peer_reset(ntp_peer_t *p) {
    memset(p, 0, sizeof(*p));
    p->stratum = 16;
}

static int resolve_peers(void) {
    for (int i = 0; i < NTP_MAX_PEERS; i++) peer_reset(&g.peers[i]);
    g.selected_peer = -1;
    g.stratum = 16;

    struct sockaddr_storage addrs[NTP_MAX_PEERS];
    int n = dns_resolve_all(g.server, g.prefer_ipv6, addrs, NTP_MAX_PEERS);

    // Fallback to LWIP getaddrinfo if direct DNS failed (e.g. v6-only resolver).
    if (n == 0) {
        struct addrinfo hints = {
            .ai_family   = AF_UNSPEC,
            .ai_socktype = SOCK_DGRAM,
            .ai_protocol = IPPROTO_UDP,
        };
        struct addrinfo *res = NULL;
        if (getaddrinfo(g.server, "123", &hints, &res) == 0) {
            for (struct addrinfo *ai = res; ai && n < NTP_MAX_PEERS; ai = ai->ai_next) {
                if (ai->ai_addrlen > sizeof(addrs[0])) continue;
                memcpy(&addrs[n], ai->ai_addr, ai->ai_addrlen);
                n++;
            }
            if (res) freeaddrinfo(res);
        }
    }

    if (n == 0) {
        ESP_LOGW(TAG, "DNS failed for %s", g.server);
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

// ---------- send ----------

static void send_request(ntp_peer_t *p) {
    ntp_pkt_t pkt = {0};
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.poll       = 6;
    pkt.precision  = -18;   // ~4 us

    struct timeval t1;
    gettimeofday(&t1, NULL);
    uint32_t sec, frac;
    tv_to_ntp(&t1, &sec, &frac);
    frac ^= esp_random();   // random low bits -> replay protection / fingerprint

    pkt.xmt_ts_sec  = htonl(sec);
    pkt.xmt_ts_frac = htonl(frac);

    int sock = (p->addr.ss_family == AF_INET6) ? g.sock6 : g.sock4;
    if (sock < 0) return;

    ssize_t n = sendto(sock, &pkt, sizeof(pkt), 0,
                       (struct sockaddr *)&p->addr, p->addr_len);
    if (n != sizeof(pkt)) {
        ESP_LOGW(TAG, "sendto %s failed (errno=%d)", p->addr_str, errno);
        return;
    }

    p->t1 = t1;
    p->xmt_sec_net  = pkt.xmt_ts_sec;
    p->xmt_frac_net = pkt.xmt_ts_frac;
    p->reach <<= 1;        // new poll starts with bit 0 = 0 until response
    p->request_outstanding = true;
    p->request_sent_ms     = mono_ms();
    ESP_LOGI(TAG, "SEND peer=%s reach=%02x next_in=%lds poll_s=%lu",
             p->addr_str, p->reach,
             (long)((int32_t)(p->next_poll_ms - p->request_sent_ms) / 1000),
             (unsigned long)g.current_poll_s);
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
    ESP_LOGI(TAG, "FILTER peer=%s valid=%d reach=%02x off=%+ldus delay=%ldus jitter=%ldus",
             p->addr_str, valid, p->reach,
             (long)p->best_offset_us, (long)p->best_delay_us, (long)p->jitter_us);
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

    // Cold boot: system time is at epoch, server is decades ahead. Offset
    // would overflow int32_t sample storage, so bypass the filter and step
    // the clock straight to the server's transmit timestamp. The next poll
    // produces a sane sample that flows through the normal path.
    if (!g.first_sync_done) {
        settimeofday(&t3, NULL);
        p->stratum = pkt->stratum;
        p->last_response_ms = mono_ms();
        p->reach |= 1;
        g.first_sync_done = true;
        g.sync_count++;
        g.last_sync_time = t3.tv_sec;
        g.last_offset_us = 0;
        ESP_LOGI(TAG, "Initial time set from %s (stratum %d)",
                 p->addr_str, pkt->stratum);
        return false;   // don't run filter/select/discipline on this sample
    }

    int64_t offset = (tv_diff_us(&t2, &p->t1) + tv_diff_us(&t3, t4)) / 2;
    int64_t delay  = tv_diff_us(t4, &p->t1) - tv_diff_us(&t3, &t2);

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

    p->filter_head = (p->filter_head + 1) % NTP_FILTER_SIZE;
    p->filter[p->filter_head].offset_us    = (int32_t)offset;
    p->filter[p->filter_head].delay_us     = (int32_t)delay;
    p->filter[p->filter_head].dispersion_us = 10000;   // initial per-sample dispersion
    p->filter[p->filter_head].valid        = true;

    p->stratum         = pkt->stratum;
    p->precision       = pkt->precision;
    p->root_delay_raw      = ntohl(pkt->root_delay);
    p->root_dispersion_raw = ntohl(pkt->root_dispersion);
    p->last_response_ms = mono_ms();
    p->reach |= 1;
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
        if (!(best_mask & (1 << i))) continue;
        ntp_peer_t *p = &g.peers[c[i].idx];
        if (p->jitter_us < best_jitter) {
            best_jitter = p->jitter_us;
            chosen = c[i].idx;
        }
    }
    if (chosen < 0) { g.selected_peer = -1; g.stratum = 16; return; }

    ntp_peer_t *sp = &g.peers[chosen];
    int prev = g.selected_peer;
    g.selected_peer     = chosen;
    g.stratum           = (sp->stratum < 15) ? sp->stratum + 1 : 15;
    g.system_jitter_us  = sp->jitter_us;
    g.root_delay_us     = fp1616_to_us(sp->root_delay_raw) + sp->best_delay_us;
    g.root_dispersion_us = fp1616_to_us(sp->root_dispersion_raw) + sp->dispersion_us;
    ESP_LOGI(TAG, "SELECT candidates=%d survivors=%d chosen=%s jitter=%ldus%s",
             n, best_count, sp->addr_str, (long)sp->jitter_us,
             prev != chosen ? " (CHANGED)" : "");
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

        // Integrate frequency error: offset accumulated over poll interval
        int32_t poll_s = g.current_poll_s ? (int32_t)g.current_poll_s : MIN_POLL_S;
        int32_t inc = (offset_us * 1000) / poll_s;       // ppm * 1000 contribution
        g.freq_ppm_x1000 += inc >> PLL_KI_SHIFT;
        if (g.freq_ppm_x1000 >  MAX_FREQ_PPM_X1000) g.freq_ppm_x1000 =  MAX_FREQ_PPM_X1000;
        if (g.freq_ppm_x1000 < -MAX_FREQ_PPM_X1000) g.freq_ppm_x1000 = -MAX_FREQ_PPM_X1000;

        ESP_LOGI(TAG, "Clock slewed %+ld us (freq est %+ld ppb)",
                 (long)offset_us, (long)g.freq_ppm_x1000);
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
    uint32_t prev_poll = g.current_poll_s;
    int8_t   prev_adj  = g.poll_adjust;

    if (good) {
        if (g.poll_adjust < 0) g.poll_adjust = 0;
        g.poll_adjust++;
        if (g.poll_adjust >= GOOD_RUN && g.current_poll_s < g.max_interval_s) {
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
    if (g.current_poll_s > g.max_interval_s) g.current_poll_s = g.max_interval_s;

    const char *why = good ? "good" : "bad";
    const char *why2 = "";
    if (!sp) why2 = " no_sel";
    else if (!(sp->reach & 0x01)) why2 = " reach_miss";
    else if (g.system_jitter_us >= JITTER_MAX_US) why2 = " jitter_hi";
    ESP_LOGI(TAG, "POLL_ADJ %s%s jitter=%ldus adj:%d->%d poll:%lu->%lu",
             why, why2, (long)g.system_jitter_us, prev_adj, g.poll_adjust,
             (unsigned long)prev_poll, (unsigned long)g.current_poll_s);
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

static void schedule_after_request(ntp_peer_t *p) {
    p->next_poll_ms = mono_ms() + g.current_poll_s * 1000;
}

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

static void handle_socket_readable(int sock) {
    uint8_t buf[64];
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
        uint32_t now = mono_ms();
        bool is_selected = (g.selected_peer >= 0 && &g.peers[g.selected_peer] == p);
        int32_t since_disc_ms = (int32_t)(now - g.last_discipline_ms);
        bool due = (g.last_discipline_ms == 0) ||
                   since_disc_ms >= (int32_t)(g.current_poll_s * 500);
        if (is_selected && due) {
            g.last_discipline_ms = now;
            discipline_clock(p->best_offset_us);
            adaptive_poll_update();
        } else {
            ESP_LOGI(TAG, "DISC_SKIP peer=%s is_sel=%d due=%d since_disc=%ldms",
                     p->addr_str, is_selected, due, (long)since_disc_ms);
        }
    } else {
        ESP_LOGI(TAG, "NOT_OK peer=%s reach=%02x", p->addr_str, p->reach);
    }
}

static void ntp_task(void *arg) {
    (void)arg;
    g.sync_start_ms = mono_ms();
    g.current_poll_s = MIN_POLL_S;

    while (g.running) {
        lock_take();

        apply_freq_correction();

        if (g.dirty_config) {
            g.dirty_config = false;
            resolve_peers();
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
                schedule_after_request(p);
                // If the selected peer timed out, treat as a "bad" poll-adjust
                // event so the poll interval has a chance to shrink.
                if (g.selected_peer == i) {
                    adaptive_poll_update();
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
        if (g.sock4 >= 0) { FD_SET(g.sock4, &rfds); if (g.sock4 > maxfd) maxfd = g.sock4; }
        if (g.sock6 >= 0) { FD_SET(g.sock6, &rfds); if (g.sock6 > maxfd) maxfd = g.sock6; }

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
            if (g.sock4 >= 0 && FD_ISSET(g.sock4, &rfds)) handle_socket_readable(g.sock4);
            if (g.sock6 >= 0 && FD_ISSET(g.sock6, &rfds)) handle_socket_readable(g.sock6);
            lock_give();
        }
    }

    close_sockets();
    vTaskDelete(NULL);
}

// ---------- public API ----------

void ntp_init(const char *server, uint32_t max_interval_sec, bool prefer_ipv6) {
    if (g.running) ntp_stop();
    if (!g.lock) g.lock = xSemaphoreCreateMutex();

    strncpy(g.server, server ? server : "pool.ntp.org", sizeof(g.server) - 1);
    g.server[sizeof(g.server) - 1] = '\0';
    g.max_interval_s = max_interval_sec ? max_interval_sec : 86400;
    g.prefer_ipv6    = prefer_ipv6;
    g.current_poll_s = MIN_POLL_S;
    g.first_sync_done = false;
    g.sync_count     = 0;
    g.selected_peer  = -1;
    g.stratum        = 16;
    g.dirty_config   = true;

    g.running = true;
    xTaskCreate(ntp_task, "ntp", 4096, NULL, 5, &g.task);
}

void ntp_stop(void) {
    if (!g.running) return;
    g.running = false;
    // The task polls `running` after each select cycle.
}

void ntp_force_sync(void) {
    if (!g.lock) return;
    lock_take();
    g.force_sync = true;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        g.peers[i].next_poll_ms = mono_ms();
    }
    g.sync_start_ms = mono_ms();
    g.first_sync_done = false;   // show "Syncing..." until next success
    lock_give();
}

void ntp_set_server(const char *server) {
    if (!g.lock || !server) return;
    lock_take();
    strncpy(g.server, server, sizeof(g.server) - 1);
    g.server[sizeof(g.server) - 1] = '\0';
    g.dirty_config = true;
    g.force_sync   = true;
    lock_give();
}

void ntp_set_max_interval(uint32_t seconds) {
    if (!g.lock) return;
    if (seconds < MIN_POLL_S) seconds = MIN_POLL_S;
    lock_take();
    g.max_interval_s = seconds;
    if (g.current_poll_s > seconds) g.current_poll_s = seconds;
    lock_give();
}

void ntp_set_prefer_ipv6(bool prefer) {
    if (!g.lock) return;
    lock_take();
    if (g.prefer_ipv6 != prefer) {
        g.prefer_ipv6 = prefer;
        g.dirty_config = true;
        g.force_sync   = true;
    }
    lock_give();
}

void ntp_get_sys_stats(ntp_sys_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g.lock) { out->stratum = 16; out->selected_peer = 0xFF; return; }

    lock_take();
    out->synced         = g.first_sync_done;
    out->last_sync_time = g.last_sync_time;
    out->sync_count     = g.sync_count;
    out->sync_interval  = g.max_interval_s;
    out->current_poll_s = g.current_poll_s;
    out->sync_elapsed_ms = g.first_sync_done ? 0 : (mono_ms() - g.sync_start_ms);
    out->last_offset_us = g.last_offset_us;
    out->system_jitter_us    = g.system_jitter_us;
    out->root_delay_us       = g.root_delay_us;
    out->root_dispersion_us  = g.root_dispersion_us;
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
