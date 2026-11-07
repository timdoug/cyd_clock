/*
 * lwIP's netdb.h, with getaddrinfo() redirected to the port's wrapper (see
 * chronyd_esp.c). lwIP answers an AF_UNSPEC query with one family only
 * (IPv4 when present), so chrony would never see IPv6 addresses of
 * dual-stack names. The wrapper queries both families, preferred first.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_NETDB_H
#define CHRONY_ESP_NETDB_H

#include_next <netdb.h>

int ces_getaddrinfo(const char *nodename, const char *servname,
                    const struct addrinfo *hints, struct addrinfo **res);
void ces_freeaddrinfo(struct addrinfo *ai);

#undef getaddrinfo
#define getaddrinfo(nodename, servname, hints, res) \
  ces_getaddrinfo(nodename, servname, hints, res)
#undef freeaddrinfo
#define freeaddrinfo(ai) ces_freeaddrinfo(ai)

#endif
