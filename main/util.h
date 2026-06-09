#ifndef CYD_UTIL_H
#define CYD_UTIL_H

#include <string.h>

// Bounded string copy that always NUL-terminates (strncpy alone leaves the
// destination unterminated when src fills it).
static inline void str_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

#endif // CYD_UTIL_H
