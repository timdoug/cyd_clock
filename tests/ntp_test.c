#include "ntp_host.h"

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
    else { fprintf(stderr, "Unknown case: %s\n", argv[1]); return 1; }
    printf("PASS %s\n", argv[1]);
    return 0;
}
