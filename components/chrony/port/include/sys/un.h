/*
 * Unix-domain socket address for chrony on ESP-IDF. lwIP has no AF_UNIX:
 * socket(AF_UNIX, ...) fails, which chrony reports as an unusable socket.
 * The definitions only let socket.c compile; its Unix-socket paths serve
 * the command socket and privileged helpers, neither of which is built.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_SYS_UN_H
#define CHRONY_ESP_SYS_UN_H

#include <sys/socket.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};

#endif
