/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Lonnie Abelbeck  2016, 2018
 * Copyright (C) Thomas Kupper  2026
 * Copyright (C) Miroslav Lichvar  2009-2026
 * Copyright (C) Domaine Nyquist, A Limited Liability Company  2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 **********************************************************************

  =======================================================================

  chronyc's report formatting (client.c: print_seconds(),
  print_nanoseconds(), print_signed_nanoseconds(), print_report() and the
  tracking/sources formats), writing to buffers for the display instead
  of stdout. client.c cannot be linked: it is chronyc's whole program.

  */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "chronyd_esp.h"

/* ================================================== */

/* print_seconds() */
void
ces_fmt_interval(char *buf, size_t len, uint32_t s)
{
  uint32_t d;

  if (s == (uint32_t)-1) {
    snprintf(buf, len, "   -");
  } else if (s < 1200) {
    snprintf(buf, len, "%4"PRIu32, s);
  } else if (s < 36000) {
    snprintf(buf, len, "%3"PRIu32"m", s / 60);
  } else if (s < 345600) {
    snprintf(buf, len, "%3"PRIu32"h", s / 3600);
  } else {
    d = s / 86400;
    if (d > 999) {
      snprintf(buf, len, "%3"PRIu32"y", d / 365);
    } else {
      snprintf(buf, len, "%3"PRIu32"d", d);
    }
  }
}

/* ================================================== */

/* print_nanoseconds() */
void
ces_fmt_offset(char *buf, size_t len, double s)
{
  s = fabs(s);

  if (s < 9999.5e-9) {
    snprintf(buf, len, "%4.0fns", s * 1e9);
  } else if (s < 9999.5e-6) {
    snprintf(buf, len, "%4.0fus", s * 1e6);
  } else if (s < 9999.5e-3) {
    snprintf(buf, len, "%4.0fms", s * 1e3);
  } else if (s < 999.5) {
    snprintf(buf, len, "%5.1fs", s);
  } else if (s < 99999.5) {
    snprintf(buf, len, "%5.0fs", s);
  } else if (s < 99999.5 * 60) {
    snprintf(buf, len, "%5.0fm", s / 60);
  } else if (s < 99999.5 * 3600) {
    snprintf(buf, len, "%5.0fh", s / 3600);
  } else if (s < 99999.5 * 3600 * 24) {
    snprintf(buf, len, "%5.0fd", s / (3600 * 24));
  } else {
    snprintf(buf, len, "%5.0fy", s / (3600 * 24 * 365));
  }
}

/* ================================================== */

/* print_signed_nanoseconds() */
void
ces_fmt_signed_offset(char *buf, size_t len, double s)
{
  double x;

  x = fabs(s);

  if (x < 9999.5e-9) {
    snprintf(buf, len, "%+5.0fns", s * 1e9);
  } else if (x < 9999.5e-6) {
    snprintf(buf, len, "%+5.0fus", s * 1e6);
  } else if (x < 9999.5e-3) {
    snprintf(buf, len, "%+5.0fms", s * 1e3);
  } else if (x < 999.5) {
    snprintf(buf, len, "%+6.1fs", s);
  } else if (x < 99999.5) {
    snprintf(buf, len, "%+6.0fs", s);
  } else if (x < 99999.5 * 60) {
    snprintf(buf, len, "%+6.0fm", s / 60);
  } else if (x < 99999.5 * 3600) {
    snprintf(buf, len, "%+6.0fh", s / 3600);
  } else if (x < 99999.5 * 3600 * 24) {
    snprintf(buf, len, "%+6.0fd", s / (3600 * 24));
  } else {
    snprintf(buf, len, "%+6.0fy", s / (3600 * 24 * 365));
  }
}

/* ================================================== */

/* "%+S[%+S] +/- %S" of process_cmd_sources() */
void
ces_fmt_last_sample(char *buf, size_t len, const ces_source_t *src)
{
  char adj[16], orig[16], err[16];

  ces_fmt_signed_offset(adj, sizeof (adj), src->latest_meas);
  ces_fmt_signed_offset(orig, sizeof (orig), src->orig_latest_meas);
  ces_fmt_offset(err, sizeof (err), src->latest_meas_err);
  snprintf(buf, len, "%s[%s] +/- %s", adj, orig, err);
}

/* ================================================== */

/* %L */
const char *
ces_fmt_leap(int leap_status)
{
  switch (leap_status) {
    case CES_LEAP_NORMAL:
      return "Normal";
    case CES_LEAP_INSERT_SECOND:
      return "Insert second";
    case CES_LEAP_DELETE_SECOND:
      return "Delete second";
    case CES_LEAP_UNSYNCHRONISED:
      return "Not synchronised";
    default:
      return "Invalid";
  }
}

/* ================================================== */

/* "%.9O of NTP time": positive correction means the clock is slow */
void
ces_fmt_system_time(char *buf, size_t len, double correction)
{
  snprintf(buf, len, "%.9f seconds %s of NTP time", fabs(correction),
           correction > 0.0 ? "slow" : "fast");
}

/* ================================================== */

/* "%.3F": positive frequency means the clock is fast */
void
ces_fmt_frequency(char *buf, size_t len, double freq_ppm)
{
  snprintf(buf, len, "%.3f ppm %s", fabs(freq_ppm),
           freq_ppm > 0.0 ? "fast" : "slow");
}

/* ================================================== */

/* process_cmd_tracking() */
void
ces_fmt_tracking(const ces_status_t *st,
                 char labels[CES_TRACKING_LINES][16],
                 char values[CES_TRACKING_LINES][64])
{
  static const char *const names[CES_TRACKING_LINES] = {
    "Reference ID", "Stratum", "Ref time (UTC)", "System time",
    "Last offset", "RMS offset", "Frequency", "Residual freq", "Skew",
    "Root delay", "Root dispersion", "Update interval", "Leap status",
  };
  struct tm tm;
  int i;

  for (i = 0; i < CES_TRACKING_LINES; i++)
    snprintf(labels[i], 16, "%s", names[i]);

  snprintf(values[0], 64, "%08"PRIX32" (%.52s)", st->ref_id, st->ref_name);
  snprintf(values[1], 64, "%d", st->stratum);
  if (gmtime_r(&st->ref_time.tv_sec, &tm))
    strftime(values[2], 64, "%a %b %d %T %Y", &tm);
  else
    values[2][0] = '\0';
  ces_fmt_system_time(values[3], 64, st->current_correction);
  snprintf(values[4], 64, "%+.9f seconds", st->last_offset);
  snprintf(values[5], 64, "%.9f seconds", st->rms_offset);
  ces_fmt_frequency(values[6], 64, st->freq_ppm);
  snprintf(values[7], 64, "%+.3f ppm", st->resid_freq_ppm);
  snprintf(values[8], 64, "%.3f ppm", st->skew_ppm);
  snprintf(values[9], 64, "%.9f seconds", st->root_delay);
  snprintf(values[10], 64, "%.9f seconds", st->root_dispersion);
  snprintf(values[11], 64, "%.1f seconds", st->last_update_interval);
  snprintf(values[12], 64, "%s", ces_fmt_leap(st->leap_status));
}
