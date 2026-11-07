/*
 * POSIX pieces that chrony references and ESP-IDF's newlib/lwIP lack.
 * Included at the end of config.h, ahead of chrony's sysincl.h. Each stub
 * returns the failure chrony already handles for the feature being absent;
 * none of these paths is reached with this port's configuration.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_COMPAT_H
#define CHRONY_ESP_COMPAT_H

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* File-descriptor passing over Unix sockets (cmdmon/privops helpers). */
#ifndef SCM_RIGHTS
#define SCM_RIGHTS 1
#endif

/* ESP-IDF's sys/random.h declares getrandom() but no flags; its
   getrandom() ignores them and never blocks. */
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x01
#endif

/* lwIP's fcntl() supports only F_GETFL/F_SETFL, so chrony's
   close-on-exec setup (F_GETFD/F_SETFD in UTI_FdSetCloexec(), whose failure
   makes socket.c discard every new socket) fails. The wrapper in compat.c
   answers the descriptor-flag commands: with no exec(), close-on-exec holds
   trivially. The macro precedes <fcntl.h>, so it renames the declaration
   there as well. */
#define fcntl ces_fcntl
int ces_fcntl(int fd, int cmd, ...);

/* lwIP declares in6addr_any but not the loopback constant. */
extern const struct in6_addr in6addr_loopback;

/* "mailonchange" directive: no mailer on this device. */
static inline FILE *popen(const char *command, const char *mode)
{
  (void)command;
  (void)mode;
  errno = ENOSYS;
  return NULL;
}

static inline int pclose(FILE *stream)
{
  (void)stream;
  return -1;
}

/* Unix socket pairs (privops helper, async NTS-KE server helper). */
static inline int socketpair(int domain, int type, int protocol, int sv[2])
{
  (void)domain;
  (void)type;
  (void)protocol;
  (void)sv;
  errno = ENOSYS;
  return -1;
}

#endif
