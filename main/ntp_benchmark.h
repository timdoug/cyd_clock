#ifndef CYD_NTP_BENCHMARK_H
#define CYD_NTP_BENCHMARK_H

#include <stdbool.h>
#include <stdint.h>
#include "ntp.h"

typedef enum {
    NTP_BENCHMARK_OK,
    NTP_BENCHMARK_DNS_FAILED,
    NTP_BENCHMARK_NTS_FAILED,
    NTP_BENCHMARK_TIMEOUT,
    NTP_BENCHMARK_BAD_RESPONSE,
} ntp_benchmark_status_t;

typedef struct {
    ntp_benchmark_status_t status;
    int32_t delay_us;
    bool nts;
    char addr_str[46];
} ntp_benchmark_result_t;

ntp_benchmark_status_t ntp_benchmark_server(const char *host,
                                            bool prefer_ipv6,
                                            nts_mode_t nts_mode,
                                            ntp_benchmark_result_t *out);

#endif
