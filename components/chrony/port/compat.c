/*
 * Definitions for the declarations in include/esp_compat.h.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "config.h"

#include <fcntl.h>
#include <stdarg.h>

const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

#undef fcntl

int
ces_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  int arg;

  switch (cmd) {
    case F_GETFD:
      return FD_CLOEXEC;
    case F_SETFD:
      return 0;
    case F_GETFL:
      return fcntl(fd, cmd);
    default:
      va_start(ap, cmd);
      arg = va_arg(ap, int);
      va_end(ap);
      return fcntl(fd, cmd, arg);
  }
}
