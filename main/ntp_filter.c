#include "ntp_internal.h"


void update_peer_filter(ntp_peer_t *p) {
    int valid = 0;
    for (int i = 0; i < NTP_FILTER_SIZE; i++) {
        if (p->filter[i].valid) valid++;
    }
    if (valid == 0) {
        p->best_offset_us = 0;
        p->best_delay_us  = 0;
        p->jitter_us      = 0;
        p->dispersion_us  = 16000000;
        p->best_sample_ms = 0;
        return;
    }

    int best_idx = p->filter_head;
    if (valid > 1) {
        uint32_t now = mono_ms();
        uint32_t max_age_ms = ROBUST_SAMPLE_MAX_AGE_S * 1000UL;
        for (int i = 0; i < NTP_FILTER_SIZE; i++) {
            if (!p->filter[i].valid) continue;
            uint32_t age_ms = now - p->filter[i].received_ms;
            if (age_ms > max_age_ms) continue;
            if (p->filter[i].delay_us < p->filter[best_idx].delay_us) {
                best_idx = i;
            }
        }
    }

    // Queueing and route asymmetry show up as higher RTT. Use the
    // lowest-delay recent sample, but age its dispersion from when that
    // sample was actually received so stale wins do not look fresh.
    const ntp_sample_t *best = &p->filter[best_idx];
    p->best_offset_us = best->offset_us;
    p->best_delay_us  = best->delay_us;
    p->best_sample_ms = best->received_ms;

    if (valid > 1) {
        // Jitter: RMS difference of other samples from the chosen offset.
        double sum_sq = 0.0;
        int n = 0;
        for (int i = 0; i < NTP_FILTER_SIZE; i++) {
            if (!p->filter[i].valid || i == best_idx) continue;
            double d = (double)p->filter[i].offset_us - (double)p->best_offset_us;
            sum_sq += d * d;
            n++;
        }
        // Saturate: two large-but-subpanic offsets (+/-999 s both pass the
        // panic check) give an RMS above INT32_MAX, and casting that double
        // to int32 is UB. 16 s of jitter is as meaningless as 16 s of
        // anything else here, and the cap keeps downstream sums overflow-free.
        double rms = n > 0 ? sqrt(sum_sq / n) : 0.0;
        p->jitter_us = rms > 16000000.0 ? 16000000 : (int32_t)rms;
    } else {
        int32_t half_delay = best->delay_us / 2;
        p->jitter_us = half_delay > 16000000 ? 16000000 : half_delay;
    }
    p->dispersion_us = best->dispersion_us + p->jitter_us;
}


int32_t aged_peer_dispersion_us(const ntp_peer_t *p, uint32_t now_ms) {
    int64_t disp = p->dispersion_us;
    if (p->best_sample_ms != 0) {
        uint32_t age_ms = now_ms - p->best_sample_ms;
        disp += (int64_t)PHI_US_PER_SEC * age_ms / 1000;
    } else {
        disp += 16000000LL;
    }
    if (disp > INT32_MAX) return INT32_MAX;
    return (int32_t)disp;
}

void shift_filters(int32_t offset_us) {
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active) continue;
        bool any = false;
        for (int j = 0; j < NTP_FILTER_SIZE; j++) {
            if (!p->filter[j].valid) continue;
            int64_t adj = (int64_t)p->filter[j].offset_us - offset_us;
            if (adj > INT32_MAX) adj = INT32_MAX;
            if (adj < INT32_MIN) adj = INT32_MIN;
            p->filter[j].offset_us = (int32_t)adj;
            any = true;
        }
        if (any) update_peer_filter(p);
    }
}


void select_system_peer(void) {
    struct { int idx; int64_t lo; int64_t hi; } c[NTP_MAX_PEERS];
    uint32_t now = mono_ms();
    int n = 0;
    g.select_spread_wide = false;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *p = &g.peers[i];
        if (!p->active || p->reach == 0) continue;
        if (p->stratum == 0 || p->stratum >= 16) continue;
        // RFC 5905 root distance: the server's own uncertainty about UTC,
        // root_delay/2 + root_dispersion. A server that lost its upstream
        // advertises a huge root dispersion; MAXDIST drops it from selection
        // entirely (otherwise it can pass the 16 s field-sanity bar, present a
        // deceptively narrow interval, and evict honest peers as falsetickers).
        int64_t root_dist_us = (int64_t)fp1616_to_us(p->root_delay_raw) / 2 +
                               fp1616_to_us(p->root_dispersion_raw);
        if (root_dist_us > MAX_ROOT_DIST_US) continue;
        // Bounded delay allowance leaves room for real path asymmetry without
        // letting RTT dominate consensus; peer dispersion already includes
        // filter jitter. Fold in the server root distance so a peer that is
        // honestly unsure of UTC widens its interval instead of winning narrow.
        int32_t delay_allowance = p->best_delay_us / 4;
        if (delay_allowance > 10000) delay_allowance = 10000;
        int64_t unc = (int64_t)delay_allowance + aged_peer_dispersion_us(p, now) +
                      root_dist_us;
        c[n].idx = i;
        c[n].lo  = (int64_t)p->best_offset_us - unc;
        c[n].hi  = (int64_t)p->best_offset_us + unc;
        n++;
    }
    if (n == 0) {
        g.selected_peer = -1;
        g.stratum = 16;
        return;
    }

    // Largest subset of intervals sharing a common point (max_lo <= min_hi).
    // n <= 4, so brute-force 2^n - 1 masks. Ties on subset size are broken
    // by HYSTERESIS: prefer the cluster containing the currently selected
    // peer. Without it, a 2-2 split resolves by mask enumeration order -
    // i.e. lowest peer index - and selection can flap between clusters as
    // jitter nudges the intervals.
    int sel_bit = -1;
    for (int i = 0; i < n; i++) {
        if (c[i].idx == g.selected_peer) { sel_bit = i; break; }
    }
    int best_count = 0;
    int best_mask  = 0;
    for (int mask = 1; mask < (1 << n); mask++) {
        int count = __builtin_popcount(mask);
        if (count < best_count) continue;
        int64_t lo = INT64_MIN, hi = INT64_MAX;
        for (int i = 0; i < n; i++) {
            if (!(mask & (1 << i))) continue;
            if (c[i].lo > lo) lo = c[i].lo;
            if (c[i].hi < hi) hi = c[i].hi;
        }
        if (lo > hi) continue;
        bool better = count > best_count;
        if (!better && sel_bit >= 0 &&
            (mask & (1 << sel_bit)) && !(best_mask & (1 << sel_bit))) {
            better = true;
        }
        if (better) {
            best_count = count;
            best_mask  = mask;
        }
    }
    if (best_count == 0) {
        g.selected_peer = -1;
        g.stratum = 16;
        return;
    }

    // Quality-counter epoch: select_system_peer runs once per good RESPONSE
    // (up to 4x per wave), so increments are tagged with the wave id and
    // applied at most once per poll cycle - otherwise the eviction
    // thresholds fire ~4x faster than their comments claim, and a freshly
    // swapped-in peer (whose single-sample jitter is delay/2) can be
    // evicted as "jittery" before its 8-sample filter ever fills.
    //
    // Blame also requires a STRICT MAJORITY of candidates in the winning
    // cluster: a 2-2 split or an everything-disjoint round is evidence of
    // ambiguity, not of any particular peer lying, so counters stay
    // untouched (selection still proceeds - the clock needs an answer even
    // when the vote is ugly).
    bool majority = (best_count * 2) > n;

    int chosen = -1;
    int32_t best_jitter = INT32_MAX;
    for (int i = 0; i < n; i++) {
        ntp_peer_t *pp = &g.peers[c[i].idx];
        if (best_mask & (1 << i)) {
            if (majority) pp->falseticker_runs = 0;
            if (pp->jitter_us < best_jitter) {
                best_jitter = pp->jitter_us;
                chosen = c[i].idx;
            }
        } else if (majority &&
                   pp->quality_cycle_id != next_global_poll_cycle_id) {
            if (pp->falseticker_runs < 255) pp->falseticker_runs++;
            pp->quality_cycle_id = next_global_poll_cycle_id;
        }
    }
    if (chosen < 0) { g.selected_peer = -1; g.stratum = 16; return; }

    // Wide truechimers raise combined variance even while staying inside
    // Marzullo consensus. Compare against the cleanest survivor so this
    // adapts to link quality instead of using an absolute jitter floor.
    const int32_t JITTER_REL_X = 4;
    for (int i = 0; i < n; i++) {
        ntp_peer_t *pp = &g.peers[c[i].idx];
        bool wide = (best_mask & (1 << i)) &&
                    pp->jitter_us > JITTER_REL_X * best_jitter;
        if (wide) {
            if (majority &&
                pp->quality_cycle_id != next_global_poll_cycle_id) {
                if (pp->jittery_runs < 255) pp->jittery_runs++;
                pp->quality_cycle_id = next_global_poll_cycle_id;
            }
        } else if (majority) {
            pp->jittery_runs = 0;
        }
    }

    ntp_peer_t *sp = &g.peers[chosen];
    g.selected_peer     = chosen;
    g.stratum           = (sp->stratum < 15) ? sp->stratum + 1 : 15;
    g.root_delay_us     = fp1616_to_us(sp->root_delay_raw) + sp->best_delay_us;

    // Combine survivors only while their spread is within the useful
    // accuracy target; wider spreads are probably fixed route/server bias.
    // Weights use 1/dispersion^2 and system jitter uses the weighted-mean
    // variance, so clean peers dominate and comparable peers average down.
    {
        int32_t min_off = INT32_MAX;
        int32_t max_off = INT32_MIN;
        for (int i = 0; i < n; i++) {
            if (!(best_mask & (1 << i))) continue;
            ntp_peer_t *pp = &g.peers[c[i].idx];
            if (pp->best_offset_us < min_off) min_off = pp->best_offset_us;
            if (pp->best_offset_us > max_off) max_off = pp->best_offset_us;
        }
        bool survivor_spread_wide = min_off != INT32_MAX &&
            (int64_t)max_off - min_off > ROBUST_COMBINE_SPREAD_US;
        g.select_spread_wide = survivor_spread_wide;

        double num_off = 0.0, num_jit_var = 0.0, denom = 0.0;
        if (!survivor_spread_wide) {
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
