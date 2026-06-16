#include "ntp_internal.h"


int32_t precision_to_us(int8_t precision) {
    if (precision >= 0) {
        if (precision >= 12) return INT32_MAX;
        int64_t us = 1000000LL << precision;
        return us > INT32_MAX ? INT32_MAX : (int32_t)us;
    }
    int shift = -(int)precision;
    if (shift >= 20) return 1;
    return 1000000 >> shift;
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


int64_t tv_diff_us(const struct timeval *a, const struct timeval *b) {
    return ((int64_t)a->tv_sec - b->tv_sec) * 1000000LL + (a->tv_usec - b->tv_usec);
}


struct timeval tv_from_us(int64_t total_us) {
    struct timeval tv = {
        .tv_sec  = (time_t)(total_us / 1000000),
        .tv_usec = (suseconds_t)(total_us % 1000000),
    };
    if (tv.tv_usec < 0) {
        tv.tv_sec--;
        tv.tv_usec += 1000000;
    }
    return tv;
}


int64_t tv_to_us(const struct timeval *tv) {
    return (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
}


int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


int32_t fp1616_to_us(uint32_t raw) {
    uint64_t us = ((uint64_t)raw * 1000000ULL) >> 16;
    if (us > 0x7FFFFFFFULL) us = 0x7FFFFFFFULL;
    return (int32_t)us;
}

void step_clock(int64_t step_us) {
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
}
