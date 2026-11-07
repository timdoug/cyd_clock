#ifndef CYD_NTP_H
#define CYD_NTP_H

// Time synchronization is chronyd (components/chrony). This layer turns the
// settings into chrony's configuration: one "pool <server> iburst" source,
// as in chrony's example configuration, with chrony's "nts" option and
// "ipv4"/"ipv6" family options. Status comes from chronyd_esp.h.

#include <stdbool.h>

typedef enum {
    NTP_FAMILY_ANY  = 0,  // no family option: addresses of both families
    NTP_FAMILY_IPV4 = 1,  // chrony source option "ipv4"
    NTP_FAMILY_IPV6 = 2,  // chrony source option "ipv6"
} ntp_family_t;

void ntp_init(const char *server, ntp_family_t family, bool nts);

void ntp_set_server(const char *server);
void ntp_set_family(ntp_family_t family);
void ntp_set_nts(bool nts);

#endif
