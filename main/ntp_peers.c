#include "ntp_internal.h"
#include "ntp_siv.h"
#include "mbedtls/platform_util.h"

#include <stdlib.h>

#define NTS_KE_RETRY_MS (5 * 60 * 1000U)

typedef struct {
    char host[64];
    uint32_t generation;
} nts_ke_arg_t;

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


static int dns_resolve_all(const char *host, bool prefer_ipv6,
                           struct sockaddr_storage *out, int max) {
    int count = resolve_numeric_host(host, out, max);
    if (count > 0) return count;

    if (prefer_ipv6) {
        count = append_getaddrinfo_results(host, AF_INET6, out, count, max);
        if (count < max) {
            count = append_getaddrinfo_results(host, AF_INET, out, count, max);
        }
    } else {
        count = append_getaddrinfo_results(host, AF_INET, out, count, max);
    }
    return count;
}


static void peer_reset(ntp_peer_t *p) {
    memset(p, 0, sizeof(*p));
    p->stratum = 16;
    // This is purely a UI affordance: show a newly-installed peer long
    // enough to notice, independent of the adaptive poll interval. Tying the
    // highlight to the next poll tick made replacement peers stay green for
    // minutes once polling had grown to 1024 s.
    p->fresh_until_ms = mono_ms() + NEW_PEER_HIGHLIGHT_MS;
}


static void peer_set_port(ntp_peer_t *p, uint16_t port) {
    if (p->addr.ss_family == AF_INET6)
        ((struct sockaddr_in6 *)&p->addr)->sin6_port = htons(port);
    else
        ((struct sockaddr_in *)&p->addr)->sin_port = htons(port);
}


static void peer_set_addr(ntp_peer_t *p, const struct sockaddr_storage *a,
                          uint16_t port, bool nts) {
    socklen_t alen = (a->ss_family == AF_INET6)
        ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memcpy(&p->addr, a, alen);
    p->addr_len = alen;
    peer_set_port(p, port);
    p->active   = true;
    p->stratum  = 16;
    p->nts      = nts;
    p->next_poll_ms = mono_ms();
    p->next_poll_cycle_id = next_global_poll_cycle_id;
    const void *src = (a->ss_family == AF_INET6)
        ? (const void *)&((struct sockaddr_in6 *)&p->addr)->sin6_addr
        : (const void *)&((struct sockaddr_in  *)&p->addr)->sin_addr;
    inet_ntop(a->ss_family, src, p->addr_str, sizeof(p->addr_str));
}


static void nts_ke_task(void *arg) {
    nts_ke_arg_t *ke = (nts_ke_arg_t *)arg;
    char host[sizeof(ke->host)];
    uint32_t generation = ke->generation;
    str_copy(host, sizeof(host), ke->host);
    free(ke);

    // Guard the AEAD and NTS parser once before any authenticated time is
    // trusted: a broken SIV/parser that still "verifies" would be worse than
    // no auth. If it fails, refuse NTS and fall back to plain.
    static bool nts_checked = false, nts_ok = false;
    if (!nts_checked) {
        nts_ok = ntp_siv_selftest() && ntp_nts_selftest();
        nts_checked = true;
        ESP_LOGI(TAG, "NTS crypto self-test %s", nts_ok ? "passed" : "FAILED");
    }

    ntp_nts_ctx_t local = {0};
    bool ok = nts_ok && ntp_nts_ke_run(host, &local);

    lock_take();
    if (g.nts.ke_generation == generation && strcmp(g.server, host) != 0) {
        // Keep at most one 16 KB KE task alive; the stale task exits, then the
        // new config resolves and starts its own KE.
        g.nts.ke_in_flight = false;
        g.nts.ke_failed = false;
        g.nts.ke_retry_at_ms = 0;
        g.dirty_config = true;
    } else if (g.nts.ke_generation == generation && ok) {
        uint32_t keep_generation = g.nts.ke_generation;
        g.nts = local;
        g.nts.ke_generation = keep_generation;
        g.nts.ke_in_flight = false;
        g.nts.ke_failed    = false;
        g.nts.ke_retry_at_ms = 0;
        g.nts_rebind       = true;
    } else if (g.nts.ke_generation == generation) {
        g.nts.ke_in_flight = false;
        g.nts.ke_failed    = true;
        g.nts.ke_retry_at_ms = mono_ms() + NTS_KE_RETRY_MS;
    }
    mbedtls_platform_zeroize(&local, sizeof(local));
    lock_give();
    wake_task();
    vTaskDelete(NULL);
}


void nts_start_ke_if_needed(void) {
    if (g.nts.valid || g.nts.ke_in_flight) return;
    if (g.nts.ke_failed && (int32_t)(mono_ms() - g.nts.ke_retry_at_ms) < 0) return;
    nts_ke_arg_t *ke = calloc(1, sizeof(*ke));
    if (!ke) {
        ESP_LOGW(TAG, "Failed to allocate NTS-KE task args");
        return;
    }
    str_copy(ke->host, sizeof(ke->host), g.server);
    ke->generation = ++g.nts.ke_generation;
    if (ke->generation == 0) ke->generation = ++g.nts.ke_generation;
    g.nts.ke_in_flight = true;
    g.nts.ke_failed = false;
    if (xTaskCreate(nts_ke_task, "nts_ke", 16384, ke, 5, NULL) != pdPASS) {
        g.nts.ke_in_flight = false;
        free(ke);
        ESP_LOGW(TAG, "Failed to spawn NTS-KE task");
    }
}


void nts_rebind_peers(void) {
    if (!g.nts.valid) return;
    if (strcmp(g.nts.ntp_host, g.server) == 0) {
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *p = &g.peers[i];
            if (!p->active) continue;
            p->nts = true;
            peer_set_port(p, g.nts.ntp_port);
        }
        ESP_LOGI(TAG, "NTS active for %s", g.server);
    } else {
        resolve_peers();
    }
}


void nts_drop_context_and_fallback(void) {
    bool negotiated_host = g.nts.valid && strcmp(g.nts.ntp_host, g.server) != 0;
    bool ke_in_flight = g.nts.ke_in_flight;
    uint32_t ke_generation = g.nts.ke_generation;

    mbedtls_platform_zeroize(&g.nts, sizeof(g.nts));
    g.nts.ke_in_flight = ke_in_flight;
    g.nts.ke_generation = ke_generation;
    g.nts_rebind = false;

    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active) continue;
        p->nts = false;
        memset(p->uid, 0, sizeof(p->uid));
        peer_set_port(p, NTP_PORT);
    }

    if (negotiated_host) {
        g.dirty_config = true;
    }

    nts_start_ke_if_needed();
}


int resolve_peers(void) {
    // DNS runs unlocked; reject the result if config changed while we were out.
    bool use_nts = g.nts.valid;
    char host_copy[sizeof(g.server)];
    str_copy(host_copy, sizeof(host_copy), use_nts ? g.nts.ntp_host : g.server);
    char server_copy[sizeof(g.server)];
    str_copy(server_copy, sizeof(server_copy), g.server);
    bool prefer_ipv6_copy = g.prefer_ipv6;
    uint16_t port = use_nts ? g.nts.ntp_port : NTP_PORT;
    lock_give();

    struct sockaddr_storage addrs[NTP_MAX_PEERS];
    int n = dns_resolve_all(host_copy, prefer_ipv6_copy, addrs, NTP_MAX_PEERS);

    lock_take();

    if (g.dirty_config ||
        strcmp(g.server, server_copy) != 0 || g.prefer_ipv6 != prefer_ipv6_copy ||
        g.nts.valid != use_nts ||
        (use_nts && (strcmp(g.nts.ntp_host, host_copy) != 0 ||
                     g.nts.ntp_port != port))) {
        ESP_LOGI(TAG, "Discarding stale DNS results for %s", host_copy);
        return 0;
    }

    for (int i = 0; i < NTP_MAX_PEERS; i++) peer_reset(&g.peers[i]);
    g.selected_peer = -1;
    g.stratum = 16;

    if (n == 0) {
        ESP_LOGW(TAG, "DNS failed for %s", host_copy);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        peer_set_addr(&g.peers[i], &addrs[i], port, use_nts);
    }
    ESP_LOGI(TAG, "Resolved %s to %d peer(s)%s", host_copy, n,
             use_nts ? " (NTS)" : "");

    // Idempotent: skips if a KE is running or this host already succeeded/failed.
    if (!use_nts) nts_start_ke_if_needed();
    return n;
}

static int find_worst_eligible_peer(void) {
    int      worst          = -1;
    uint32_t worst_severity = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active) {
            if (p->addr_len != 0 && worst_severity < UINT32_MAX) {
                worst_severity = UINT32_MAX;
                worst          = i;
            }
            continue;
        }
        // Counters freeze (neither advance nor decay) while a peer is not
        // a selection candidate; they resume from where they were when it
        // returns. Severity is expressed in units of "times the eviction
        // threshold" (x256 fixed point) so counters with different
        // thresholds compare fairly - 4 misses and 10 jittery runs are
        // both exactly at threshold and should look equally bad.
        if (p->consecutive_misses < 4 &&
            p->falseticker_runs   < 8 &&
            p->jittery_runs       < 10 &&
            p->panic_runs         < 8) continue;
        uint32_t severity = ((uint32_t)p->consecutive_misses << 8) / 4 +
                            ((uint32_t)p->falseticker_runs   << 8) / 8 +
                            ((uint32_t)p->jittery_runs       << 8) / 10 +
                            ((uint32_t)p->panic_runs         << 8) / 8;
        if (severity > worst_severity) {
            worst_severity = severity;
            worst          = i;
        }
    }
    return worst;
}


static bool try_replace_peer(int dead_idx) {
    bool use_nts = g.nts.valid;
    char host_copy[sizeof(g.server)];
    str_copy(host_copy, sizeof(host_copy), use_nts ? g.nts.ntp_host : g.server);
    char server_copy[sizeof(g.server)];
    str_copy(server_copy, sizeof(server_copy), g.server);
    bool prefer_ipv6_copy = g.prefer_ipv6;
    uint16_t port = use_nts ? g.nts.ntp_port : NTP_PORT;
    lock_give();

    struct sockaddr_storage fresh[NTP_MAX_PEERS];
    int n = dns_resolve_all(host_copy, prefer_ipv6_copy, fresh, NTP_MAX_PEERS);

    lock_take();

    if (g.dirty_config ||
        strcmp(g.server, server_copy) != 0 ||
        g.prefer_ipv6 != prefer_ipv6_copy ||
        g.nts.valid != use_nts ||
        (use_nts && (strcmp(g.nts.ntp_host, host_copy) != 0 ||
                     g.nts.ntp_port != port))) {
        return false;
    }
    if (n == 0) return false;

    for (int i = 0; i < n; i++) {
        // Skip any address already recorded in a slot, active or not: an
        // inactive-but-populated slot is one a DENY/RSTR KoD deactivated,
        // and reinstalling the same server would ping-pong against an
        // explicit go-away. (The slot being replaced still holds its old
        // address at this point, so it's excluded too.)
        bool in_use = false;
        for (int j = 0; j < NTP_MAX_PEERS; j++) {
            if (g.peers[j].addr_len != 0 && sockaddr_matches(&g.peers[j].addr, &fresh[i])) {
                in_use = true;
                break;
            }
        }
        if (in_use) continue;

        ntp_peer_t *p = &g.peers[dead_idx];
        char old_addr[46];
        str_copy(old_addr, sizeof(old_addr), p->addr_str);

        peer_reset(p);
        peer_set_addr(p, &fresh[i], port, use_nts);

        ESP_LOGI(TAG, "Peer swap slot %d: %s -> %s", dead_idx, old_addr, p->addr_str);
        return true;
    }
    return false;
}

void maybe_evict_worst_peer(void) {
    if (last_evict_tick_ms == next_global_poll_ms) return;
    int worst = find_worst_eligible_peer();
    if (worst < 0) return;
    // Record the ATTEMPT, not just success: a failed replacement (DNS had
    // nothing fresh to offer) used to retry at every settle event - up to
    // ~5 DNS queries per wave, forever, with a persistently eligible peer
    // and a small address pool. One attempt per poll tick is the intent.
    last_evict_tick_ms = next_global_poll_ms;
    if (try_replace_peer(worst)) {
        if (g.selected_peer == worst) g.selected_peer = -1;
    }
}
