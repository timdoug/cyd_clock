#ifndef CYD_NTP_H
#define CYD_NTP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NTP_MAX_PEERS 4

// Per-peer statistics (snapshot)
typedef struct {
    bool     active;           // Peer slot in use
    bool     selected;         // Chosen as system peer
    char     addr_str[46];     // Printable address
    uint8_t  stratum;          // 0 = unreachable / KoD
    uint8_t  reach;            // 8-bit reach register (MSB = most recent)
    int32_t  offset_us;        // Best-sample offset in microseconds
    int32_t  delay_us;         // Best-sample round-trip delay
    int32_t  jitter_us;        // RMS deviation across recent samples
    int32_t  dispersion_us;    // Root dispersion estimate
    uint32_t last_response_ms; // ms since last valid response (UINT32_MAX if never)
} ntp_peer_stats_t;

// System-wide NTP statistics
typedef struct {
    bool     synced;
    time_t   last_sync_time;
    uint32_t sync_count;
    uint32_t current_poll_s;   // Currently active adaptive poll interval
    uint32_t sync_elapsed_ms;  // ms since sync attempt started (pre-first-sync)
    int64_t  last_offset_us;   // Offset applied at last discipline step
    int32_t  system_jitter_us;
    int32_t  root_delay_us;
    int32_t  root_dispersion_us;
    int32_t  freq_ppm_x1000;   // Estimated crystal freq error, ppm * 1000 (for 1 ppb resolution)
    uint8_t  stratum;          // Our stratum (selected peer's + 1, or 16 if unsynced)
    uint8_t  selected_peer;    // Index into peer table, 0xFF if none
    const char *server;        // Configured hostname
} ntp_sys_stats_t;

// Lifecycle
void ntp_init(const char *server, bool prefer_ipv6);
void ntp_stop(void);

// Configuration (takes effect on next poll cycle; triggers a re-sync internally)
void ntp_set_server(const char *server);
void ntp_set_prefer_ipv6(bool prefer);

// Stats
void ntp_get_sys_stats(ntp_sys_stats_t *out);
bool ntp_get_peer_stats(int idx, ntp_peer_stats_t *out);
void ntp_get_primary_addr_str(char *buf, size_t len);

#endif // CYD_NTP_H
