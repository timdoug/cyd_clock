#include "ntp_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
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


void sockaddr_set_port(struct sockaddr_storage *addr, uint16_t port) {
    if (addr->ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
    } else {
        ((struct sockaddr_in *)addr)->sin_port = htons(port);
    }
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


// Refcounted so a tcpip thread backed up past the wait timeout can't give a
// semaphore the waiter already deleted: the last side out frees.
typedef struct {
    SemaphoreHandle_t done;
    atomic_int refs;
} dns_clear_ctx_t;


static void dns_clear_ctx_unref(dns_clear_ctx_t *ctx) {
    if (atomic_fetch_sub(&ctx->refs, 1) == 1) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }
}


static void dns_clear_cache_cb(void *arg) {
    dns_clear_ctx_t *ctx = (dns_clear_ctx_t *)arg;
    dns_clear_cache();
    xSemaphoreGive(ctx->done);
    dns_clear_ctx_unref(ctx);
}


static void clear_dns_cache_for_lookup(void) {
    dns_clear_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        return;
    }
    atomic_init(&ctx->refs, 2);

    if (tcpip_callback(dns_clear_cache_cb, ctx) == ERR_OK) {
        xSemaphoreTake(ctx->done, pdMS_TO_TICKS(1000));
    } else {
        dns_clear_ctx_unref(ctx);   // callback never queued; drop its ref
    }
    dns_clear_ctx_unref(ctx);
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
    // preferred family means a single-stack network can resolve nothing and the
    // staleness watchdog spins re-resolving forever. The toggle sets which
    // family wins, not the only one.
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
