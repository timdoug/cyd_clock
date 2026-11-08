/*
 * Interface between the application and chronyd running on ESP-IDF.
 *
 * chronyd runs single-threaded in its own FreeRTOS task. The calls below
 * are safe from any other task: commands are queued to the chronyd task
 * and status is read from a snapshot the chronyd task refreshes once a
 * second. The snapshot carries chronyd's reports as chronyc receives them
 * (tracking, sources, authdata), and ces_fmt_*() print them as chronyc
 * does. No chrony header is exposed here, so application code keeps its
 * own header names (chrony's include tree has ntp.h, util.h, config.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONYD_ESP_H
#define CHRONYD_ESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* A "pool ... maxsources N" directive holds up to 2*N records until N are
   confirmed (ntp_sources.c), unresolved placeholders included. The
   application uses one pool of at most maxsources 4: 8 records. */
#define CES_MAX_SOURCES 8
#define CES_NAME_LEN    64
#define CES_ADDR_LEN    46

/* NTP_Leap values */
#define CES_LEAP_NORMAL           0
#define CES_LEAP_INSERT_SECOND    1
#define CES_LEAP_DELETE_SECOND    2
#define CES_LEAP_UNSYNCHRONISED   3

/* One entry of "chronyc sources" plus its "chronyc authdata" row */
typedef struct {
  uint32_t group;               /* id returned by ces_add_source() */
  char     name[CES_NAME_LEN];  /* name as configured (chronyc -N) */
  char     addr[CES_ADDR_LEN];  /* address (chronyc -n) */
  bool     resolved;            /* false for a pool record awaiting DNS */
  char     mode;                /* M column: '^' server, '=' peer, '#' refclock */
  char     state;               /* S column: * + - x ~ ? */
  int      stratum;
  int      poll;                /* log2 seconds */
  unsigned reach;               /* reachability register, shown in octal */
  uint32_t since_sample;        /* LastRx seconds; UINT32_MAX if none */
  double   latest_meas;         /* adjusted offset, seconds */
  double   orig_latest_meas;    /* measured offset, seconds */
  double   latest_meas_err;     /* estimated error, seconds */
  /* authdata */
  const char *auth_mode;        /* "-", "SK", "NTS" */
  uint32_t key_id;
  int      key_type;
  int      key_length;          /* bits */
  uint32_t last_ke_ago;         /* seconds; UINT32_MAX if never */
  int      ke_attempts;
  int      nak;
  int      cookies;
  int      cookie_length;
} ces_source_t;

/* "chronyc tracking" plus the sources */
typedef struct {
  bool         running;
  uint32_t     ref_id;
  char         ref_name[CES_NAME_LEN];
  int          stratum;
  struct timespec ref_time;
  double       current_correction;  /* System time; positive = clock slow */
  double       last_offset;
  double       rms_offset;
  double       freq_ppm;            /* positive = clock fast */
  double       resid_freq_ppm;
  double       skew_ppm;
  double       root_delay;
  double       root_dispersion;
  double       last_update_interval;
  int          leap_status;         /* CES_LEAP_* */
  int          n_sources;
  ces_source_t sources[CES_MAX_SOURCES];
} ces_status_t;

/* Start chronyd. Sources are added afterwards with ces_add_source().
   Returns false if the task could not be created. */
bool ces_start(void);

/* Add a source from a chrony "server"/"pool" directive, for example
   "pool pool.ntp.org iburst". The directive is parsed with chrony's own
   parser; an invalid one is rejected without side effects. Returns true
   and the group id on success. */
bool ces_add_source(const char *directive, uint32_t *group);

/* Remove every source added by the ces_add_source() call that returned
   this group id. */
void ces_remove_source(uint32_t group);

/* The network went down (false) or came up (true), as chronyc's "offline"
   and "onoffline" commands that NetworkManager's dispatcher sends on Linux:
   offline sources are not polled (so they are neither counted unreachable
   nor replaced), and sources coming back online poll at once, with a burst
   when configured with iburst. */
void ces_set_online(bool online);

/* Copy the latest snapshot. */
void ces_get_status(ces_status_t *out);

/* chrony release the firmware is built from, e.g. "4.9". */
const char *ces_version(void);

/* Name validation for ces_add_source() callers: true if the string is a
   plain host name or address (letters, digits, '.', '-', ':', '_'), so it
   cannot inject further directive options. */
bool ces_valid_host(const char *host);

/* chronyc output, transcribed from chrony's client.c. Values are
   formatted as chronyc prints them; only the padding between a label and
   its value is left to the caller. */

#define CES_TRACKING_LINES 13

/* "chronyc tracking": label (e.g. "Last offset") and value
   (e.g. "+0.000812345 seconds") per line. The Reference ID line shows
   the configured source name, as chronyc -N does. */
void ces_fmt_tracking(const ces_status_t *st,
                      char labels[CES_TRACKING_LINES][16],
                      char values[CES_TRACKING_LINES][64]);

/* Single tracking values, as in the lines above */
void ces_fmt_system_time(char *buf, size_t len, double correction);
void ces_fmt_frequency(char *buf, size_t len, double freq_ppm);
const char *ces_fmt_leap(int leap_status);

/* chronyc's interval (%I, e.g. "  34", " 12m"), signed offset with unit
   (%+S, e.g. " +812us") and unsigned offset with unit (%S) */
void ces_fmt_interval(char *buf, size_t len, uint32_t seconds);
void ces_fmt_signed_offset(char *buf, size_t len, double seconds);
void ces_fmt_offset(char *buf, size_t len, double seconds);

/* "Last sample" column of chronyc sources: " +812us[ +900us] +/-   21ms" */
void ces_fmt_last_sample(char *buf, size_t len, const ces_source_t *src);

#endif
