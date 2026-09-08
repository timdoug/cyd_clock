#include "ntp_host.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    bool vary_delay = strcmp(argv[1], "variable") == 0;
    bool noisy = strcmp(argv[1], "noisy") == 0;
    int drift = strcmp(argv[1], "slow") == 0 ? -20 : 20;
    bool adaptive = true;
    reset();
    const int64_t start_wall = wall_us;
    const uint32_t start_ms = test_ms;
    g.first_sync_done = true;
    ntp_peer_t *p = make_peer(0);
    double square = 0, max_error = 0;
    int measurements = 0;
    int next_sample = 32, cycle = 0;
    for (int second = 1; second <= 43200; second++) {
        // Simulated fast crystal: +20 ppm. Each queued slew is fully
        // completed before the next 1s sample (all slews here are small).
        wall_us += 1000000 + drift + pending_us;
        pending_us = 0;
        test_ms = start_ms + second * 1000;
        apply_freq_correction();
        if (second == next_sample) {
            cycle++;
            next_sample = second + g.current_poll_s;
            int32_t offset = (int32_t)(start_wall + (int64_t)second*1000000 - wall_us);
            int delay = vary_delay ? 10000 + (cycle % 8)*100 : 10000;
            int noise = noisy ? ((cycle * 137) % 601) - 300 : 0;
            sample(p, cycle % 8, offset + noise, delay, test_ms);
            p->cycle_id_when_sent = p->last_settle_cycle_id = cycle;
            p->next_poll_cycle_id = cycle+1;
            p->reach = 255;
            update_peer_filter(p);
            select_system_peer();
            try_discipline(cycle);
            // Hold poll at 32 seconds to isolate sample handling and PLL.
            if (!adaptive) {
                g.current_poll_s = 32;
                g.last_discipline_poll_s = 32;
            }
        }
        if (second >= 39600) {
            double error = (double)(wall_us-start_wall-(int64_t)second*1000000);
            square += error*error;
            if (fabs(error) > max_error) max_error = fabs(error);
            measurements++;
        }
    }
    printf("12h crystal, %s symmetric RTT: learned_freq_ppb=%d last_hour_rms_us=%.1f max_us=%.1f jitter_us=%d poll_s=%u\n",
        argv[1], g.freq_ppm_x1000,
        sqrt(square/measurements), max_error, g.system_jitter_us, g.current_poll_s);
    assert(abs(g.freq_ppm_x1000 + drift*1000) < 500);
    assert(sqrt(square/measurements) < 500);
    assert(max_error < 1500);
    return 0;
}
