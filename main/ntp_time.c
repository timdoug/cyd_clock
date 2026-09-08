#include "ntp_internal.h"

uint32_t clock_step_generation;

bool ntp_server_usable(const ntp_pkt_t *pkt) {
    uint8_t li = pkt->li_vn_mode >> 6;
    int64_t distance = (int64_t)fp1616_to_us(ntohl(pkt->root_delay)) / 2 +
                       fp1616_to_us(ntohl(pkt->root_dispersion));
    return li != 3 && pkt->stratum >= 1 && pkt->stratum <= 15 &&
           distance <= MAX_ROOT_DIST_US;
}

bool ntp_measurement_delay(const struct timeval *t1, const struct timeval *t2,
                           const struct timeval *t3, const struct timeval *t4,
                           int64_t *delay_us) {
    int64_t elapsed = tv_diff_us(t4, t1);
    int64_t server = tv_diff_us(t3, t2);
    if (elapsed < 0 || server < 0) return false;
    int64_t delay = elapsed - server;
    // IDF's fastest phase slew is 1/64. Allow that rate error across the
    // measurement plus 200 us of capture/rounding uncertainty, but never
    // turn a grossly impossible interval into a winning zero-delay sample.
    int64_t tolerance = elapsed / 63 + 2 * SAMPLE_DISP_FLOOR_US;
    if (delay < -tolerance) return false;
    *delay_us = delay < 0 ? 0 : delay;
    return true;
}

void ntp_to_tv(uint32_t sec, uint32_t frac, struct timeval *tv) {
    // Map the wire value to Unix seconds mod 2^32 (the add wraps by design;
    // see NTP_UNIX_FOLD), then resolve the 136-year era ambiguity against
    // the anchor year (util.h): the device cannot legitimately observe a
    // time before it, so the unique 64-bit value congruent to `folded`
    // within [anchor_epoch, anchor_epoch + 2^32) is the right
    // interpretation - in any era, forever, as long as UTIL_ANCHOR_YEAR is
    // bumped at least once per ~136 years. (ntpd and chrony anchor the
    // same ambiguity on "current system time +/- 68 years"; we can't,
    // because a battery-less cold boot starts at 1970.)
    //
    // The fold is an EXPLICIT uint32 add rather than a per-era if/else the
    // optimizer must merge: GCC 15.2 at -O2 was observed folding the branchy
    // form while keeping the 64-bit carry of the unified add, projecting
    // current-era timestamps 2^32 seconds into the future.
    uint32_t folded = sec + NTP_UNIX_FOLD;
    uint64_t epoch  = (uint64_t)util_anchor_epoch();
    uint64_t t      = (epoch & ~(uint64_t)0xFFFFFFFF) | folded;
    if (t < epoch) t += (1ULL << 32);
    tv->tv_sec  = (time_t)t;
    tv->tv_usec = (suseconds_t)(((uint64_t)frac * 1000000ULL) >> 32);
}

void step_clock(int64_t step_us) {
    ntp_wifi_stamp_set_step_in_progress(true);
    for (int i = 0; i < NTP_MAX_PEERS; i++) {
        ntp_peer_t *q = &g.peers[i];
        if (!q->request_outstanding) continue;
        q->t1 = tv_from_us(tv_to_us(&q->t1) + step_us);
    }
    shift_early(early_t1_ring(), step_us);
    shift_early(early_t4_ring(), step_us);

    struct timeval now_pre;
    gettimeofday(&now_pre, NULL);
    struct timeval target = tv_from_us(tv_to_us(&now_pre) + step_us);
    settimeofday(&target, NULL);
    clock_step_generation++;
    if (clock_step_generation == 0) clock_step_generation = 1;
    ntp_wifi_stamp_set_step_in_progress(false);
}
