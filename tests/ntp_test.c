#include "ntp_host.h"

static void test_holdover(void) {
    reset();
    ntp_sys_stats_t stats = {0};
    ntp_fill_clock_quality(&stats);
    assert(!stats.time_valid && !stats.synced && !stats.holdover);
    g.first_sync_done = true;
    ntp_peer_t *p = make_peer(0);
    sample(p, 0, 0, 1000, test_ms);
    p->last_response_ms = test_ms;
    select_system_peer();
    ntp_record_reference(0);
    ntp_fill_clock_quality(&stats);
    assert(stats.time_valid && stats.synced && !stats.holdover);
    int32_t base = stats.root_dispersion_us;
    test_ms += 10000;
    ntp_fill_clock_quality(&stats);
    assert(stats.root_dispersion_us >= base + 5000); // uncalibrated crystal
    g.selected_peer = -1;
    memset(g.peers, 0, sizeof(g.peers));
    ntp_fill_clock_quality(&stats);
    assert(stats.time_valid && !stats.synced && stats.holdover);
    test_ms += 10000;
    ntp_fill_clock_quality(&stats);
    assert(stats.reference_age_s == 20 && stats.root_dispersion_us >= base+10000);
    // Peer-slot resets and even a 32-bit tick wrap cannot freeze uncertainty.
    test_mono_extra_us = 60LL * 86400 * 1000000;
    ntp_fill_clock_quality(&stats);
    assert(stats.root_dispersion_us == INT32_MAX);
    assert(stats.reference_age_s >= 60u * 86400);
}

static void test_frequency_cadence(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        reset();
        test_ms = UINT32_MAX - 999;
        g.freq_ppm_x1000 = sign * 20000;
        apply_freq_correction();
        int64_t total = 0;
        for (int i = 0; i < 10000; i++) {
            test_ms += FREQ_APPLY_INTERVAL_MS;
            pending_us = 0; // prior sub-5us slew has completed
            apply_freq_correction();
            assert(llabs(pending_us) <= 1);
            total += pending_us;
        }
        assert(total == sign * 2000); // 100 seconds at 20 ppm, across wrap
    }
}

static void test_freshness(void) {
    reset();
    g.first_sync_done = true;
    g.last_freq_sample_ms = test_ms - 32000;
    g.freq_jitter_floor = 400;
    ntp_peer_t *p = make_peer(0);
    sample(p, 0, 1000, 10000, test_ms-32000);
    sample(p, 1, 1400, 12000, test_ms);
    p->cycle_id_when_sent = p->last_settle_cycle_id = 10;
    assert(try_discipline(10));
    int32_t freq = g.freq_ppm_x1000;
    uint32_t syncs = g.sync_count;
    for (int cycle = 11; cycle < 15; cycle++) {
        test_ms += 33000;
        sample(p, cycle-9, p->best_offset_us+400, 12000, test_ms);
        p->cycle_id_when_sent = p->last_settle_cycle_id = cycle;
        assert(!try_discipline(cycle));
        assert(g.freq_ppm_x1000 == freq && g.sync_count == syncs);
    }
    sample(p, 6, 100, 9000, test_ms);
    p->cycle_id_when_sent = p->last_settle_cycle_id = 15;
    assert(try_discipline(15));
    assert(g.sync_count == syncs + 1);
}

static void test_expiry(void) {
    reset();
    ntp_peer_t *p = make_peer(0);
    sample(p, 0, 100, 1000, test_ms);
    select_system_peer();
    assert(g.selected_peer == 0);
    test_ms += ROBUST_SAMPLE_MAX_AGE_S * 1000 + 1;
    select_system_peer();
    assert(g.selected_peer == -1 && !p->best_sample_valid);
    // Expired low-delay samples must not enter either selection or jitter.
    sample(p, 1, 9000, 2000, test_ms);
    update_peer_filter(p);
    assert(p->best_offset_us == 9000 && p->jitter_us == 1000);
    // Timestamp zero is a valid observation epoch after tick wrap.
    test_ms = 0;
    memset(p->filter, 0, sizeof(p->filter));
    sample(p, 0, 100, 1000, 0);
    update_peer_filter(p);
    assert(p->best_sample_valid);
    assert(aged_peer_dispersion_us(p, 1000) == p->dispersion_us+PHI_US_PER_SEC);
}

static void test_congestion(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        reset();
        g.first_sync_done = true;
        ntp_peer_t *p = make_peer(0);
        for (int i = 0; i < 3; i++) sample(p, i, sign*4000, 10000, test_ms-32000*(i+1));
        struct timeval t4 = tv_from_us(wall_us + 20000);
        ntp_pkt_t pkt = packet(2, wall_us+10000+sign*4000, wall_us+10000+sign*4000);
        assert(process_response(p, &pkt, (uint8_t *)&pkt, sizeof(pkt), &t4) == RESP_GOOD);
        ntp_sample_t *s = &p->filter[p->filter_head];
        assert(abs(s->offset_us-sign*4000) <= 1);
        assert(s->dispersion_us >= 5000);
    }
}

static void test_intervals(void) {
    for (int synced = 0; synced < 2; synced++) {
        reset();
        g.first_sync_done = synced;
        ntp_peer_t *p = make_peer(0);
        int64_t before = wall_us;
        struct timeval t4 = tv_from_us(wall_us + 20000);
        ntp_pkt_t pkt = packet(2, wall_us + 10000, wall_us + 1010000);
        assert(process_response(p, &pkt, (uint8_t *)&pkt, sizeof(pkt), &t4) == RESP_BAD);
        assert(wall_us == before && step_count == 0);
        assert(!p->filter[p->filter_head].valid);
    }
    struct timeval t1 = tv_from_us(1000000), t2 = tv_from_us(1001000);
    struct timeval t3 = tv_from_us(1021300), t4 = tv_from_us(1020000);
    int64_t delay;
    assert(ntp_measurement_delay(&t1, &t2, &t3, &t4, &delay) && delay == 0);
    t3 = tv_from_us(1040000);
    assert(!ntp_measurement_delay(&t1, &t2, &t3, &t4, &delay));
    t3 = tv_from_us(1000000);
    assert(!ntp_measurement_delay(&t1, &t2, &t3, &t4, &delay));
}

static void test_server_validation(void) {
    for (int variant = 0; variant < 6; variant++) {
        reset();
        ntp_peer_t *p = make_peer(0);
        int64_t before = wall_us;
        struct timeval t4 = tv_from_us(wall_us + 20000);
        ntp_pkt_t pkt = packet(2, wall_us + 3600010000LL, wall_us + 3600010000LL);
        if (variant == 0) pkt.stratum = 16;
        if (variant == 1) pkt.stratum = 255;
        if (variant == 2) pkt.li_vn_mode |= 0xc0;
        if (variant == 3) pkt.root_dispersion = htonl(2u << 16);
        if (variant == 4) pkt.root_delay = htonl(4u << 16);
        int result = process_response(p, &pkt, (uint8_t *)&pkt, sizeof(pkt), &t4);
        if (variant < 5) {
            assert(result == RESP_BAD);
            assert(wall_us == before && step_count == 0 && !g.first_sync_done);
            assert(!p->filter[p->filter_head].valid);
        } else {
            assert(result == RESP_GOOD && g.first_sync_done && step_count == 1);
        }
    }
}

static void test_persistence(int32_t freq) {
    reset();
    g.first_sync_done = true;
    g.freq_loaded_from_nvs = true;
    g.freq_ppm_x1000 = freq;
    discipline_clock(0, false);
    assert(save_count == 1 && saved_freq == freq);
    test_ms += 1000;
    g.freq_ppm_x1000 += 100;
    discipline_clock(0, false);
    assert(save_count == 1);
    test_ms += 30 * 60 * 1000;
    discipline_clock(0, false);
    assert(save_count == 2 && saved_freq == freq + 100);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "persist-zero")) test_persistence(0);
    else if (!strcmp(argv[1], "persist-positive")) test_persistence(20000);
    else if (!strcmp(argv[1], "persist-negative")) test_persistence(-20000);
    else if (!strcmp(argv[1], "server-validation")) test_server_validation();
    else if (!strcmp(argv[1], "intervals")) test_intervals();
    else if (!strcmp(argv[1], "congestion")) test_congestion();
    else if (!strcmp(argv[1], "expiry")) test_expiry();
    else if (!strcmp(argv[1], "freshness")) test_freshness();
    else if (!strcmp(argv[1], "frequency-cadence")) test_frequency_cadence();
    else if (!strcmp(argv[1], "holdover")) test_holdover();
    else { fprintf(stderr, "Unknown case: %s\n", argv[1]); return 1; }
    printf("PASS %s\n", argv[1]);
    return 0;
}
