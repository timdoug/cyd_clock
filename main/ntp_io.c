#include "ntp_internal.h"

// Tri-state result so the caller can tell "this datagram doesn't belong to
// the outstanding request" (stray, duplicate, or spoofed -- ignore it and
// keep waiting for the real response) apart from "this IS the response but
// it's unusable" (settle the request, no sample).
typedef enum {
    RESP_IGNORE,
    RESP_BAD,
    RESP_GOOD,
} ntp_resp_result_t;

bool open_sockets(void) {
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


void close_sockets(void) {
    if (g.sock4 >= 0) { close(g.sock4); g.sock4 = -1; }
    if (g.sock6 >= 0) { close(g.sock6); g.sock6 = -1; }
}


void close_wake_sock(void) {
    if (g.wake_sock >= 0) {
        close(g.wake_sock);
        g.wake_sock = -1;
        g.wake_port = 0;
    }
}


void open_wake_sock(void) {
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

void wake_task(void) {
    if (g.wake_sock < 0 || g.wake_port == 0) return;
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(g.wake_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    uint8_t byte = 1;
    sendto(g.wake_sock, &byte, 1, 0, (struct sockaddr *)&dst, sizeof(dst));
}


void drain_wake_sock(void) {
    if (g.wake_sock < 0) return;
    uint8_t buf[16];
    while (recv(g.wake_sock, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}


void schedule_after_request(ntp_peer_t *p) {
    uint32_t now = mono_ms();
    uint32_t interval_ms = g.current_poll_s * 1000;
    if (poll_reset_pending || (int32_t)(now - next_global_poll_ms) >= 0) {
        poll_reset_pending = false;
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

// Bookkeeping shared by every poll that settles without a response. A
// failed send still owes its reach shift (shift_reach); a timeout's shift
// already happened at send time. Eviction and the discipline gate stay
// with the caller - the timeout path evicts between settling and gating.
void settle_miss(ntp_peer_t *p, uint32_t cycle_id, bool shift_reach) {
    if (p->consecutive_misses < 255) p->consecutive_misses++;
    if (shift_reach) p->reach <<= 1;
    p->request_outstanding  = false;
    p->cycle_id_when_sent   = cycle_id;
    p->last_settle_cycle_id = cycle_id;
    schedule_after_request(p);
}

static void settle_send_miss(ntp_peer_t *p, uint32_t request_cycle_id) {
    settle_miss(p, request_cycle_id, true);
    try_discipline(request_cycle_id);
}


void ntp_build_client_request(ntp_pkt_t *pkt, int8_t poll) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt->precision  = LOCAL_PRECISION;
    pkt->poll       = poll;
    // Fully random transmit timestamp. The server echoes xmt verbatim as
    // orig and nothing on our side reads it back as a time - it's purely
    // the correlation key for response matching and the early-stamp rings -
    // so a 64-bit random nonce works unchanged. Versus stamping the real
    // clock: an off-path spoofer must guess 64 random bits instead of 32,
    // and we don't leak clock state (a pre-sync device would otherwise
    // advertise "clock at epoch" in every request). Same data minimization
    // chrony applies.
    pkt->xmt_ts_sec  = esp_random();
    pkt->xmt_ts_frac = esp_random();
}


bool send_request(ntp_peer_t *p) {
    uint32_t poll_s = g.current_poll_s ? g.current_poll_s : MIN_POLL_S;
    int8_t poll_exp = 0;
    while ((1U << poll_exp) < poll_s && poll_exp < 15) poll_exp++;

    ntp_pkt_t pkt;
    ntp_build_client_request(&pkt, poll_exp);

    uint32_t request_cycle_id = p->next_poll_cycle_id;
    if (request_cycle_id == 0) {
        request_cycle_id = next_global_poll_cycle_id ? next_global_poll_cycle_id : 1;
    }

    int sock = (p->addr.ss_family == AF_INET6) ? g.sock6 : g.sock4;
    if (sock < 0) {
        ESP_LOGW(TAG, "No socket for peer=%s family=%d", p->addr_str, p->addr.ss_family);
        settle_send_miss(p, request_cycle_id);
        return false;
    }

    uint8_t buf[1280];
    memcpy(buf, &pkt, sizeof(pkt));
    size_t pkt_len = sizeof(pkt);
    if (p->nts) {
        if (!ntp_nts_add_ef(buf, &pkt_len, sizeof(buf), &g.nts, p->uid)) {
            ESP_LOGW(TAG, "NTS cookies exhausted for %s; re-running KE", p->addr_str);
            nts_drop_context_and_fallback();
            // If we downgraded to plain (opportunistic) retry soon to keep time
            // flowing; if we stayed NTS (REQUIRE, fail closed) back off a full
            // interval - the re-run KE force_syncs a fresh poll once cookies
            // return, so there's no need to spin retrying add_ef every second.
            uint32_t retry_ms = p->nts ? g.current_poll_s * 1000 : 1000;
            p->next_poll_ms = mono_ms() + retry_ms;
            p->next_poll_cycle_id = next_global_poll_cycle_id;
            return false;
        }
    }

    // Take t1_pre / sendto / t1_post around the call itself; the MIDPOINT
    // of pre and post is the real t1 for offset computation, which halves
    // the stack-transit bias vs capturing only before sendto. (The WiFi
    // TX-done hook usually overrides this t1 anyway.)
    struct timeval t1_pre, t1_post;
    gettimeofday(&t1_pre, NULL);
    ssize_t n = sendto(sock, buf, pkt_len, 0,
                       (struct sockaddr *)&p->addr, p->addr_len);
    gettimeofday(&t1_post, NULL);
    if (n != (ssize_t)pkt_len) {
        ESP_LOGW(TAG, "sendto %s failed (errno=%d)", p->addr_str, errno);
        settle_send_miss(p, request_cycle_id);
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
    p->reach <<= 1;
    p->request_outstanding = true;
    p->request_sent_ms     = mono_ms();
    p->cycle_id_when_sent  = request_cycle_id;
    return true;
}


static ntp_resp_result_t process_response(ntp_peer_t *p, const ntp_pkt_t *pkt,
                                          const uint8_t *raw, size_t raw_len,
                                          const struct timeval *t4) {
    uint8_t mode = pkt->li_vn_mode & 0x07;
    uint8_t vn   = (pkt->li_vn_mode >> 3) & 0x07;
    uint8_t li   = (pkt->li_vn_mode >> 6) & 0x03;

    if (mode != NTP_MODE_SERVER || vn < 3) return RESP_IGNORE;

    // Match originate timestamp to the one we sent. Checked BEFORE the KoD
    // handling: per RFC 5905 a KoD is only valid if it echoes our transmit
    // timestamp, and gating on it means an off-path attacker can't disable
    // a peer with a blind spoofed DENY.
    if (pkt->orig_ts_sec != p->xmt_sec_net ||
        pkt->orig_ts_frac != p->xmt_frac_net) {
        ESP_LOGW(TAG, "orig-ts mismatch from %s", p->addr_str);
        return RESP_IGNORE;
    }

    // REQUIRE: reject any unauthenticated peer (plain pre-KE, or no NTS here).
    if (g.nts_mode == NTS_MODE_REQUIRE && !p->nts) return RESP_BAD;

    if (p->nts && pkt->stratum == 0 && memcmp(&pkt->ref_id, "NTSN", 4) == 0) {
        if (!ntp_nts_response_uid_matches(raw, raw_len, p->uid)) {
            ESP_LOGW(TAG, "NTS NAK uid mismatch from %s", p->addr_str);
            return RESP_IGNORE;
        }
        // A NAK is unauthenticated by design (RFC 8915 sec 5.7): the only gates
        // are the echoed origin timestamp and the cleartext Unique ID, both of
        // which any on-path observer of our request can replay. So a single
        // (possibly forged) NAK must NOT tear down an established, working NTS
        // association or downgrade peers to plain NTP - that would let an
        // attacker strip authentication with one spoofed datagram. Keep the
        // current keys/cookies (real responses still verify) and just kick off a
        // background re-key to recover if the NAK was genuine (server rotated
        // its key). Settle this response with no sample.
        ESP_LOGW(TAG, "NTS NAK from %s; re-keying, keeping current context", p->addr_str);
        nts_request_rekey();
        return RESP_BAD;
    }

    // Authenticate before trusting anything else (including KoD and the
    // cold-boot step): per RFC 8915 an unverifiable response - forged, tampered,
    // or a stale-key mismatch - must be discarded. Verifying also matches the
    // echoed Unique ID and harvests fresh cookies into the pool.
    if (p->nts && !ntp_nts_check_response(raw, raw_len, &g.nts, p->uid)) {
        ESP_LOGW(TAG, "NTS auth failed from %s", p->addr_str);
        return RESP_IGNORE;
    }

    if (pkt->stratum == 0) {
        char code[5] = {0};
        memcpy(code, &pkt->ref_id, 4);
        ESP_LOGW(TAG, "KoD from %s: %.4s", p->addr_str, code);
        p->stratum = 16;
        p->reach   = 0;
        if (!memcmp(code, "NTSN", 4)) {
            // A prior NTSN in the same response burst may already have dropped
            // peer NTS state. Do not park this peer as if NTSN were RATE.
        } else if (!memcmp(code, "DENY", 4) || !memcmp(code, "RSTR", 4)) {
            // Deactivated slots stay eligible for replacement via
            // find_worst_eligible_peer, so this is not permanent attrition.
            p->active = false;
        } else {
            // RATE (or unknown code): per RFC 5905 the KoD's poll field
            // carries the server's minimum acceptable interval. Honor it,
            // bounded, instead of going silent for a flat hour.
            int8_t min_poll = pkt->poll;
            if (min_poll < 6)  min_poll = 6;
            if (min_poll > 12) min_poll = 12;
            p->kod_until_ms = mono_ms() + (1000UL << min_poll);
        }
        return RESP_BAD;
    }
    if (li == 3) return RESP_BAD;

    if (pkt->xmt_ts_sec == 0) return RESP_BAD;

    struct timeval t2, t3;
    ntp_to_tv(ntohl(pkt->recv_ts_sec), ntohl(pkt->recv_ts_frac), &t2);
    ntp_to_tv(ntohl(pkt->xmt_ts_sec),  ntohl(pkt->xmt_ts_frac),  &t3);

    // Era-window tripwire on the mapped server timestamps. A correct
    // ntp_to_tv maps every wire value into [anchor_epoch,
    // anchor_epoch + 2^32) by construction, so a value outside that window
    // can only come from a conversion bug - exactly how a GCC 15.2
    // miscompile that projected timestamps +2^32 s was caught here. The
    // check costs nothing in device lifetime (the window IS the
    // representable range); in-window garbage is the job of the panic
    // threshold and consensus machinery, since the cold-boot step and
    // majority-panic re-step trust t2/t3 with no other reference. Log the
    // raw words: if this ever fires we want to know what the server sent.
    int64_t sane_min = util_anchor_epoch();
    int64_t sane_max = sane_min + (1LL << 32);
    if (t2.tv_sec < sane_min || t2.tv_sec >= sane_max ||
        t3.tv_sec < sane_min || t3.tv_sec >= sane_max) {
        ESP_LOGW(TAG, "Implausible server time from %s: recv=%08lx.%08lx xmt=%08lx.%08lx",
                 p->addr_str,
                 (unsigned long)ntohl(pkt->recv_ts_sec),
                 (unsigned long)ntohl(pkt->recv_ts_frac),
                 (unsigned long)ntohl(pkt->xmt_ts_sec),
                 (unsigned long)ntohl(pkt->xmt_ts_frac));
        return RESP_BAD;
    }

    // Defensive sanity checks on server-side fields:
    //  * t3 (server transmit) must not precede t2 (server receive) - that
    //    would imply the server processed in negative time.
    //  * root_delay and root_dispersion are 16.16 seconds; anything > 16s
    //    is nonsense for a real stratum-N server.
    if (tv_diff_us(&t3, &t2) < 0) return RESP_BAD;
    if (fp1616_to_us(ntohl(pkt->root_delay))      > 16000000 ||
        fp1616_to_us(ntohl(pkt->root_dispersion)) > 16000000) return RESP_BAD;

    // Prefer the WiFi-hook timestamps (t4 from the RX cb, t1 from the TX-done
    // cb) when present. Skip early_t1 on the cold-boot path: the shift loop
    // below mutates each peer's stored t1, and reconciling that with an
    // independent early value is more bookkeeping than it's worth for an
    // event that fires at most once per boot.
    struct timeval t1_local = p->t1;
    struct timeval t4_local = *t4;
    int64_t early_us;
    if (consume_early(early_t4_ring(), pkt->orig_ts_sec, pkt->orig_ts_frac, &early_us)) {
        t4_local.tv_sec  = (time_t)(early_us / 1000000);
        t4_local.tv_usec = (suseconds_t)(early_us % 1000000);
    }
    if (g.first_sync_done) {
        if (consume_early(early_t1_ring(), pkt->orig_ts_sec, pkt->orig_ts_frac, &early_us)) {
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

        // step_clock shifts every outstanding peer's t1 (including self)
        // and the early-stamp rings into the post-step frame. Without
        // that, peers 1-3 would hit the panic threshold and get tossed,
        // and peer 0 (self) would produce a nonsense sample.
        step_clock(step_us);
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
        // A lone sample this far off is discarded. But if OUR clock is the
        // falseticker (e.g. the cold-boot step trusted a server that was
        // wildly wrong), every honest peer panics here forever: their reach
        // bits never get set, they never enter selection, and the wrong-but-
        // self-consistent selected peer keeps the staleness watchdog fed -
        // a sticky lockout with no recovery path. Escape hatch: when this
        // peer and at least one other have each panicked on consecutive
        // waves AND their panic offsets agree within PANIC_AGREE_US (honest
        // peers agree to within network noise; independent liars don't),
        // conclude we are the falseticker and re-step to the consensus. Two
        // corroborating peers could in principle be coordinated liars, but
        // a never-recovers lockout is the worse failure mode.
        if (p->panic_runs < 255) p->panic_runs++;
        p->panic_offset_us = offset;
        p->panic_last_ms   = mono_ms();

        // Corroborating votes must be recent: a peer that panicked long ago
        // and went quiet (dodging eviction) would otherwise hold a standing
        // vote that a single later liar could pair with to re-step the clock.
        uint32_t vote_window_ms = 3u * g.current_poll_s * 1000u;
        int agree = 0;
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *q = &g.peers[i];
            if (!q->active || q->panic_runs < 2) continue;
            if ((uint32_t)(p->panic_last_ms - q->panic_last_ms) > vote_window_ms) continue;
            int64_t d = q->panic_offset_us - offset;
            if (d < 0) d = -d;
            if (d <= PANIC_AGREE_US) agree++;
        }
        if (agree < 2) {
            ESP_LOGW(TAG, "Offset %lld us from %s exceeds panic threshold",
                     (long long)offset, p->addr_str);
            return RESP_BAD;
        }

        ESP_LOGW(TAG, "%d peers agree we are off by %+lld s; re-stepping",
                 agree, (long long)(offset / 1000000));
        step_clock(offset);
        t1_local = tv_from_us(tv_to_us(&t1_local) + offset);
        t4_local = tv_from_us(tv_to_us(&t4_local) + offset);

        // Every peer's filter history is in the pre-step frame - invalidate
        // it all and let the new frame refill. Reach is cleared too: the
        // formerly-selected peer's full reach register was earned against
        // the wrong clock.
        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *q = &g.peers[i];
            if (!q->active) continue;
            memset(q->filter, 0, sizeof(q->filter));
            q->filter_head      = 0;
            q->best_offset_us   = 0;
            q->best_delay_us    = 0;
            q->jitter_us        = 0;
            q->dispersion_us    = 16000000;
            q->best_sample_ms   = 0;
            q->reach            = 0;
            q->falseticker_runs = 0;
            q->jittery_runs     = 0;
            q->panic_runs       = 0;
        }
        g.selected_peer          = -1;
        g.stratum                = 16;
        g.current_poll_s         = MIN_POLL_S;
        poll_reset_pending       = true;
        g.poll_adjust            = 0;
        g.last_freq_sample_ms    = 0;
        g.last_discipline_ms     = mono_ms();
        g.last_discipline_poll_s = g.current_poll_s;
        g.sync_count++;
        g.last_sync_time         = t4_local.tv_sec;
        g.last_offset_us         = offset;

        // Recompute this sample's offset in the post-step frame (delay is
        // shift-invariant) and fall through to the normal filter path.
        offset = (tv_diff_us(&t2, &t1_local) + tv_diff_us(&t3, &t4_local)) / 2;
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
    // Only trust recent samples for the min-delay reference: a stale low delay
    // (e.g. from before a route change raised the true path RTT) would make
    // every fresh sample look asymmetric and drag legitimate offsets toward
    // zero. Mirror the sample-age bound update_peer_filter uses.
    int32_t min_delay = INT32_MAX;
    int valid_samples = 0;
    uint32_t now_hnp = mono_ms();
    uint32_t hnp_max_age_ms = ROBUST_SAMPLE_MAX_AGE_S * 1000UL;
    for (int i = 0; i < NTP_FILTER_SIZE; i++) {
        if (!p->filter[i].valid) continue;
        if (now_hnp - p->filter[i].received_ms > hnp_max_age_ms) continue;
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
    //
    // On top of the RFC terms, add half the excess RTT over the peer's recent
    // minimum: any excess could be entirely one-directional, biasing the
    // offset by up to excess/2. The huff-n-puff shift above only corrects
    // mid-range excess (1.5x-3x min) and only toward zero, so the residual
    // asymmetry uncertainty is still ~excess/2 either way. Folding it in here
    // makes spike samples quadratically less influential in the
    // inverse-dispersion-squared peer combine and honestly widens the
    // Marzullo interval -- which matters when same-uplink congestion hits
    // every peer's sample in the same splay window and the spikes would
    // otherwise pass consensus at full weight.
    int64_t rtt_us  = tv_diff_us(&t4_local, &t1_local);
    int32_t eps_phi = (rtt_us > 0)
                      ? (int32_t)((uint64_t)PHI_US_PER_SEC * rtt_us / 1000000)
                      : 0;
    int64_t excess_disp = 0;
    if (min_delay != INT32_MAX && delay > min_delay) {
        excess_disp = (delay - min_delay) / 2;
    }
    int64_t sample_disp64 = (int64_t)precision_to_us(LOCAL_PRECISION) +
                            precision_to_us(pkt->precision) +
                            eps_phi +
                            excess_disp;
    // Cap at 16 s rather than INT32_MAX: a server advertising a garbage
    // precision (>= 2^12 s) used to saturate this to INT32_MAX, and the
    // dispersion + jitter sum in update_peer_filter then overflowed int32
    // (UB). Anything past 16 s is equally meaningless and already the
    // rejection bar for root delay/dispersion.
    int32_t sample_disp = sample_disp64 > 16000000 ? 16000000
                                                   : (int32_t)sample_disp64;
    if (sample_disp < SAMPLE_DISP_FLOOR_US) sample_disp = SAMPLE_DISP_FLOOR_US;

    p->filter_head = (p->filter_head + 1) % NTP_FILTER_SIZE;
    p->filter[p->filter_head].offset_us     = (int32_t)offset;
    p->filter[p->filter_head].delay_us      = (int32_t)delay;
    p->filter[p->filter_head].dispersion_us = sample_disp;
    p->filter[p->filter_head].received_ms   = mono_ms();
    p->filter[p->filter_head].valid         = true;

    p->stratum         = pkt->stratum;
    p->root_delay_raw      = ntohl(pkt->root_delay);
    p->root_dispersion_raw = ntohl(pkt->root_dispersion);
    p->last_response_ms = mono_ms();
    p->reach |= 1;
    p->consecutive_misses = 0;
    p->panic_runs = 0;
    g.last_any_response_ms = mono_ms();
    return RESP_GOOD;
}


void handle_socket_readable(int sock, const struct timeval *t4) {
    // t4 is stamped by the caller immediately after select() returns, before
    // taking the NTP lock -- closer to actual packet arrival than stamping
    // here would be (skips lock contention + recvfrom latency). The WiFi RX
    // hook timestamp still overrides this when it matches.
    //
    // RFC 5905 header is 48 bytes, followed by optional RFC 7822 extension
    // fields and a trailing MAC (for authenticated / NTS packets). We don't
    // validate extensions or MAC, but the buffer is sized to the same worst
    // case the NTS layer accepts (NTS_RESP_BUF_LEN: header + Unique ID +
    // Authenticator carrying up to NTS_MAX_COOKIES fresh cookies of
    // NTS_COOKIE_MAX bytes each) so a large-cookie server's refill isn't
    // truncated to an unreadable prefix.
    uint8_t buf[NTS_RESP_BUF_LEN];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT,
                        (struct sockaddr *)&from, &fromlen);
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
    ntp_resp_result_t res = process_response(p, &pkt, buf, (size_t)n, t4);
    if (res == RESP_IGNORE) return;
    bool ok = (res == RESP_GOOD);
    p->request_outstanding = false;
    p->last_settle_cycle_id = p->cycle_id_when_sent;
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
    }

    // Eviction is event-driven: the falseticker/jittery counters only move
    // inside select_system_peer, so checking at every settle means one
    // try_replace_peer attempt per peer per poll cycle - naturally
    // cycle-aligned. On failure the counters stay elevated and we retry at
    // the next cycle's response burst. Running on bad responses too lets a
    // DENY-deactivated slot refill on its own (final) response instead of
    // waiting for another peer's good one. `p` may be reset/replaced
    // across this call.
    maybe_evict_worst_peer();

    try_discipline(settled_cycle_id);
}
