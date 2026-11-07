/*
 * chrony build configuration for ESP-IDF (ESP32). Hand-maintained in place
 * of the output of chrony's ./configure, which targets Unix hosts.
 * Features match "--disable-cmdmon --disable-refclock --disable-rtc
 * --disable-privdrop --disable-timestamping --disable-sechash" with NTS
 * enabled on the mbedTLS port (port/tls_mbedtls.c, port/siv_mbedtls.c).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_CONFIG_H
#define CHRONY_ESP_CONFIG_H

#define ESP_IDF 1
#define DEBUG 0
#define HAVE_LONG_TIME_T 1
/* Wire timestamps are interpreted within [split, split + 2^32 s). The split
   sits 50 years before 2026-11-07, so dates resolve correctly until 2112. */
#define NTP_ERA_SPLIT (1794065463LL - 18250 * 24 * 3600)
#define FEAT_IPV6 1
#define USE_PTHREAD_ASYNCDNS 1
#define FORCE_DNSRETRY 1
/* ESP-IDF's getrandom() reads the hardware RNG (esp_fill_random) and never
   blocks. Without it, chrony falls back to /dev/urandom, which does not
   exist here, and the first random read is fatal. */
#define HAVE_GETRANDOM 1
#define FEAT_NTS 1
#define HAVE_SIV 1

#define DEFAULT_CONF_FILE ""
#define DEFAULT_HWCLOCK_FILE ""
#define DEFAULT_PID_FILE ""
#define DEFAULT_RTC_DEVICE ""
#define DEFAULT_USER "root"
#define DEFAULT_CHRONYC_USER "root"
#define DEFAULT_COMMAND_SOCKET ""
#define MAIL_PROGRAM ""
#define CHRONYC_FEATURES "-READLINE -SECHASH +IPV6 -DEBUG"
#define CHRONYD_FEATURES "-CMDMON -REFCLOCK -RTC -PRIVDROP -SCFILTER -SIGND +NTS -SECHASH +IPV6 -DEBUG"
#define CHRONY_VERSION "4.9"

#include "esp_compat.h"

#endif
