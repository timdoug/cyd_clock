#ifndef CYD_UTIL_H
#define CYD_UTIL_H

#include <stdint.h>
#include <string.h>

// Era anchor year: "a year the device cannot legitimately observe a time
// before". Anchors NTP era resolution (ntp.c) and the display's
// time-validity check (ui_clock.c). Hardcoded rather than parsed from
// __DATE__ so builds are reproducible; each value is good for ~136 years
// (one NTP era past the anchor), so bump it whenever you happen to be
// editing nearby in some future century.
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
