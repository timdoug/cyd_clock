#ifndef CYD_NTP_H
#define CYD_NTP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NTP_MAX_PEERS 4

typedef struct {
    bool     active;
    bool     selected;
    char     addr_str[46];
    uint8_t  stratum;
    uint8_t  reach;
    int32_t  offset_us;
    int32_t  delay_us;
    int32_t  jitter_us;
    int32_t  dispersion_us;
    uint32_t last_response_ms;
    bool     fresh;
    bool     nts;       // this peer authenticates exchanges via NTS (RFC 8915)
} ntp_peer_stats_t;

typedef struct {
    bool     synced;
    time_t   last_sync_time;
    uint32_t sync_count;
    uint32_t current_poll_s;
    uint32_t sync_elapsed_ms;
    int64_t  last_offset_us;
    int32_t  system_jitter_us;
    int32_t  root_delay_us;
    int32_t  root_dispersion_us;
    int32_t  freq_ppm_x1000;
    bool     freq_known;
    uint8_t  stratum;
    uint8_t  selected_peer;
    bool     nts_active;   // an NTS context is established for the current server
    char     server[64];
} ntp_sys_stats_t;

typedef enum {
    NTS_MODE_OFF           = 0, // never attempt NTS; plain NTP only
    NTS_MODE_OPPORTUNISTIC = 1, // authenticate when the server offers NTS, else plain
    NTS_MODE_REQUIRE       = 2, // only accept NTS-authenticated peers; never plain
} nts_mode_t;

void ntp_init(const char *server, bool prefer_ipv6, nts_mode_t nts_mode);
void ntp_stop(void);

// Install a WiFi internal RX callback that stamps NTP responses at the
// WiFi-task layer (before the lwIP/scheduler latency that would otherwise
// inflate t4). Call once after esp_wifi_start(). Safe to call before any
// connection - the cb just isn't exercised until packets arrive.
void ntp_install_wifi_rx_hook(void);

void ntp_set_server(const char *server);
void ntp_set_prefer_ipv6(bool prefer);
void ntp_set_nts_mode(nts_mode_t mode);

void ntp_get_sys_stats(ntp_sys_stats_t *out);
bool ntp_get_peer_stats(int idx, ntp_peer_stats_t *out);
// One-lock snapshot of system + all peer stats (inactive slots have
// active == false): the once-a-second UI paths previously took the NTP
// lock five times per refresh, and a snapshot is also internally
// consistent rather than five reads of a moving target.
void ntp_get_all_stats(ntp_sys_stats_t *sys, ntp_peer_stats_t peers[NTP_MAX_PEERS]);
void ntp_get_primary_addr_str(char *buf, size_t len);

#endif
