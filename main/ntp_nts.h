#ifndef CYD_NTP_NTS_H
#define CYD_NTP_NTS_H

// NTS (Network Time Security, RFC 8915) for the NTP client: the TLS 1.3 key
// establishment (NTS-KE) handshake plus the RFC 7822 extension-field codec
// that carries authentication on the regular NTP/UDP exchange.
//
// One KE handshake to a host yields a shared key pair (c2s/s2c) and a pool of
// single-use cookies. Cookies are not bound to a server address, so all of a
// host's resolved peers draw from one shared context. Each request consumes a
// cookie and (via cookie placeholders) the response refills the pool.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTS_KEY_LEN      32   // AES-SIV-CMAC-256 key length (== NTP_SIV_KEY_LEN)
#define NTS_UID_LEN      32   // Unique Identifier extension-field nonce length
#define NTS_MAX_COOKIES  8    // RFC 8915 recommends keeping ~8 cookies on hand
#define NTS_COOKIE_MAX   256  // generous cap; real cookies are ~100-200 bytes
#define NTS_KE_PORT      4460

typedef struct {
    bool     valid;          // KE succeeded; peers for this host run authenticated
    bool     ke_in_flight;   // a KE task is currently running
    bool     ke_failed;
    uint32_t ke_retry_at_ms;
    uint32_t ke_generation;  // rejects stale task completions after config churn
    uint8_t  c2s[NTS_KEY_LEN];
    uint8_t  s2c[NTS_KEY_LEN];
    uint8_t  cookie[NTS_MAX_COOKIES][NTS_COOKIE_MAX];
    uint16_t cookie_len[NTS_MAX_COOKIES];
    int      cookie_count;
    char     ntp_host[64];    // KE-negotiated NTP server (default = the KE host)
    uint16_t ntp_port;        // KE-negotiated NTP port (default 123)
} ntp_nts_ctx_t;

// Run NTS-KE against host:4460 (TLS 1.3, ALPN "ntske/1", cert verified against
// the bundle) and fill *out. Blocking and stack-hungry (TLS 1.3 handshake) -
// MUST run in a dedicated large-stack task, not the NTP task. On success *out
// has valid=true, keys, >=1 cookie, and the negotiated host/port. Returns false
// on any failure (no TLS 1.3, refused, NTS not offered, error record, ...).
bool ntp_nts_ke_run(const char *host, ntp_nts_ctx_t *out);

// Append the NTS client extension fields (Unique Identifier, NTS Cookie, Cookie
// Placeholders to refill the pool, and the Authenticator) to a request packet
// that already holds the 48-byte NTP header at offset 0. Consumes one cookie
// from ctx and writes the sent Unique ID to uid_out for response matching.
// *len is the current packet length in/out; cap is the buffer size. Returns
// false if no cookie is available or the buffer is too small.
bool ntp_nts_add_ef(uint8_t *buf, size_t *len, size_t cap,
                    ntp_nts_ctx_t *ctx, uint8_t uid_out[NTS_UID_LEN]);

// Verify a received NTS response (whole datagram in pkt/pkt_len): check the
// echoed Unique ID against uid, verify the Authenticator with s2c, and harvest
// any fresh cookies from the encrypted extension fields into ctx's pool.
// Returns true only if authentication passes.
bool ntp_nts_check_response(const uint8_t *pkt, size_t pkt_len,
                            ntp_nts_ctx_t *ctx, const uint8_t uid[NTS_UID_LEN]);

// Check only the cleartext Unique Identifier extension field. This is used for
// NTS NAK KoDs, which RFC 8915 sends without the Authenticator EF.
bool ntp_nts_response_uid_matches(const uint8_t *pkt, size_t pkt_len,
                                  const uint8_t uid[NTS_UID_LEN]);

// Local parser/AEAD regression checks for NTS extension-field handling.
bool ntp_nts_selftest(void);

#endif
