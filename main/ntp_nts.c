#include "ntp_nts.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include "ntp_siv.h"
#include "util.h"

static const char *TAG = "nts";

#define EF_UNIQUE_ID          0x0104
#define EF_COOKIE             0x0204
#define EF_COOKIE_PLACEHOLDER 0x0304
#define EF_AUTHENTICATOR      0x0404

#define NTS_NONCE_LEN 16

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
            if (!proto_ok || !aead_ok || cookies == 0) return -1;
            out->cookie_count = cookies;
            return 1;
        case 1:
            for (int i = 0; i + 1 < blen; i += 2)
                if (((body[i] << 8) | body[i + 1]) == 0) proto_ok = true;
            break;
        case 2:
            ESP_LOGW(TAG, "KE error record code %u",
                     blen >= 2 ? (unsigned)((body[0] << 8) | body[1]) : 0);
            return -1;
        case 3:
            break;
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
            if (blen > 0 && blen < sizeof(out->ntp_host)) {
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

bool ntp_nts_ke_run(const char *host, ntp_nts_ctx_t *out) {
    memset(out, 0, sizeof(*out));
    out->ntp_port = 123;
    str_copy(out->ntp_host, sizeof(out->ntp_host), host);

    const char *alpn[] = { "ntske/1", NULL };
    esp_tls_cfg_t cfg = {
        .alpn_protos = alpn,
        .timeout_ms  = 8000,
        .tls_version = ESP_TLS_VER_TLS_1_3,  // RFC 8915 requires TLS 1.3
    };
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return false;

    bool ok = esp_tls_conn_new_sync(host, (int)strlen(host), NTS_KE_PORT, &cfg, tls) == 1;

    bool keys_ok = false;
    if (ok) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)esp_tls_get_ssl_context(tls);
        static const char label[] = "EXPORTER-network-time-security";
        uint8_t ctx_c2s[5] = { 0x00, 0x00, 0x00, 0x0f, 0x00 };
        uint8_t ctx_s2c[5] = { 0x00, 0x00, 0x00, 0x0f, 0x01 };
        keys_ok = ssl &&
            mbedtls_ssl_export_keying_material(ssl, out->c2s, NTS_KEY_LEN, label,
                strlen(label), ctx_c2s, sizeof(ctx_c2s), 1) == 0 &&
            mbedtls_ssl_export_keying_material(ssl, out->s2c, NTS_KEY_LEN, label,
                strlen(label), ctx_s2c, sizeof(ctx_s2c), 1) == 0;
    }

    size_t sent = 0;
    while (ok && sent < sizeof(KE_REQUEST)) {
        int w = esp_tls_conn_write(tls, KE_REQUEST + sent, sizeof(KE_REQUEST) - sent);
        if (w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (w <= 0) { ok = false; break; }
        sent += (size_t)w;
    }

    uint8_t buf[1500];
    size_t total = 0;
    bool eom = false;
    while (ok && total < sizeof(buf)) {
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
        ESP_LOGI(TAG, "KE ok: %s:%u, %d cookies", out->ntp_host, out->ntp_port,
                 out->cookie_count);
        return true;
    }
    ESP_LOGW(TAG, "KE failed for %s", host);
    return false;
}

bool ntp_nts_add_ef(uint8_t *buf, size_t *len, size_t cap,
                    ntp_nts_ctx_t *ctx, uint8_t uid_out[NTS_UID_LEN]) {
    if (ctx->cookie_count <= 0) return false;

    uint8_t uid[NTS_UID_LEN];
    esp_fill_random(uid, sizeof(uid));
    if (!ef_append(buf, len, cap, EF_UNIQUE_ID, uid, sizeof(uid))) return false;
    memcpy(uid_out, uid, sizeof(uid));

    uint8_t cookie[NTS_COOKIE_MAX];
    uint16_t clen = ctx->cookie_len[0];
    memcpy(cookie, ctx->cookie[0], clen);
    for (int i = 1; i < ctx->cookie_count; i++) {
        memcpy(ctx->cookie[i - 1], ctx->cookie[i], ctx->cookie_len[i]);
        ctx->cookie_len[i - 1] = ctx->cookie_len[i];
    }
    ctx->cookie_count--;
    if (!ef_append(buf, len, cap, EF_COOKIE, cookie, clen)) return false;

    // Request enough replacement cookies to refill the pool.
    int want = NTS_MAX_COOKIES - ctx->cookie_count - 1;
    for (int i = 0; i < want; i++) {
        if (!ef_append(buf, len, cap, EF_COOKIE_PLACEHOLDER, NULL, clen)) break;
    }

    uint8_t nonce[NTS_NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));
    const uint8_t *ad[2]  = { buf, nonce };
    const size_t   adl[2] = { *len, sizeof(nonce) };
    uint8_t tag[NTP_SIV_TAG_LEN];
    if (!ntp_siv_encrypt(ctx->c2s, ad, adl, 2, NULL, 0, tag, NULL)) return false;

    uint8_t body[4 + NTS_NONCE_LEN + NTP_SIV_TAG_LEN];
    body[0] = 0; body[1] = NTS_NONCE_LEN;     // Nonce Length
    body[2] = 0; body[3] = NTP_SIV_TAG_LEN;   // Ciphertext Length (tag only)
    memcpy(body + 4, nonce, NTS_NONCE_LEN);
    memcpy(body + 4 + NTS_NONCE_LEN, tag, NTP_SIV_TAG_LEN);
    return ef_append(buf, len, cap, EF_AUTHENTICATOR, body, sizeof(body));
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
        if (eflen < 4 || off + eflen > pkt_len) break;
        if (type == EF_UNIQUE_ID) {
            if (eflen - 4 >= NTS_UID_LEN && memcmp(pkt + off + 4, uid, NTS_UID_LEN) == 0)
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
    if (a_eflen < 8) return false;
    const uint8_t *bd = a + 4;
    uint16_t nonce_len = (uint16_t)((bd[0] << 8) | bd[1]);
    uint16_t ct_len    = (uint16_t)((bd[2] << 8) | bd[3]);
    size_t nonce_pad = (nonce_len + 3u) & ~3u;
    size_t ct_pad    = (ct_len + 3u) & ~3u;
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
    if (!ntp_siv_decrypt(ctx->s2c, ad, adl, 2, tag, enc, enc_len, plain)) return false;

    size_t po = 0;
    while (po + 4 <= enc_len) {
        uint16_t t = (uint16_t)((plain[po] << 8) | plain[po + 1]);
        uint16_t l = (uint16_t)((plain[po + 2] << 8) | plain[po + 3]);
        if (l < 4 || po + l > enc_len) break;
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
    return true;
}

bool ntp_nts_response_uid_matches(const uint8_t *pkt, size_t pkt_len,
                                  const uint8_t uid[NTS_UID_LEN]) {
    if (pkt_len < 48) return false;

    size_t off = 48;
    while (off + 4 <= pkt_len) {
        uint16_t type  = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t eflen = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        if (eflen < 4 || off + eflen > pkt_len) return false;
        if (type == EF_UNIQUE_ID) {
            return eflen - 4 >= NTS_UID_LEN &&
                   memcmp(pkt + off + 4, uid, NTS_UID_LEN) == 0;
        }
        if (type == EF_AUTHENTICATOR) return false;
        off += eflen;
    }
    return false;
}
