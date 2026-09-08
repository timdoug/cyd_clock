#include "ntp_internal.h"

void ntp_record_reference(int32_t residual_us) {
    if (g.selected_peer < 0) return;
    uint8_t mask = g.combine_mask ? g.combine_mask : 1u << g.selected_peer;
    int64_t distance = 0;
    int32_t root_delay = 0;
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        if (!(mask & (1u << i))) continue;
        ntp_peer_t *p = &g.peers[i];
        int32_t delay = fp1616_to_us(p->root_delay_raw) + p->best_delay_us;
        int64_t d = (int64_t)delay / 2 + fp1616_to_us(p->root_dispersion_raw) +
                    aged_peer_dispersion_us(p, mono_ms());
        if (d > distance) { distance = d; root_delay = delay; }
    }
    int64_t disp = distance - root_delay / 2 + llabs((int64_t)residual_us);
    g.reference_root_delay_us = root_delay;
    g.reference_dispersion_us = disp > INT32_MAX ? INT32_MAX : (int32_t)disp;
    // Until acquisition converges, allow the full possible residual between
    // our bounded crystal estimate and the physical oscillator's range.
    g.reference_freq_uncertainty_ppb = g.freq_converged ? PHI_US_PER_SEC * 1000 :
        MAX_FREQ_PPM_X1000 + abs(g.freq_ppm_x1000);
    g.reference_mono_us = esp_timer_get_time();
    g.reference_valid = true;
}

void ntp_fill_clock_quality(ntp_sys_stats_t *out) {
    out->time_valid = g.first_sync_done && g.reference_valid;
    out->synced = false;
    out->holdover = out->time_valid;
    out->reference_age_s = 0;
    if (!out->time_valid) return;
    int64_t elapsed = esp_timer_get_time() - g.reference_mono_us;
    if (elapsed < 0) elapsed = 0;
    int64_t age_s = elapsed / 1000000;
    out->reference_age_s = age_s > UINT32_MAX ? UINT32_MAX : (uint32_t)age_s;
    // Split seconds and microseconds to avoid overflowing after long uptime.
    int64_t disp = g.reference_dispersion_us +
        age_s * g.reference_freq_uncertainty_ppb / 1000 +
        (elapsed % 1000000) * g.reference_freq_uncertainty_ppb / 1000000000;
    out->root_delay_us = g.reference_root_delay_us;
    out->root_dispersion_us = disp > INT32_MAX ? INT32_MAX : (int32_t)disp;
    if (g.selected_peer >= 0 && !g.poll_paused && !g.dirty_config) {
        ntp_peer_t *p = &g.peers[g.selected_peer];
        uint32_t timeout_ms = 2 * g.current_poll_s * 1000 +
                              SPLAY_WINDOW_MS + RESPONSE_TIMEOUT_MS;
        out->synced = p->active && p->stratum > 0 && p->stratum < 16 && p->reach &&
            (uint32_t)(mono_ms() - p->last_response_ms) <= timeout_ms &&
            elapsed <= (int64_t)timeout_ms * 1000 &&
            (int64_t)out->root_delay_us / 2 + out->root_dispersion_us <= MAX_ROOT_DIST_US;
    }
    out->holdover = !out->synced;
}
