#include "ntp_internal.h"

static void adaptive_poll_update(void);

void adaptive_poll_update_once(uint32_t cycle_id) {
    if (cycle_id == 0 || last_poll_adjust_cycle_id == cycle_id) return;
    adaptive_poll_update();
    last_poll_adjust_cycle_id = cycle_id;
}

static void discipline_clock(int32_t offset_us, bool fresh) {
    bool step = !g.first_sync_done ||
                offset_us >  STEP_THRESHOLD_US ||
                offset_us < -STEP_THRESHOLD_US;
    int32_t applied_us = offset_us;

    if (step) {
        step_clock(offset_us);
        // The step is a phase discontinuity: the next residual offset only
        // measures drift accrued SINCE the step, so a freq sample computed
        // against the pre-step timestamp would be diluted by the whole
        // spanned window. Skip one learning cycle instead.
        g.last_freq_sample_ms = 0;
        ESP_LOGI(TAG, "Clock stepped %+ld us", (long)offset_us);
    } else {
        // Dampen offsets inside the clean-wave noise band and apply excess
        // in full. Before the jitter floor exists, use full phase gain so
        // startup and post-step recovery converge quickly.
        uint32_t now_ms = mono_ms();
        int32_t knee = 3 * g.freq_jitter_floor;
        if (knee > FREQ_MAX_OFFSET_US) knee = FREQ_MAX_OFFSET_US;
        if (g.last_freq_sample_ms == 0) knee = 0;
        int32_t mag  = offset_us < 0 ? -offset_us : offset_us;
        int32_t band = mag < knee ? mag : knee;
        int32_t applied_mag = mag - band + band / 8;
        applied_us = offset_us < 0 ? -applied_mag : applied_mag;

        struct timeval outstanding = {0};
        adjtime(NULL, &outstanding);
        struct timeval delta = tv_from_us(tv_to_us(&outstanding) + applied_us);
        adjtime(&delta, NULL);
        // (The slew is applied by the kernel over the next fraction of a
        // second; re-framing the filters below treats it as immediate,
        // which is within the noise for the few-ms slews discipline emits.)

        // Learn drift only from clean residual phase error. The PLL form is
        // step = offset/TAU, so the frequency loop has a ~TAU time constant
        // at any poll interval; the adaptive jitter floor keeps public-NTP
        // path noise out of the crystal estimate.
        int32_t freq_step = 0;
        bool learned_freq = false;
        bool noisy = false;
        bool gateable = fresh &&
            g.last_freq_sample_ms != 0 &&
            !g.select_spread_wide &&
            g.system_jitter_us <= ROBUST_FREQ_MAX_JITTER_US &&
            offset_us < ROBUST_FREQ_MAX_OFFSET_US &&
            offset_us > -ROBUST_FREQ_MAX_OFFSET_US;
        noisy = fresh &&
            g.last_freq_sample_ms != 0 &&
            (g.select_spread_wide ||
             g.system_jitter_us > ROBUST_FREQ_MAX_JITTER_US ||
             offset_us >= ROBUST_FREQ_MAX_OFFSET_US ||
             offset_us <= -ROBUST_FREQ_MAX_OFFSET_US);
        if (gateable) {
            int32_t j = g.system_jitter_us;
            bool clean;
            if (g.freq_jitter_floor <= 0) {
                g.freq_jitter_floor = j > 0 ? j : 1;
                clean = true;
            } else {
                int32_t clean_allowance = g.freq_jitter_floor / 4;
                clean = j <= g.freq_jitter_floor + clean_allowance;
                if (j < g.freq_jitter_floor)
                    g.freq_jitter_floor -= (g.freq_jitter_floor - j) / 4;
                else
                    g.freq_jitter_floor += (j - g.freq_jitter_floor) / 16;
                if (g.freq_jitter_floor < 1) g.freq_jitter_floor = 1;
            }
            noisy = !clean;
            uint32_t elapsed_ms = now_ms - g.last_freq_sample_ms;
            if (clean && elapsed_ms >= (MIN_POLL_S / 2) * 1000) {
                freq_step = (int32_t)(((int64_t)offset_us * 1000) / FREQ_TAU_S);
                freq_step = clamp_i32(freq_step, -MAX_FREQ_STEP_PPB, MAX_FREQ_STEP_PPB);
                g.freq_ppm_x1000 += freq_step;
                g.freq_ppm_x1000 = clamp_i32(g.freq_ppm_x1000,
                                             -MAX_FREQ_PPM_X1000,
                                              MAX_FREQ_PPM_X1000);
                g.freq_learned_this_session = true;
                learned_freq = true;
            }
        }
        // Advance this every discipline; it only gates whether enough time
        // has passed to bother with a frequency update.
        g.last_freq_sample_ms = now_ms;

        const char *freq_note = learned_freq ? "step"
                              : !fresh        ? "skip(forced)"
                              : noisy         ? "skip(noisy)"
                                              : "skip(soon)";
        ESP_LOGI(TAG, "Clock slewed %+ld of %+ld us (jit %ld/%ld, freq %+ld ppb %s %+ld)",
                 (long)applied_us, (long)offset_us, (long)g.system_jitter_us,
                 (long)g.freq_jitter_floor, (long)g.freq_ppm_x1000,
                 freq_note, (long)freq_step);

        // Persist slowly enough for NVS wear; the delta gate just suppresses
        // rewrites below what a fresh boot can resolve.
        static uint32_t last_freq_save_ms;
        static int32_t  last_saved_freq    = INT32_MIN;
        const  uint32_t SAVE_INTERVAL_MS   = 30 * 60 * 1000;
        const  int32_t  SAVE_DELTA_PPB     = 10;
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

    shift_filters(applied_us);

    g.first_sync_done = true;
    g.last_offset_us  = offset_us;
    struct timeval now;
    gettimeofday(&now, NULL);
    g.last_sync_time = now.tv_sec;
    g.sync_count++;
}



static void adaptive_poll_update(void) {
    if (!g.first_sync_done) {
        g.current_poll_s = MIN_POLL_S;
        g.poll_adjust = 0;
        return;
    }
    if (g.current_poll_s == 0) g.current_poll_s = MIN_POLL_S;

    const int8_t GOOD_RUN = 4;
    const int8_t BAD_RUN  = 2;

    ntp_peer_t *sp = (g.selected_peer >= 0) ? &g.peers[g.selected_peer] : NULL;
    bool responded = sp && (sp->reach & 0x01);

    int64_t signed_off = g.last_offset_us;
    int64_t off = signed_off;
    if (off < 0) off = -off;

    int32_t grow_offset_max = ROBUST_COMBINE_SPREAD_US;
    if (g.freq_jitter_floor > 0 && 4 * g.freq_jitter_floor > grow_offset_max) {
        grow_offset_max = 4 * g.freq_jitter_floor;
    }
    if (grow_offset_max > ROBUST_FREQ_MAX_OFFSET_US) {
        grow_offset_max = ROBUST_FREQ_MAX_OFFSET_US;
    }
    const int32_t shrink_offset_max = ROBUST_FREQ_MAX_OFFSET_US;

    bool good = responded &&
                g.system_jitter_us <= ROBUST_FREQ_MAX_JITTER_US &&
                off <= grow_offset_max;
    bool bad = !responded ||
               off > shrink_offset_max;
    int8_t bad_sign = 0;
    if (bad && responded && off > shrink_offset_max) {
        bad_sign = (signed_off > 0) ? 1 : (signed_off < 0) ? -1 : 0;
    }

    // Saturate the counter at the run thresholds: at MAX_POLL_S (or pinned to
    // MIN_POLL_S during an outage) nothing resets it, and an int8_t would
    // wrap after 128 cycles - turning a long good streak into a phantom bad
    // one and letting a single bad poll shrink the interval.
    if (good) {
        if (g.poll_adjust < 0) g.poll_adjust = 0;
        g.poll_bad_sign = 0;
        if (g.poll_adjust < GOOD_RUN) g.poll_adjust++;
        if (g.poll_adjust >= GOOD_RUN && g.current_poll_s < MAX_POLL_S) {
            g.current_poll_s *= 2;
            g.poll_adjust = 0;
        }
    } else if (bad) {
        if (g.poll_adjust > 0) g.poll_adjust = 0;
        if (bad_sign != 0 && g.poll_bad_sign != 0 &&
            g.poll_bad_sign != bad_sign && g.poll_adjust < 0) {
            g.poll_adjust = 0;
        }
        g.poll_bad_sign = bad_sign;
        if (g.poll_adjust > -BAD_RUN) g.poll_adjust--;
        if (g.poll_adjust <= -BAD_RUN && g.current_poll_s > MIN_POLL_S) {
            g.current_poll_s /= 2;
            g.poll_adjust = 0;
        }
    } else {
        g.poll_adjust = 0;
        g.poll_bad_sign = 0;
    }

    if (g.current_poll_s < MIN_POLL_S) g.current_poll_s = MIN_POLL_S;
    if (g.current_poll_s > MAX_POLL_S) g.current_poll_s = MAX_POLL_S;
}


void apply_freq_correction(void) {
    uint32_t now = mono_ms();
    if (g.last_freq_apply_ms == 0) { g.last_freq_apply_ms = now; return; }
    uint32_t elapsed_ms = now - g.last_freq_apply_ms;
    if (elapsed_ms < 1000) return;

    // delta_us = (freq_ppm_x1000 / 1000) ppm * (elapsed_ms / 1000) s * 1 us/(ppm*s)
    //          = freq_ppm_x1000 * elapsed_ms / 1_000_000
    // Accumulate in ppb*ms (1e6 ppb*ms = 1 us) and carry the sub-us remainder
    // forward instead of truncating it away. Discarding the fraction at each
    // application loses up to ~1 us per apply interval -- an effective
    // dead-band of up to ~1 ppm pulling toward zero that the PI loop would
    // otherwise absorb as a biased freq estimate.
    int64_t accum    = (int64_t)g.freq_ppm_x1000 * elapsed_ms + g.freq_apply_residual;
    int64_t delta_us = accum / 1000000;
    g.freq_apply_residual = accum - delta_us * 1000000;
    g.last_freq_apply_ms  = now;
    if (delta_us == 0) return;

    struct timeval outstanding = {0};
    adjtime(NULL, &outstanding);
    struct timeval merged_tv = tv_from_us(tv_to_us(&outstanding) + delta_us);
    adjtime(&merged_tv, NULL);
}

bool try_discipline(uint32_t settled_cycle_id) {
    if (settled_cycle_id == 0 || g.selected_peer < 0) return false;

    uint32_t now = mono_ms();
    uint32_t ref_poll_s = g.last_discipline_poll_s ?
                          g.last_discipline_poll_s : g.current_poll_s;
    int32_t  threshold_ms = ((int32_t)ref_poll_s - 3) * 1000;
    int32_t  since_disc = (int32_t)(now - g.last_discipline_ms);
    // A negative diff can only mean last_discipline_ms froze for over 2^31 ms
    // (e.g. weeks with no selectable peer); that is overdue, not early --
    // without this clamp discipline would stay locked out until the counter
    // wrapped back around, up to another ~24.8 days.
    if (since_disc < 0) since_disc = INT32_MAX;
    bool basic_due = (g.last_discipline_ms == 0) || since_disc >= threshold_ms;
    if (!basic_due) return false;

    bool all_settled = true;
    int  responded   = 0;
    int  reachable   = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *q = &g.peers[i];
        if (!q->active) continue;
        if (q->stratum == 0 || q->stratum >= 16) continue;
        if (q->kod_until_ms && (int32_t)(now - q->kod_until_ms) < 0) continue;
        if (q->consecutive_misses >= 2) continue;
        if (q->cycle_id_when_sent != settled_cycle_id &&
            q->next_poll_cycle_id != settled_cycle_id) continue;
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
    g.last_discipline_poll_s = g.current_poll_s;
    discipline_clock(g.combined_offset_us, good);
    adaptive_poll_update_once(settled_cycle_id);
    return true;
}
