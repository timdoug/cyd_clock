#include "ntp_internal.h"
#include "ntp_siv.h"
#include "mbedtls/platform_util.h"

#include <stdlib.h>

#define NTS_KE_RETRY_MS (5 * 60 * 1000U)

typedef struct {
    char host[64];
    uint32_t generation;
} nts_ke_arg_t;

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
    } else if (g.nts.ke_generation == generation && ok &&
               g.nts_mode != NTS_MODE_OFF) {
        // The mode check covers NTS being switched off while this KE was in
        // flight: dirty-config preserves ke_generation for a running task, so
        // without it the completed handshake would install and rebind every
        // peer to NTS against the user's setting.
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


static void spawn_ke_task(void) {
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
    // Pin to core 0 (the network core) at one priority below the NTP task: the
    // transient handshake crypto then can't preempt time-critical NTP work, and
    // stays off core 1 so it can't stall the render loop.
    if (xTaskCreatePinnedToCore(nts_ke_task, "nts_ke", 16384, ke, 4, NULL, 0) != pdPASS) {
        g.nts.ke_in_flight = false;
        free(ke);
        ESP_LOGW(TAG, "Failed to spawn NTS-KE task");
    }
}


void nts_start_ke_if_needed(void) {
    if (g.nts_mode == NTS_MODE_OFF) return;
    if (g.nts.valid || g.nts.ke_in_flight) return;
    if (g.nts.ke_failed && (int32_t)(mono_ms() - g.nts.ke_retry_at_ms) < 0) return;
    spawn_ke_task();
}


void nts_request_rekey(void) {
    // Refresh keys/cookies while keeping the current context usable. Unlike
    // nts_start_ke_if_needed this runs even when g.nts.valid, because the
    // trigger (a NAK) means the current keys may be stale - but we must not
    // drop them until the new context installs, so a forged NAK can't strip an
    // established session. The completed KE swaps g.nts atomically under the
    // lock; a failed KE leaves the current context intact.
    if (g.nts_mode == NTS_MODE_OFF) return;
    if (!g.nts.valid) { nts_start_ke_if_needed(); return; }
    if (g.nts.ke_in_flight) return;   // one KE at a time bounds forged-NAK churn
    if (g.nts.ke_failed && (int32_t)(mono_ms() - g.nts.ke_retry_at_ms) < 0) return;
    spawn_ke_task();
}


void nts_rebind_peers(void) {
    if (g.nts_mode == NTS_MODE_OFF) return;
    if (!g.nts.valid) return;
    if (strcmp(g.nts.ntp_host, g.server) == 0) {
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *p = &g.peers[i];
            if (!p->active) continue;
            p->nts = true;
            peer_set_port(p, g.nts.ntp_port);
        }
        // Poll now: REQUIRE rejected the pre-KE plain responses, so the clock
        // would otherwise wait a full poll interval for the first NTS sample.
        g.force_sync = true;
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

    // REQUIRE never falls back to plain: keep the peers marked NTS (they back
    // off until the re-run KE refills cookies) so we never emit an
    // unauthenticated request. Opportunistic mode downgrades the peers to plain
    // port-123 NTP to keep time flowing during the handshake.
    if (g.nts_mode != NTS_MODE_REQUIRE) {
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
    }

    nts_start_ke_if_needed();
}


// Config snapshot for a DNS resolve. ntp_resolve_host blocks, so both the
// initial resolve and the eviction refresh drop g.lock across the lookup and
// must reject the result if any of these changed meanwhile.
typedef struct {
    char     host[sizeof(g.server)];
    char     server[sizeof(g.server)];
    bool     prefer_ipv6;
    uint16_t port;
    bool     use_nts;
} resolve_ctx_t;

// Snapshot under the lock, before dropping it for the lookup.
static void resolve_ctx_snapshot(resolve_ctx_t *s) {
    s->use_nts = g.nts.valid;
    str_copy(s->host, sizeof(s->host), s->use_nts ? g.nts.ntp_host : g.server);
    str_copy(s->server, sizeof(s->server), g.server);
    s->prefer_ipv6 = g.prefer_ipv6;
    s->port = s->use_nts ? g.nts.ntp_port : NTP_PORT;
}

// True if config drifted while the lock was dropped; call after re-taking it.
static bool resolve_ctx_stale(const resolve_ctx_t *s) {
    return g.dirty_config ||
           strcmp(g.server, s->server) != 0 ||
           g.prefer_ipv6 != s->prefer_ipv6 ||
           g.nts.valid != s->use_nts ||
           (s->use_nts && (strcmp(g.nts.ntp_host, s->host) != 0 ||
                           g.nts.ntp_port != s->port));
}


void resolve_peers(void) {
    resolve_ctx_t s;
    resolve_ctx_snapshot(&s);
    ESP_LOGD(TAG, "DNS query host=%s server=%s ipv6=%s nts=%s",
             s.host, s.server, s.prefer_ipv6 ? "yes" : "no",
             s.use_nts ? "yes" : "no");
    lock_give();

    struct sockaddr_storage addrs[NTP_MAX_PEERS];
    int n = ntp_resolve_host(s.host, s.prefer_ipv6, addrs, NTP_MAX_PEERS);

    lock_take();

    if (resolve_ctx_stale(&s)) {
        ESP_LOGI(TAG, "Discarding stale DNS results for %s", s.host);
        return;
    }

    for (int i = 0; i < NTP_MAX_PEERS; i++) peer_reset(&g.peers[i]);
    g.selected_peer = -1;
    g.stratum = 16;

    if (n == 0) {
        ESP_LOGW(TAG, "DNS failed for %s", s.host);
        return;
    }

    for (int i = 0; i < n; i++) {
        peer_set_addr(&g.peers[i], &addrs[i], s.port, s.use_nts);
        ESP_LOGD(TAG, "Peer install server=%s host=%s slot=%d addr=%s%s",
                 s.server, s.host, i, g.peers[i].addr_str,
                 s.use_nts ? " NTS" : "");
    }
    ESP_LOGI(TAG, "Resolved %s to %d peer(s)%s", s.host, n,
             s.use_nts ? " (NTS)" : "");

    // Idempotent: skips if a KE is running or this host already succeeded/failed.
    if (!s.use_nts) nts_start_ke_if_needed();
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
    resolve_ctx_t s;
    resolve_ctx_snapshot(&s);
    ESP_LOGD(TAG, "DNS refresh host=%s server=%s ipv6=%s nts=%s",
             s.host, s.server, s.prefer_ipv6 ? "yes" : "no",
             s.use_nts ? "yes" : "no");
    lock_give();

    struct sockaddr_storage fresh[NTP_MAX_PEERS];
    int n = ntp_resolve_host(s.host, s.prefer_ipv6, fresh, NTP_MAX_PEERS);

    lock_take();

    if (resolve_ctx_stale(&s)) return false;
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
        peer_set_addr(p, &fresh[i], s.port, s.use_nts);

        ESP_LOGI(TAG, "Peer swap slot %d: %s -> %s", dead_idx, old_addr, p->addr_str);
        return true;
    }
    return false;
}

void maybe_evict_worst_peer(void) {
    if (last_evict_tick_ms == next_global_poll_ms) return;
    // After a replacement attempt found nothing fresh, wait out a backoff before
    // querying DNS again: a permanently-deactivated slot (a single-IP host that
    // sent DENY, whose only address is excluded from reinstall) is always the
    // worst-eligible peer, so without this it triggers a DNS query every poll
    // tick for the life of the config. Cleared on config change and on success.
    if (replace_backoff_until_ms &&
        (int32_t)(mono_ms() - replace_backoff_until_ms) < 0) return;
    int worst = find_worst_eligible_peer();
    if (worst < 0) return;
    // Record the ATTEMPT, not just success: a failed replacement (DNS had
    // nothing fresh to offer) used to retry at every settle event - up to
    // ~5 DNS queries per wave, forever, with a persistently eligible peer
    // and a small address pool. One attempt per poll tick is the intent.
    last_evict_tick_ms = next_global_poll_ms;
    if (try_replace_peer(worst)) {
        replace_backoff_until_ms = 0;
        if (g.selected_peer == worst) g.selected_peer = -1;
    } else {
        replace_backoff_until_ms = mono_ms() + MAX_POLL_S * 1000;
    }
}
