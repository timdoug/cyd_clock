#include "ntp_host.h"

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
    else { fprintf(stderr, "Unknown case: %s\n", argv[1]); return 1; }
    printf("PASS %s\n", argv[1]);
    return 0;
}
