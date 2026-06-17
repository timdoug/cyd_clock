#include "ntp_internal.h"
#include "wifi.h"

const char *NTP_TAG = "ntp";

ntp_state_t g = {
    .selected_peer = -1,
    .stratum = 16,
    .wake_sock = -1,
    .sock4 = -1,
    .sock6 = -1,
};

uint32_t next_global_poll_ms;
uint32_t next_global_poll_cycle_id = 1;
uint32_t last_poll_adjust_cycle_id;
uint32_t last_evict_tick_ms = UINT32_MAX;

static void ntp_task(void *arg) {
    (void)arg;
    g.sync_start_ms = mono_ms();
    g.current_poll_s = MIN_POLL_S;
    open_wake_sock();

    while (g.running) {
        lock_take();

        apply_freq_correction();

        if (g.nts_rebind) {
            g.nts_rebind = false;
            nts_rebind_peers();
        }

        // Staleness watchdog: if we've heard from no peer for several poll
        // cycles, shrink the poll interval back toward MIN_POLL_S and re-resolve
        // DNS. Handles the case where all cached pool IPs went away (network
        // change, pool rotation, etc.) - otherwise we'd keep retrying the same
        // dead addresses on a grown poll cap forever.
        if (g.last_any_response_ms != 0) {
            uint32_t dead_ms = mono_ms() - g.last_any_response_ms;
            uint32_t threshold_ms = g.current_poll_s * 4 * 1000;
            if (dead_ms > threshold_ms) {
                ESP_LOGW(TAG, "No peer responses in %lus; re-resolving, poll %lus -> %ds",
                         (unsigned long)(dead_ms / 1000),
                         (unsigned long)g.current_poll_s, MIN_POLL_S);
                g.dirty_config    = true;
                g.current_poll_s  = MIN_POLL_S;
                g.last_discipline_poll_s = 0;
                g.last_freq_sample_ms = 0;
                g.poll_adjust     = 0;
                g.last_any_response_ms = mono_ms();
            }
        }

        if (g.dirty_config) {
            g.dirty_config = false;
            // Avoid double-spawning the 16 KB KE task during config churn.
            bool nts_ke_in_flight = g.nts.ke_in_flight;
            uint32_t nts_ke_generation = g.nts.ke_generation;
            memset(&g.nts, 0, sizeof(g.nts));
            if (nts_ke_in_flight) {
                g.nts.ke_in_flight = true;
                g.nts.ke_generation = nts_ke_generation;
            } else {
                g.nts.ke_generation = nts_ke_generation + 1;
                if (g.nts.ke_generation == 0) g.nts.ke_generation = 1;
            }
            g.nts_rebind = false;
            next_global_poll_ms = 0;
            next_global_poll_cycle_id++;
            if (next_global_poll_cycle_id == 0) next_global_poll_cycle_id = 1;
            last_evict_tick_ms = UINT32_MAX;
            last_poll_adjust_cycle_id = 0;
            resolve_peers();
            // Arm the staleness watchdog against this resolution: even before
            // any peer responds, we'll retrigger after threshold_ms if the new
            // set of IPs is also silent (e.g. DNS gave stale results).
            g.last_any_response_ms = mono_ms();
            // Keep the learned crystal drift across peer-set changes.
            g.current_poll_s         = MIN_POLL_S;
            g.last_discipline_poll_s = 0;
            g.last_freq_sample_ms    = 0;
            g.poll_adjust            = 0;
            // Reset sync accounting so the drilldown doesn't display a stale
            // Syncs count / Age value tied to the previous server. Keep
            // first_sync_done so we don't re-step the clock on the first
            // response from the new server - the local clock is already set.
            g.sync_count             = 0;
            g.last_sync_time         = 0;
        }
        if (!open_sockets()) {
            lock_give();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = mono_ms();
        uint32_t next_wake = now + IDLE_WAKE_MS;

        // Plain NTP success must not permanently mask a transient KE failure.
        if (!g.nts.valid) nts_start_ke_if_needed();

        for (int i = 0; i < NTP_MAX_PEERS; i++) {
            ntp_peer_t *p = &g.peers[i];
            if (!p->active) continue;

            if (p->kod_until_ms && (int32_t)(now - p->kod_until_ms) < 0) {
                if ((int32_t)(p->kod_until_ms - next_wake) < 0) next_wake = p->kod_until_ms;
                continue;
            }

            if (p->request_outstanding &&
                (int32_t)(now - p->request_sent_ms) >= RESPONSE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "TIMEOUT peer=%s reach=%02x", p->addr_str, p->reach);
                uint32_t settled_cycle_id = p->cycle_id_when_sent;
                p->request_outstanding   = false;
                p->last_settle_cycle_id  = settled_cycle_id;
                if (p->consecutive_misses < 255) p->consecutive_misses++;
                schedule_after_request(p);
                // Swap out chronically-bad peers. The peer actually evicted
                // is the WORST currently-eligible one - it might be another
                // peer with higher falseticker/jittery runs than this one's
                // misses. `p` may be reset/replaced and `now` is stale
                // after this call (it can drop the lock for DNS).
                maybe_evict_worst_peer();
                // Re-evaluate the discipline gate: this timeout may have
                // been the last unresolved peer of the wave (the response
                // path won't fire again for it).
                bool disciplined = try_discipline(settled_cycle_id);
                // If no discipline fired, still treat a selected-peer timeout
                // as a bad poll-adjust event so the interval can shrink.
                if (!disciplined && g.selected_peer == i) {
                    adaptive_poll_update_once(settled_cycle_id);
                }
            }

            bool due = g.force_sync || (int32_t)(now - p->next_poll_ms) >= 0;
            if (due && !p->request_outstanding) {
                if (send_request(p)) {
                    uint32_t deadline = p->request_sent_ms + RESPONSE_TIMEOUT_MS;
                    if ((int32_t)(deadline - next_wake) < 0) next_wake = deadline;
                } else {
                    if ((int32_t)(p->next_poll_ms - next_wake) < 0) next_wake = p->next_poll_ms;
                }
            } else if (p->request_outstanding) {
                uint32_t deadline = p->request_sent_ms + RESPONSE_TIMEOUT_MS;
                if ((int32_t)(deadline - next_wake) < 0) next_wake = deadline;
            } else {
                if ((int32_t)(p->next_poll_ms - next_wake) < 0) next_wake = p->next_poll_ms;
            }
        }
        g.force_sync = false;

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (g.sock4 >= 0)     { FD_SET(g.sock4, &rfds);     if (g.sock4 > maxfd)     maxfd = g.sock4; }
        if (g.sock6 >= 0)     { FD_SET(g.sock6, &rfds);     if (g.sock6 > maxfd)     maxfd = g.sock6; }
        if (g.wake_sock >= 0) { FD_SET(g.wake_sock, &rfds); if (g.wake_sock > maxfd) maxfd = g.wake_sock; }

        uint32_t wait_ms = (int32_t)(next_wake - now) > 0 ? (next_wake - now) : 0;
        if (wait_ms > IDLE_WAKE_MS) wait_ms = IDLE_WAKE_MS;

        lock_give();

        struct timeval tv = {
            .tv_sec  = wait_ms / 1000,
            .tv_usec = (wait_ms % 1000) * 1000,
        };
        int sr = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (sr > 0) {
            // Stamp t4 before taking the lock: any datagram select() flagged
            // arrived no later than this, and lock acquisition can add real
            // latency (UI stats getters hold it briefly).
            struct timeval t4_sel;
            gettimeofday(&t4_sel, NULL);
            lock_take();
            if (g.sock4 >= 0     && FD_ISSET(g.sock4, &rfds))     handle_socket_readable(g.sock4, &t4_sel);
            if (g.sock6 >= 0     && FD_ISSET(g.sock6, &rfds))     handle_socket_readable(g.sock6, &t4_sel);
            if (g.wake_sock >= 0 && FD_ISSET(g.wake_sock, &rfds)) drain_wake_sock();
            lock_give();
        }
    }

    close_sockets();
    close_wake_sock();
    g.task = NULL;
    vTaskDelete(NULL);
}


void ntp_init(const char *server, bool prefer_ipv6) {
    if (g.running) ntp_stop();
    if (!g.lock) g.lock = xSemaphoreCreateMutex();

    str_copy(g.server, sizeof(g.server), server ? server : DEFAULT_NTP_SERVER);
    g.prefer_ipv6    = prefer_ipv6;
    g.current_poll_s = MIN_POLL_S;
    g.first_sync_done = false;
    g.sync_count     = 0;
    g.selected_peer  = -1;
    g.stratum        = 16;
    g.dirty_config   = true;
    g.force_sync     = false;
    g.last_sync_time = 0;
    g.last_offset_us = 0;
    g.system_jitter_us = 0;
    g.root_delay_us = 0;
    g.root_dispersion_us = 0;
    g.combined_offset_us = 0;
    g.last_freq_apply_ms = 0;
    g.freq_apply_residual = 0;
    g.last_freq_sample_ms = 0;
    g.freq_jitter_floor = 0;
    g.last_discipline_ms = 0;
    g.last_discipline_poll_s = 0;
    g.last_any_response_ms = 0;
    g.poll_adjust = 0;
    memset(&g.nts, 0, sizeof(g.nts));
    g.nts_rebind = false;
    next_global_poll_ms = 0;
    next_global_poll_cycle_id++;
    if (next_global_poll_cycle_id == 0) next_global_poll_cycle_id = 1;
    last_evict_tick_ms = UINT32_MAX;
    last_poll_adjust_cycle_id = 0;
    // Restore the persisted crystal-drift estimate so we start near-converged
    // instead of the ~30 minutes the PI loop normally needs to settle on the
    // hardware-intrinsic value from a cold 0 ppm seed.
    int32_t saved_freq = 0;
    g.freq_ppm_x1000 = 0;
    g.freq_loaded_from_nvs = false;
    g.freq_learned_this_session = false;
    if (nvs_config_get_freq_ppm_x1000(&saved_freq) &&
        saved_freq >  -MAX_FREQ_PPM_X1000 &&
        saved_freq <   MAX_FREQ_PPM_X1000) {
        g.freq_ppm_x1000       = saved_freq;
        g.freq_loaded_from_nvs = true;
        ESP_LOGI(TAG, "Restored freq estimate: %+ld ppb", (long)saved_freq);
    }

    g.running = true;
    // Pinned to core 0 with WiFi and lwip; core 1 is reserved for the render
    // loop (main task) so network work can't stretch a display tick mid-draw.
    // 8 KB (up from 4 KB): NTS responses add a ~1 KB receive buffer and the
    // in-place AEAD verify/cookie-harvest on top of the existing select loop.
    if (xTaskCreatePinnedToCore(ntp_task, "ntp", 8192, NULL, 5, &g.task, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NTP task");
        g.running = false;
        g.task = NULL;
    }
}


void ntp_stop(void) {
    if (!g.running) return;
    g.running = false;
    wake_task();
    while (g.task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void ntp_set_server(const char *server) {
    if (!g.lock || !server) return;
    lock_take();
    str_copy(g.server, sizeof(g.server), server);
    g.dirty_config = true;
    g.force_sync   = true;
    lock_give();
    wake_task();
}


void ntp_set_prefer_ipv6(bool prefer) {
    if (!g.lock) return;
    lock_take();
    bool changed = (g.prefer_ipv6 != prefer);
    if (changed) {
        g.prefer_ipv6 = prefer;
        g.dirty_config = true;
        g.force_sync   = true;
    }
    lock_give();
    if (changed) wake_task();
}


static void fill_sys_stats(ntp_sys_stats_t *out) {
    out->synced         = g.first_sync_done;
    out->last_sync_time = g.last_sync_time;
    out->sync_count     = g.sync_count;
    out->current_poll_s = g.current_poll_s;
    out->sync_elapsed_ms = g.first_sync_done ? 0 : (mono_ms() - g.sync_start_ms);
    out->last_offset_us = g.last_offset_us;
    out->system_jitter_us    = g.system_jitter_us;
    out->root_delay_us       = g.root_delay_us;
    // Age dispersion by PHI * seconds since we last heard from the selected
    // peer so the reported +/- bound grows honestly between polls.
    out->root_dispersion_us  = g.root_dispersion_us;
    if (g.selected_peer >= 0) {
        ntp_peer_t *sp = &g.peers[g.selected_peer];
        out->root_dispersion_us = fp1616_to_us(sp->root_dispersion_raw) +
                                  aged_peer_dispersion_us(sp, mono_ms());
    }
    out->freq_ppm_x1000      = g.freq_ppm_x1000;
    out->freq_known          = g.freq_loaded_from_nvs || g.freq_learned_this_session;
    out->stratum        = g.stratum;
    out->selected_peer  = (g.selected_peer < 0) ? 0xFF : (uint8_t)g.selected_peer;
    out->nts_active     = g.nts.valid;
    str_copy(out->server, sizeof(out->server), g.server);
}


static bool fill_peer_stats(int idx, ntp_peer_stats_t *out) {
    ntp_peer_t *p = &g.peers[idx];
    if (!p->active) return false;

    out->active    = true;
    out->selected  = (g.selected_peer == idx);
    out->stratum   = p->stratum;
    out->reach     = p->reach;
    out->offset_us     = p->best_offset_us;
    out->delay_us      = p->best_delay_us;
    out->jitter_us     = p->jitter_us;
    out->dispersion_us = aged_peer_dispersion_us(p, mono_ms());
    out->last_response_ms = p->last_response_ms
        ? (mono_ms() - p->last_response_ms)
        : UINT32_MAX;
    out->fresh = (int32_t)(mono_ms() - p->fresh_until_ms) < 0;
    out->nts   = p->nts;
    str_copy(out->addr_str, sizeof(out->addr_str), p->addr_str);
    return true;
}


void ntp_get_sys_stats(ntp_sys_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g.lock) { out->stratum = 16; out->selected_peer = 0xFF; return; }

    lock_take();
    fill_sys_stats(out);
    lock_give();
}


bool ntp_get_peer_stats(int idx, ntp_peer_stats_t *out) {
    if (!out || idx < 0 || idx >= NTP_MAX_PEERS || !g.lock) return false;
    lock_take();
    memset(out, 0, sizeof(*out));
    bool ok = fill_peer_stats(idx, out);
    lock_give();
    return ok;
}


void ntp_get_all_stats(ntp_sys_stats_t *sys, ntp_peer_stats_t peers[NTP_MAX_PEERS]) {
    if (!sys || !peers) return;
    memset(sys, 0, sizeof(*sys));
    memset(peers, 0, sizeof(ntp_peer_stats_t) * NTP_MAX_PEERS);
    if (!g.lock) { sys->stratum = 16; sys->selected_peer = 0xFF; return; }

    lock_take();
    fill_sys_stats(sys);
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        fill_peer_stats(i, &peers[i]);
    }
    lock_give();
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
        str_copy(buf, len, g.peers[idx].addr_str);
    }
    lock_give();
}
