/*
 * res_init() stub for chrony on ESP-IDF: lwIP's resolver reads the DNS
 * servers from the netif on every query, so there is no state to reload.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_RESOLV_H
#define CHRONY_ESP_RESOLV_H

static inline int res_init(void)
{
  return 0;
}

#endif
