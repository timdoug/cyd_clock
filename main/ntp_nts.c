#include "ntp_nts.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "ntp_siv.h"
#include "util.h"

static const char *TAG = "nts";

#define EF_UNIQUE_ID          0x0104
#define EF_COOKIE             0x0204
#define EF_COOKIE_PLACEHOLDER 0x0304
#define EF_AUTHENTICATOR      0x0404

#define NTS_NONCE_LEN 16

static bool ef_len_ok(uint16_t eflen, size_t off, size_t pkt_len) {
    return eflen >= 16 && (eflen & 3u) == 0 && off + eflen <= pkt_len;
}

// NTS-KE request: Next Protocol Negotiation = NTPv4(0), AEAD Negotiation =
// AES-SIV-CMAC-256(15), End of Message. The high bit of each record type is the
// Critical flag (RFC 8915 section 4).
static const uint8_t KE_REQUEST[] = {
    0x80, 0x01, 0x00, 0x02, 0x00, 0x00,   // Next Protocol Negotiation (crit), NTPv4
    0x80, 0x04, 0x00, 0x02, 0x00, 0x0f,   // AEAD Negotiation (crit), AES-SIV-CMAC-256
    0x80, 0x00, 0x00, 0x00,               // End of Message (crit)
};

// RFC 7822 extension fields are padded to a 4-byte boundary and at least 16
// bytes long. val == NULL writes a zero-filled placeholder body.
static bool ef_append(uint8_t *buf, size_t *len, size_t cap,
                      uint16_t type, const uint8_t *val, size_t val_len) {
    size_t total  = 4 + val_len;
    size_t padded = (total + 3) & ~(size_t)3;
    if (padded < 16) padded = 16;
    if (*len + padded > cap) return false;
    uint8_t *p = buf + *len;
    p[0] = (uint8_t)(type >> 8); p[1] = (uint8_t)type;
    p[2] = (uint8_t)(padded >> 8); p[3] = (uint8_t)padded;
    if (val && val_len) memcpy(p + 4, val, val_len);
    else if (val_len)   memset(p + 4, 0, val_len);
    memset(p + 4 + val_len, 0, padded - 4 - val_len);
    *len += padded;
    return true;
}

static int ke_parse(const uint8_t *buf, size_t len, ntp_nts_ctx_t *out) {
    size_t off = 0;
    bool proto_ok = false, aead_ok = false;
    int cookies = 0;
    while (off + 4 <= len) {
        uint16_t rt   = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        uint16_t blen = (uint16_t)((buf[off + 2] << 8) | buf[off + 3]);
        uint16_t type = rt & 0x7fff;
        if (off + 4 + blen > len) return 0;
        const uint8_t *body = buf + off + 4;
        switch (type) {
        case 0:
            // End of Message (RFC 8915 4.1.1): Critical Bit set, empty body.
            if ((rt & 0x8000) == 0 || blen != 0) return -1;
            if (!proto_ok || !aead_ok || cookies == 0) return -1;
            out->cookie_count = cookies;
            return 1;
        case 1:
            // Next Protocol Negotiation (RFC 8915 4.1.2) MUST be critical.
            if ((rt & 0x8000) == 0) return -1;
            for (int i = 0; i + 1 < blen; i += 2)
                if (((body[i] << 8) | body[i + 1]) == 0) proto_ok = true;
            break;
        case 2:
            ESP_LOGW(TAG, "KE error record code %u",
                     blen >= 2 ? (unsigned)((body[0] << 8) | body[1]) : 0);
            return -1;
        case 3:
            // Warning (RFC 8915 4.1.4): we recognize no warning codes, and an
            // unrecognized warning MUST be treated as fatal.
            ESP_LOGW(TAG, "KE warning record code %u",
                     blen >= 2 ? (unsigned)((body[0] << 8) | body[1]) : 0);
            return -1;
        case 4:
            for (int i = 0; i + 1 < blen; i += 2)
                if (((body[i] << 8) | body[i + 1]) == 15) aead_ok = true;
            break;
        case 5:
            if (blen > 0 && blen <= NTS_COOKIE_MAX && cookies < NTS_MAX_COOKIES) {
                memcpy(out->cookie[cookies], body, blen);
                out->cookie_len[cookies] = blen;
                cookies++;
            }
            break;
        case 6:
            // Negotiated NTP server name (RFC 8915 4.1.7). An over-long name
            // cannot be stored; silently keeping the KE host risks cookie/host
            // mismatch NAKs, so fail the handshake instead. Empty stays lenient.
            if (blen >= sizeof(out->ntp_host)) return -1;
            if (blen > 0) {
                memcpy(out->ntp_host, body, blen);
                out->ntp_host[blen] = '\0';
            }
            break;
        case 7:
            if (blen == 2) out->ntp_port = (uint16_t)((body[0] << 8) | body[1]);
            break;
        default:
            if (rt & 0x8000) return -1;
            break;
        }
        off += 4 + blen;
    }
    return 0;
}

// Pinned KE server leaf certificate. Some NTS-KE servers (Cloudflare)
// close connections about 2 s after accept; a full ESP32 handshake
// against an ECDSA chain cannot reliably fit (a single software P-384
// verify measures ~1.3 s). The handshake itself still completes and
// verifies the chain against the certificate bundle - only the request
// afterwards is lost - so remember the freshly verified leaf and retry
// with mbedTLS chain verification disabled, requiring instead that the
// server presents byte-for-byte the same certificate. That transfers
// the bundle attempt's chain and hostname verification to the retry,
// and TLS 1.3's CertificateVerify still proves the server holds the
// certificate's private key. The pin lives only in RAM, expires after
// a day (forcing a fresh bundle verification, e.g. for cert expiry),
// and any mismatch clears it and falls back to the bundle.
//
// No locking: KE attempts are serialized by the single nts_ke task
// (spawn_ke_task guards on ke_in_flight).
#define NTS_PIN_MAX    1600
#define NTS_PIN_TTL_MS (24u * 60u * 60u * 1000u)
static uint8_t pin_der[NTS_PIN_MAX];
static uint16_t pin_len;
static char pin_host[64];
static uint32_t pin_born_ms;

// Passed as crt_bundle_attach for the pinned attempt: esp-tls requires a
// verification option, and runs this after setting VERIFY_REQUIRED, so
// downgrading here sticks. The exact-DER check after the handshake is
// the actual verification.
static esp_err_t pin_attach_verify_none(void *conf) {
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

static bool ke_attempt(const char *host, ntp_nts_ctx_t *out) {
    memset(out, 0, sizeof(*out));
    out->ntp_port = 123;
    str_copy(out->ntp_host, sizeof(out->ntp_host), host);

    const char *alpn[] = { "ntske/1", NULL };
    esp_tls_cfg_t cfg = {
        .alpn_protos = alpn,
        .timeout_ms  = 8000,
        .tls_version = ESP_TLS_VER_TLS_1_3,  // RFC 8915 requires TLS 1.3
    };
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    bool use_pin = pin_len > 0 && strcmp(pin_host, host) == 0 &&
                   (uint32_t)(now_ms - pin_born_ms) < NTS_PIN_TTL_MS;
    cfg.crt_bundle_attach = use_pin ? pin_attach_verify_none : esp_crt_bundle_attach;

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return false;

    bool ok = esp_tls_conn_new_sync(host, (int)strlen(host), NTS_KE_PORT, &cfg, tls) == 1;

    bool keys_ok = false;
    if (ok) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)esp_tls_get_ssl_context(tls);
        const char *negotiated_alpn = ssl ? mbedtls_ssl_get_alpn_protocol(ssl) : NULL;
        static const char label[] = "EXPORTER-network-time-security";
        uint8_t ctx_c2s[5] = { 0x00, 0x00, 0x00, 0x0f, 0x00 };
        uint8_t ctx_s2c[5] = { 0x00, 0x00, 0x00, 0x0f, 0x01 };
        keys_ok = ssl && negotiated_alpn && strcmp(negotiated_alpn, "ntske/1") == 0 &&
            mbedtls_ssl_export_keying_material(ssl, out->c2s, NTS_KEY_LEN, label,
                strlen(label), ctx_c2s, sizeof(ctx_c2s), 1) == 0 &&
            mbedtls_ssl_export_keying_material(ssl, out->s2c, NTS_KEY_LEN, label,
                strlen(label), ctx_s2c, sizeof(ctx_s2c), 1) == 0;

        const mbedtls_x509_crt *peer = ssl ? mbedtls_ssl_get_peer_cert(ssl) : NULL;
        if (use_pin) {
            // The whole verification of this connection: the server must
            // present exactly the certificate the bundle attempt verified.
            if (!peer || peer->raw.len != pin_len ||
                memcmp(peer->raw.p, pin_der, pin_len) != 0) {
                ESP_LOGW(TAG, "pinned cert mismatch for %s", host);
                pin_len = 0;
                ok = false;
                keys_ok = false;
            }
        } else if (peer && peer->raw.len > 0 && peer->raw.len <= sizeof(pin_der)) {
            // Chain-verified by the bundle: (re)pin for fast retries.
            memcpy(pin_der, peer->raw.p, peer->raw.len);
            pin_len = (uint16_t)peer->raw.len;
            pin_born_ms = now_ms;
            str_copy(pin_host, sizeof(pin_host), host);
        }
    }

    // If ALPN or key export failed there is nothing to gain from the request/
    // response exchange, so fold keys_ok into the loop guard and fail fast.
    ok = ok && keys_ok;

    // Overall wall-clock budget for the whole KE exchange. The per-operation
    // esp-tls socket timeout is ~8 s, but a slow-drip server can reset it on
    // each byte and pin this 16 KB task for a very long time; cap the total.
    // (mono_ms() lives in ntp_internal.h, which cannot be included here without
    // clashing on its TAG macro, so use the same monotonic clock directly.)
    uint32_t ke_deadline = pdTICKS_TO_MS(xTaskGetTickCount()) + 15000;

    size_t sent = 0;
    while (ok && sent < sizeof(KE_REQUEST)) {
        if ((int32_t)(pdTICKS_TO_MS(xTaskGetTickCount()) - ke_deadline) >= 0) {
            ok = false;
            break;
        }
        int w = esp_tls_conn_write(tls, KE_REQUEST + sent, sizeof(KE_REQUEST) - sent);
        if (w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (w <= 0) { ok = false; break; }
        sent += (size_t)w;
    }

    uint8_t buf[NTS_RESP_BUF_LEN];
    size_t total = 0;
    bool eom = false;
    while (ok && total < sizeof(buf)) {
        if ((int32_t)(pdTICKS_TO_MS(xTaskGetTickCount()) - ke_deadline) >= 0) {
            ok = false;
            break;
        }
        int rd = esp_tls_conn_read(tls, buf + total, sizeof(buf) - total);
        if (rd == ESP_TLS_ERR_SSL_WANT_READ || rd == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (rd <= 0) break;
        total += (size_t)rd;
        int s = ke_parse(buf, total, out);
        if (s == 1) { eom = true; break; }
        if (s < 0)  { ok = false; break; }
    }

    esp_tls_conn_destroy(tls);

    if (ok && eom && keys_ok && out->cookie_count > 0) {
        out->valid = true;
        ESP_LOGI(TAG, "KE ok: %s:%u, %d cookies%s", out->ntp_host, out->ntp_port,
                 out->cookie_count, use_pin ? " (pinned cert)" : "");
        return true;
    }
    return false;
}

bool ntp_nts_ke_run(const char *host, ntp_nts_ctx_t *out) {
    // Up to three attempts: bundle-verified (slow, may miss the server's
    // connection deadline but pins the leaf), pinned (fast), and one
    // spare for the rotated-pin path (pinned fail clears the pin, the
    // bundle attempt re-pins, the third rides the fresh pin).
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ke_attempt(host, out)) return true;
    }
    ESP_LOGW(TAG, "KE failed for %s", host);
    return false;
}

bool ntp_nts_add_ef(uint8_t *buf, size_t *len, size_t cap,
                    ntp_nts_ctx_t *ctx, uint8_t uid_out[NTS_UID_LEN]) {
    if (ctx->cookie_count <= 0) return false;

    bool ok = false;
    size_t orig_len = *len;
    uint8_t uid[NTS_UID_LEN];
    uint8_t cookie[NTS_COOKIE_MAX];
    uint8_t nonce[NTS_NONCE_LEN];
    uint8_t tag[NTP_SIV_TAG_LEN];
    uint8_t body[4 + NTS_NONCE_LEN + NTP_SIV_TAG_LEN];

    esp_fill_random(uid, sizeof(uid));
    if (!ef_append(buf, len, cap, EF_UNIQUE_ID, uid, sizeof(uid))) {
        *len = orig_len;
        goto out;
    }

    uint16_t clen = ctx->cookie_len[0];
    memcpy(cookie, ctx->cookie[0], clen);
    if (!ef_append(buf, len, cap, EF_COOKIE, cookie, clen)) {
        *len = orig_len;
        goto out;
    }

    // Request enough replacement cookies to refill the pool, but never
    // fewer than three placeholders: chrony-style servers (PTB, BEV)
    // size their response at up to four cookies and silently DROP any
    // request too small to hold it - with a full pool we used to send
    // zero placeholders and every authenticated query vanished
    // (verified empirically: three placeholders answered, two dropped,
    // thresholds scale with the cookie length since placeholders are
    // cookie-sized). Replacement cookies beyond the pool capacity are
    // simply not stored.
    int want = NTS_MAX_COOKIES - ctx->cookie_count;
    if (want < 3) want = 3;
    for (int i = 0; i < want; i++) {
        if (!ef_append(buf, len, cap, EF_COOKIE_PLACEHOLDER, NULL, clen)) break;
    }

    esp_fill_random(nonce, sizeof(nonce));
    const uint8_t *ad[2]  = { buf, nonce };
    const size_t   adl[2] = { *len, sizeof(nonce) };
    if (!ntp_siv_encrypt(ctx->c2s, ad, adl, 2, NULL, 0, tag, NULL)) {
        *len = orig_len;
        goto out;
    }

    body[0] = 0; body[1] = NTS_NONCE_LEN;     // Nonce Length
    body[2] = 0; body[3] = NTP_SIV_TAG_LEN;   // Ciphertext Length (tag only)
    memcpy(body + 4, nonce, NTS_NONCE_LEN);
    memcpy(body + 4 + NTS_NONCE_LEN, tag, NTP_SIV_TAG_LEN);
    ok = ef_append(buf, len, cap, EF_AUTHENTICATOR, body, sizeof(body));
    if (ok) {
        memcpy(uid_out, uid, sizeof(uid));
        for (int i = 1; i < ctx->cookie_count; i++) {
            memcpy(ctx->cookie[i - 1], ctx->cookie[i], ctx->cookie_len[i]);
            ctx->cookie_len[i - 1] = ctx->cookie_len[i];
        }
        ctx->cookie_count--;
        mbedtls_platform_zeroize(ctx->cookie[ctx->cookie_count], NTS_COOKIE_MAX);
        ctx->cookie_len[ctx->cookie_count] = 0;
    } else {
        *len = orig_len;
    }

out:
    mbedtls_platform_zeroize(uid, sizeof(uid));
    mbedtls_platform_zeroize(cookie, sizeof(cookie));
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    mbedtls_platform_zeroize(tag, sizeof(tag));
    mbedtls_platform_zeroize(body, sizeof(body));
    return ok;
}

bool ntp_nts_check_response(const uint8_t *pkt, size_t pkt_len,
                            ntp_nts_ctx_t *ctx, const uint8_t uid[NTS_UID_LEN]) {
    if (pkt_len < 48) return false;

    // The Authenticator EF is last; everything before it is the AEAD
    // associated data. The echoed Unique ID is validated in the same pass.
    size_t off = 48;
    bool uid_ok = false;
    size_t auth_off = 0;
    bool have_auth = false;
    while (off + 4 <= pkt_len) {
        uint16_t type  = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t eflen = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        if (!ef_len_ok(eflen, off, pkt_len)) return false;
        if (type == EF_UNIQUE_ID) {
            if (uid_ok || eflen != 4 + NTS_UID_LEN ||
                memcmp(pkt + off + 4, uid, NTS_UID_LEN) != 0) {
                return false;
            }
            uid_ok = true;
        } else if (type == EF_AUTHENTICATOR) {
            auth_off = off;
            have_auth = true;
            break;
        }
        off += eflen;
    }
    if (!uid_ok || !have_auth) return false;

    const uint8_t *a = pkt + auth_off;
    uint16_t a_eflen = (uint16_t)((a[2] << 8) | a[3]);
    // ef_len_ok() already guaranteed a_eflen >= 16 for this EF in the parse
    // loop above, so only the exact-length (Authenticator is last) check remains.
    if (auth_off + a_eflen != pkt_len) return false;
    const uint8_t *bd = a + 4;
    uint16_t nonce_len = (uint16_t)((bd[0] << 8) | bd[1]);
    uint16_t ct_len    = (uint16_t)((bd[2] << 8) | bd[3]);
    size_t nonce_pad = (nonce_len + 3u) & ~3u;
    size_t ct_pad    = (ct_len + 3u) & ~3u;
    if (nonce_len != NTS_NONCE_LEN) return false;
    if ((size_t)4 + nonce_pad + ct_pad > (size_t)(a_eflen - 4)) return false;
    if (ct_len < NTP_SIV_TAG_LEN) return false;

    const uint8_t *nonce = bd + 4;
    const uint8_t *ct    = bd + 4 + nonce_pad;  // tag || encrypted EFs
    const uint8_t *tag   = ct;
    const uint8_t *enc   = ct + NTP_SIV_TAG_LEN;
    size_t enc_len       = ct_len - NTP_SIV_TAG_LEN;

    // AD = packet bytes before the Authenticator, plus the nonce. Decrypt to a
    // local buffer because PSA forbids in-place CTR.
    uint8_t plain[1100];
    if (enc_len > sizeof(plain)) return false;
    const uint8_t *ad[2]  = { pkt, nonce };
    const size_t   adl[2] = { auth_off, nonce_len };
    if (!ntp_siv_decrypt(ctx->s2c, ad, adl, 2, tag, enc, enc_len, plain)) {
        mbedtls_platform_zeroize(plain, sizeof(plain));
        return false;
    }

    bool enc_fields_ok = true;
    size_t po = 0;
    while (po + 4 <= enc_len) {
        uint16_t t = (uint16_t)((plain[po] << 8) | plain[po + 1]);
        uint16_t l = (uint16_t)((plain[po + 2] << 8) | plain[po + 3]);
        if (l < 16 || (l & 3u) != 0 || po + l > enc_len) {
            enc_fields_ok = false;
            break;
        }
        if (t == EF_COOKIE) {
            uint16_t cl = l - 4;
            if (cl > 0 && cl <= NTS_COOKIE_MAX && ctx->cookie_count < NTS_MAX_COOKIES) {
                memcpy(ctx->cookie[ctx->cookie_count], plain + po + 4, cl);
                ctx->cookie_len[ctx->cookie_count] = cl;
                ctx->cookie_count++;
            }
        }
        po += l;
    }
    if (po != enc_len) enc_fields_ok = false;
    mbedtls_platform_zeroize(plain, sizeof(plain));
    return enc_fields_ok;
}

bool ntp_nts_response_uid_matches(const uint8_t *pkt, size_t pkt_len,
                                  const uint8_t uid[NTS_UID_LEN]) {
    if (pkt_len < 48) return false;

    size_t off = 48;
    while (off + 4 <= pkt_len) {
        uint16_t type  = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t eflen = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        if (!ef_len_ok(eflen, off, pkt_len)) return false;
        if (type == EF_UNIQUE_ID) {
            return eflen == 4 + NTS_UID_LEN &&
                   memcmp(pkt + off + 4, uid, NTS_UID_LEN) == 0;
        }
        if (type == EF_AUTHENTICATOR) return false;
        off += eflen;
    }
    return false;
}

bool ntp_nts_selftest(void) {
    ntp_nts_ctx_t ctx = {0};
    for (size_t i = 0; i < NTS_KEY_LEN; i++) {
        ctx.c2s[i] = (uint8_t)i;
        ctx.s2c[i] = (uint8_t)(0xa0u + i);
    }

    uint8_t uid[NTS_UID_LEN];
    for (size_t i = 0; i < sizeof(uid); i++) uid[i] = (uint8_t)(0x40u + i);

    uint8_t pkt[160] = {0};
    size_t pkt_len = 48;
    if (!ef_append(pkt, &pkt_len, sizeof(pkt), EF_UNIQUE_ID, uid, sizeof(uid))) return false;

    uint8_t nonce[NTS_NONCE_LEN];
    for (size_t i = 0; i < sizeof(nonce); i++) nonce[i] = (uint8_t)(0x80u + i);
    const uint8_t *ad[2] = { pkt, nonce };
    const size_t adl[2] = { pkt_len, sizeof(nonce) };
    uint8_t tag[NTP_SIV_TAG_LEN];
    if (!ntp_siv_encrypt(ctx.s2c, ad, adl, 2, NULL, 0, tag, NULL)) return false;

    uint8_t body[4 + NTS_NONCE_LEN + NTP_SIV_TAG_LEN];
    body[0] = 0; body[1] = NTS_NONCE_LEN;
    body[2] = 0; body[3] = NTP_SIV_TAG_LEN;
    memcpy(body + 4, nonce, sizeof(nonce));
    memcpy(body + 4 + sizeof(nonce), tag, sizeof(tag));
    if (!ef_append(pkt, &pkt_len, sizeof(pkt), EF_AUTHENTICATOR, body, sizeof(body))) return false;
    if (!ntp_nts_check_response(pkt, pkt_len, &ctx, uid)) return false;

    uint8_t overlong_uid[sizeof(uid) + 4];
    memcpy(overlong_uid, uid, sizeof(uid));
    memset(overlong_uid + sizeof(uid), 0xa5, sizeof(overlong_uid) - sizeof(uid));
    uint8_t nak[96] = {0};
    size_t nak_len = 48;
    if (!ef_append(nak, &nak_len, sizeof(nak), EF_UNIQUE_ID,
                   overlong_uid, sizeof(overlong_uid))) return false;
    if (ntp_nts_response_uid_matches(nak, nak_len, uid)) return false;

    uint8_t trailing[4] = {0, 0, 0, 4};
    if (pkt_len + sizeof(trailing) > sizeof(pkt)) return false;
    memcpy(pkt + pkt_len, trailing, sizeof(trailing));
    if (ntp_nts_check_response(pkt, pkt_len + sizeof(trailing), &ctx, uid)) return false;

    mbedtls_platform_zeroize(&ctx, sizeof(ctx));
    mbedtls_platform_zeroize(tag, sizeof(tag));
    mbedtls_platform_zeroize(body, sizeof(body));
    return true;
}
