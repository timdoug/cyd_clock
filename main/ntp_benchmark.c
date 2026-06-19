#include "ntp_benchmark.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "ntp_internal.h"

static const char *BENCH_TAG = "ntp_bench";

#define BENCH_MAX_ADDRS 4
#define BENCH_TIMEOUT_MS 2500

static void set_port(struct sockaddr_storage *addr, uint16_t port) {
    if (addr->ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
    } else {
        ((struct sockaddr_in *)addr)->sin_port = htons(port);
    }
}

static socklen_t addr_len(const struct sockaddr_storage *addr) {
    return addr->ss_family == AF_INET6
        ? sizeof(struct sockaddr_in6)
        : sizeof(struct sockaddr_in);
}

static void addr_to_str(const struct sockaddr_storage *addr, char *buf, size_t len) {
    const void *src = addr->ss_family == AF_INET6
        ? (const void *)&((const struct sockaddr_in6 *)addr)->sin6_addr
        : (const void *)&((const struct sockaddr_in *)addr)->sin_addr;
    inet_ntop(addr->ss_family, src, buf, len);
}

static int resolve_numeric_host(const char *host, struct sockaddr_storage *out, int max) {
    if (!host || max <= 0) return 0;

    struct sockaddr_in *sin = (struct sockaddr_in *)&out[0];
    memset(sin, 0, sizeof(*sin));
    if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(NTP_PORT);
        return 1;
    }

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&out[0];
    memset(sin6, 0, sizeof(*sin6));
    if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(NTP_PORT);
        return 1;
    }

    return 0;
}

static int append_getaddrinfo_results(const char *host, int family,
                                      struct sockaddr_storage *out,
                                      int count, int max) {
    struct addrinfo hints = {
        .ai_family = family,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "123", &hints, &res) != 0) return count;

    for (struct addrinfo *ai = res; ai && count < max; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(out[0])) continue;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;

        struct sockaddr_storage cand;
        memset(&cand, 0, sizeof(cand));
        memcpy(&cand, ai->ai_addr, ai->ai_addrlen);

        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (sockaddr_matches(&out[j], &cand)) {
                dup = true;
                break;
            }
        }
        if (!dup) out[count++] = cand;
    }

    freeaddrinfo(res);
    return count;
}

static int resolve_host(const char *host, bool prefer_ipv6,
                        struct sockaddr_storage *out, int max) {
    ESP_LOGI(BENCH_TAG, "DNS query host=%s ipv6=%s",
             host, prefer_ipv6 ? "yes" : "no");
    int count = resolve_numeric_host(host, out, max);
    if (count > 0) {
        char addr[46];
        addr_to_str(&out[0], addr, sizeof(addr));
        ESP_LOGI(BENCH_TAG, "DNS result host=%s addr[0]=%s", host, addr);
        return count;
    }

    ntp_clear_dns_cache_for_lookup();

    if (prefer_ipv6) {
        count = append_getaddrinfo_results(host, AF_INET6, out, count, max);
        if (count < max) {
            count = append_getaddrinfo_results(host, AF_INET, out, count, max);
        }
    } else {
        count = append_getaddrinfo_results(host, AF_INET, out, count, max);
    }
    for (int i = 0; i < count; i++) {
        char addr[46];
        addr_to_str(&out[i], addr, sizeof(addr));
        ESP_LOGI(BENCH_TAG, "DNS result host=%s addr[%d]=%s", host, i, addr);
    }
    return count;
}

static ntp_benchmark_status_t bench_one_addr(const struct sockaddr_storage *addr,
                                             const ntp_nts_ctx_t *nts_in,
                                             ntp_benchmark_result_t *out) {
    ntp_nts_ctx_t nts;
    memset(&nts, 0, sizeof(nts));
    bool use_nts = nts_in && nts_in->valid;
    if (use_nts) memcpy(&nts, nts_in, sizeof(nts));

    int sock = socket(addr->ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_TIMEOUT;
    }

    ntp_pkt_t pkt = {0};
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.precision = LOCAL_PRECISION;
    pkt.poll = 6;
    pkt.xmt_ts_sec = esp_random();
    pkt.xmt_ts_frac = esp_random();
    uint32_t req_xmt_sec = pkt.xmt_ts_sec;
    uint32_t req_xmt_frac = pkt.xmt_ts_frac;

    uint8_t buf[1280];
    memcpy(buf, &pkt, sizeof(pkt));
    size_t pkt_len = sizeof(pkt);
    uint8_t uid[NTS_UID_LEN] = {0};
    if (use_nts && !ntp_nts_add_ef(buf, &pkt_len, sizeof(buf), &nts, uid)) {
        close(sock);
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_NTS_FAILED;
    }

    struct timeval t1_pre, t1_post;
    gettimeofday(&t1_pre, NULL);
    ssize_t sent = sendto(sock, buf, pkt_len, 0,
                          (const struct sockaddr *)addr, addr_len(addr));
    gettimeofday(&t1_post, NULL);
    if (sent != (ssize_t)pkt_len) {
        close(sock);
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_TIMEOUT;
    }

    int64_t t1_mid_us =
        (tv_to_us(&t1_pre) + tv_to_us(&t1_post)) / 2;
    struct timeval t1 = tv_from_us(t1_mid_us);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = {
        .tv_sec = BENCH_TIMEOUT_MS / 1000,
        .tv_usec = (BENCH_TIMEOUT_MS % 1000) * 1000,
    };
    int sr = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (sr <= 0) {
        close(sock);
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_TIMEOUT;
    }

    struct timeval t4;
    gettimeofday(&t4, NULL);
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fromlen);
    close(sock);
    if (n < (ssize_t)sizeof(ntp_pkt_t) || !sockaddr_matches(addr, &from)) {
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_BAD_RESPONSE;
    }

    memcpy(&pkt, buf, sizeof(pkt));
    uint8_t mode = pkt.li_vn_mode & 0x07;
    uint8_t vn = (pkt.li_vn_mode >> 3) & 0x07;
    uint8_t li = (pkt.li_vn_mode >> 6) & 0x03;
    if (mode != NTP_MODE_SERVER || vn < 3 ||
        pkt.orig_ts_sec != req_xmt_sec ||
        pkt.orig_ts_frac != req_xmt_frac ||
        pkt.stratum == 0 || pkt.stratum >= 16 || li == 3 ||
        pkt.xmt_ts_sec == 0) {
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_BAD_RESPONSE;
    }
    if (use_nts && !ntp_nts_check_response(buf, (size_t)n, &nts, uid)) {
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        return NTP_BENCHMARK_BAD_RESPONSE;
    }

    struct timeval t2, t3;
    ntp_to_tv(ntohl(pkt.recv_ts_sec), ntohl(pkt.recv_ts_frac), &t2);
    ntp_to_tv(ntohl(pkt.xmt_ts_sec), ntohl(pkt.xmt_ts_frac), &t3);
    int64_t delay = tv_diff_us(&t4, &t1) - tv_diff_us(&t3, &t2);
    if (delay < 0) delay = 0;
    if (delay > INT32_MAX) delay = INT32_MAX;

    out->status = NTP_BENCHMARK_OK;
    out->delay_us = (int32_t)delay;
    out->nts = use_nts;
    addr_to_str(addr, out->addr_str, sizeof(out->addr_str));

    mbedtls_platform_zeroize(&nts, sizeof(nts));
    return NTP_BENCHMARK_OK;
}

ntp_benchmark_status_t ntp_benchmark_server(const char *host,
                                            bool prefer_ipv6,
                                            nts_mode_t nts_mode,
                                            ntp_benchmark_result_t *out) {
    if (!out) return NTP_BENCHMARK_BAD_RESPONSE;
    memset(out, 0, sizeof(*out));
    out->status = NTP_BENCHMARK_TIMEOUT;
    out->delay_us = INT32_MAX;

    ntp_nts_ctx_t nts;
    memset(&nts, 0, sizeof(nts));
    bool have_nts = false;
    if (nts_mode != NTS_MODE_OFF) {
        have_nts = ntp_nts_ke_run(host, &nts);
        if (!have_nts && nts_mode == NTS_MODE_REQUIRE) {
            mbedtls_platform_zeroize(&nts, sizeof(nts));
            out->status = NTP_BENCHMARK_NTS_FAILED;
            return out->status;
        }
    }

    const char *query_host = have_nts ? nts.ntp_host : host;
    uint16_t port = have_nts ? nts.ntp_port : NTP_PORT;
    struct sockaddr_storage addrs[BENCH_MAX_ADDRS];
    int n = resolve_host(query_host, prefer_ipv6, addrs, BENCH_MAX_ADDRS);
    if (n == 0) {
        mbedtls_platform_zeroize(&nts, sizeof(nts));
        out->status = NTP_BENCHMARK_DNS_FAILED;
        return out->status;
    }

    ntp_benchmark_status_t last = NTP_BENCHMARK_TIMEOUT;
    for (int i = 0; i < n; i++) {
        set_port(&addrs[i], port);
        ntp_benchmark_result_t r;
        memset(&r, 0, sizeof(r));
        r.delay_us = INT32_MAX;
        last = bench_one_addr(&addrs[i], have_nts ? &nts : NULL, &r);
        if (last == NTP_BENCHMARK_OK) {
            *out = r;
            ESP_LOGI(BENCH_TAG, "%s %ld us%s via %s", host, (long)out->delay_us,
                     out->nts ? " NTS" : "", out->addr_str);
            mbedtls_platform_zeroize(&nts, sizeof(nts));
            return NTP_BENCHMARK_OK;
        }
    }

    mbedtls_platform_zeroize(&nts, sizeof(nts));
    out->status = last;
    return out->status;
}
