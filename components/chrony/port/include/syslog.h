/*
 * syslog stub for chrony on ESP-IDF. chrony's log output goes to stderr
 * (the console UART) because LOG_OpenSystemLog() is never called; these
 * definitions only satisfy logging.c at compile time.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_SYSLOG_H
#define CHRONY_ESP_SYSLOG_H

#define LOG_PID     0x01
#define LOG_DAEMON  (3 << 3)
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_INFO    6
#define LOG_DEBUG   7

static inline void openlog(const char *ident, int option, int facility)
{
  (void)ident;
  (void)option;
  (void)facility;
}

static inline void closelog(void)
{
}

static inline void syslog(int priority, const char *format, ...)
{
  (void)priority;
  (void)format;
}

#endif
