#include "ntp_internal.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

// Address resolution shared by the NTP engine (ntp_peers.c) and the preset
// benchmark (ntp_benchmark.c).

bool sockaddr_matches(const struct sockaddr_storage *a,
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


static int resolve_numeric_host(const char *host, struct sockaddr_storage *out, int max) {
    if (!host || max <= 0) return 0;

    struct sockaddr_in *sin = (struct sockaddr_in *)&out[0];
    memset(sin, 0, sizeof(*sin));
    if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port   = htons(NTP_PORT);
        return 1;
    }

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&out[0];
    memset(sin6, 0, sizeof(*sin6));
    if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons(NTP_PORT);
        return 1;
    }

    return 0;
}


static void dns_clear_cache_cb(void *arg) {
    SemaphoreHandle_t done = (SemaphoreHandle_t)arg;
    dns_clear_cache();
    xSemaphoreGive(done);
}


static void clear_dns_cache_for_lookup(void) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return;

    if (tcpip_callback(dns_clear_cache_cb, done) == ERR_OK) {
        xSemaphoreTake(done, pdMS_TO_TICKS(1000));
    }

    vSemaphoreDelete(done);
}


static int append_getaddrinfo_results(const char *host, int family,
                                      struct sockaddr_storage *out,
                                      int count, int max) {
    struct addrinfo hints = {
        .ai_family   = family,
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
            if (sockaddr_matches(&out[j], &cand)) { dup = true; break; }
        }
        if (!dup) out[count++] = cand;
    }

    freeaddrinfo(res);
    return count;
}


int ntp_resolve_host(const char *host, bool prefer_ipv6,
                     struct sockaddr_storage *out, int max) {
    int count = resolve_numeric_host(host, out, max);
    if (count > 0) return count;

    clear_dns_cache_for_lookup();

    // Always try both families, ordered by preference: querying only the
    // preferred family means an IPv6-only network with the default
    // (IPv4-preferred) config resolves nothing and the staleness watchdog spins
    // re-resolving forever. The toggle sets which family wins, not the only one.
    if (prefer_ipv6) {
        count = append_getaddrinfo_results(host, AF_INET6, out, count, max);
        if (count < max) {
            count = append_getaddrinfo_results(host, AF_INET, out, count, max);
        }
    } else {
        count = append_getaddrinfo_results(host, AF_INET, out, count, max);
        if (count < max) {
            count = append_getaddrinfo_results(host, AF_INET6, out, count, max);
        }
    }
    return count;
}
