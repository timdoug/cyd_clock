#ifndef CYD_UTIL_H
#define CYD_UTIL_H

#include <string.h>

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

#endif // CYD_UTIL_H
