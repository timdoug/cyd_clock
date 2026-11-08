#ifndef CYD_UTIL_H
#define CYD_UTIL_H

#include <stdint.h>
#include <string.h>

// Anchor year: "a year the device cannot legitimately observe a time
// before". A clock earlier than this has not been set yet (it starts in
// 1970 without a battery): the clock screen shows no time and OTA does not
// start (certificate dates need the clock). Hardcoded rather than parsed
// from __DATE__ so builds are reproducible. NTP era resolution is chronyd's
// (NTP_ERA_SPLIT in components/chrony/port/include/config.h).
#define UTIL_ANCHOR_YEAR 2026

// Unix seconds at 00:00 UTC on January 1 of UTIL_ANCHOR_YEAR.
static inline int64_t util_anchor_epoch(void) {
    // Days from 1970-01-01 to Jan 1 of the anchor year (Gregorian rules).
    int64_t days = 365 * (UTIL_ANCHOR_YEAR - 1970)
                 + (UTIL_ANCHOR_YEAR - 1969) / 4
                 - (UTIL_ANCHOR_YEAR - 1901) / 100
                 + (UTIL_ANCHOR_YEAR - 1601) / 400;
    return days * 86400;
}

// Bounded string copy that always NUL-terminates. strlen+memcpy rather than
// strncpy/strnlen: truncation is this function's documented purpose, so
// GCC's -Wstringop-truncation (an error under -O2 -Werror) doesn't apply,
// -Wstringop-overread can't trip on a bound larger than the source object,
// and we skip strncpy's zero-padding of the rest of the buffer.
static inline void str_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n > dst_size - 1) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#endif
