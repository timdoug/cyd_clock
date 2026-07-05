#include "ntp_siv.h"

#include <string.h>
#include <stdint.h>
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

// mbedTLS 4.x makes PSA Crypto the public API; the legacy mbedtls/aes.h and
// mbedtls/cmac.h are now private. CMAC and AES-CTR (the two primitives SIV is
// built from) are reached through PSA: an imported AES key plus PSA_ALG_CMAC /
// PSA_ALG_CTR. CTR encrypt and decrypt are the same keystream XOR, so the same
// cipher setup serves both directions.

#define BLK 16

// Caller destroys the returned key.
static bool cmac_key_import(const uint8_t k[BLK], mbedtls_svc_key_id_t *id) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_CMAC);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, BLK * 8);
    return psa_import_key(&a, k, BLK, id) == PSA_SUCCESS;
}

static bool cmac_oneshot(mbedtls_svc_key_id_t id, const uint8_t *in, size_t len,
                         uint8_t out[BLK]) {
    size_t olen = 0;
    return psa_mac_compute(id, PSA_ALG_CMAC, in, len, out, BLK, &olen) == PSA_SUCCESS &&
           olen == BLK;
}

// dbl() over GF(2^128) per RFC 5297: left-shift by one bit, and if the bit
// shifted out of the top was 1, XOR in the Rb constant (0x87 in the last byte).
static void dbl(uint8_t b[BLK]) {
    uint8_t carry = b[0] >> 7;
    for (int i = 0; i < BLK - 1; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[BLK - 1] = (uint8_t)(b[BLK - 1] << 1);
    // Constant-time: -(int)carry is 0 when carry==0 and all-ones (-1) when
    // carry==1, so the mask is 0x00 or 0x87 without branching on the secret bit.
    b[BLK - 1] ^= (uint8_t)(0x87 & -(int)carry);
}

static void xor_block(uint8_t dst[BLK], const uint8_t a[BLK], const uint8_t b[BLK]) {
    for (int i = 0; i < BLK; i++) dst[i] = a[i] ^ b[i];
}

static bool ranges_overlap(const void *a, size_t alen, const void *b, size_t blen) {
    if (alen == 0 || blen == 0) return false;
    uintptr_t ap = (uintptr_t)a;
    uintptr_t bp = (uintptr_t)b;
    return ap < bp ? bp - ap < alen : ap - bp < blen;
}

static bool inputs_valid(const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                         const uint8_t *text, size_t text_len) {
    if (ad_count > 0 && (!ad || !ad_len)) return false;
    for (size_t i = 0; i < ad_count; i++) {
        if (ad_len[i] > 0 && !ad[i]) return false;
    }
    return text_len == 0 || text != NULL;
}

// S2V(K, S_1, ..., S_m) per RFC 5297 section 2.4, where S_m (the last vector
// component) is the plaintext and S_1..S_{m-1} are the associated-data
// components passed in ad[]/ad_len[].
static bool s2v(const uint8_t key[BLK],
                const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                const uint8_t *pt, size_t pt_len, uint8_t out[BLK]) {
    static const uint8_t zero[BLK] = {0};
    mbedtls_svc_key_id_t id;
    if (!cmac_key_import(key, &id)) return false;

    bool ok = true;
    uint8_t d[BLK];
    ok = cmac_oneshot(id, zero, BLK, d);
    for (size_t i = 0; ok && i < ad_count; i++) {
        uint8_t c[BLK];
        ok = cmac_oneshot(id, ad[i], ad_len[i], c);
        dbl(d);
        xor_block(d, d, c);
        mbedtls_platform_zeroize(c, sizeof(c));
    }

    if (ok && pt_len >= BLK) {
        // T = xorend(S_m, D): CMAC over the whole plaintext with D XORed into
        // its final block. Streamed so no plaintext-sized copy is needed.
        psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
        size_t head = pt_len - BLK;
        uint8_t last[BLK];
        xor_block(last, pt + head, d);
        size_t olen = 0;
        ok = psa_mac_sign_setup(&op, id, PSA_ALG_CMAC) == PSA_SUCCESS &&
             psa_mac_update(&op, pt, head) == PSA_SUCCESS &&
             psa_mac_update(&op, last, BLK) == PSA_SUCCESS &&
             psa_mac_sign_finish(&op, out, BLK, &olen) == PSA_SUCCESS &&
             olen == BLK;
        if (!ok) psa_mac_abort(&op);
        mbedtls_platform_zeroize(last, sizeof(last));
    } else if (ok) {
        // T = dbl(D) XOR pad(S_m): pad appends 0x80 then zeros to a block.
        uint8_t t[BLK];
        dbl(d);
        memset(t, 0, BLK);
        if (pt_len) memcpy(t, pt, pt_len);
        t[pt_len] = 0x80;
        xor_block(t, t, d);
        ok = cmac_oneshot(id, t, BLK, out);
        mbedtls_platform_zeroize(t, sizeof(t));
    }

    psa_destroy_key(id);
    mbedtls_platform_zeroize(d, sizeof(d));
    return ok;
}

// CTR with the SIV-derived counter: zero bits 63 and 31 (the top bit of bytes 8
// and 12) of the synthetic IV per RFC 5297 section 2.6, then AES-CTR. in and out
// must not overlap.
static bool siv_ctr(const uint8_t enc_key[BLK], const uint8_t siv[BLK],
                    const uint8_t *in, size_t len, uint8_t *out) {
    if (len == 0) return true;
    if (!in || !out || ranges_overlap(in, len, out, len)) return false;
    uint8_t ctr[BLK];
    memcpy(ctr, siv, BLK);
    ctr[8]  &= 0x7f;
    ctr[12] &= 0x7f;

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_CTR);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, BLK * 8);
    mbedtls_svc_key_id_t id;
    if (psa_import_key(&a, enc_key, BLK, &id) != PSA_SUCCESS) {
        mbedtls_platform_zeroize(ctr, sizeof(ctr));
        return false;
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t o1 = 0, o2 = 0;
    bool ok = psa_cipher_encrypt_setup(&op, id, PSA_ALG_CTR) == PSA_SUCCESS &&
              psa_cipher_set_iv(&op, ctr, BLK) == PSA_SUCCESS &&
              psa_cipher_update(&op, in, len, out, len, &o1) == PSA_SUCCESS &&
              psa_cipher_finish(&op, out + o1, len - o1, &o2) == PSA_SUCCESS &&
              o1 + o2 == len;
    if (!ok) psa_cipher_abort(&op);
    psa_destroy_key(id);
    mbedtls_platform_zeroize(ctr, sizeof(ctr));
    return ok;
}

bool ntp_siv_encrypt(const uint8_t key[NTP_SIV_KEY_LEN],
                     const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                     const uint8_t *plaintext, size_t pt_len,
                     uint8_t siv_out[NTP_SIV_TAG_LEN], uint8_t *ct_out) {
    if (!key || !siv_out || !inputs_valid(ad, ad_len, ad_count, plaintext, pt_len)) return false;
    if (pt_len > 0 && (!ct_out || ranges_overlap(plaintext, pt_len, ct_out, pt_len))) return false;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    const uint8_t *mac_key = key;        // leftmost half: S2V
    const uint8_t *enc_key = key + BLK;  // rightmost half: CTR
    if (!s2v(mac_key, ad, ad_len, ad_count, plaintext, pt_len, siv_out)) return false;
    return siv_ctr(enc_key, siv_out, plaintext, pt_len, ct_out);
}

bool ntp_siv_decrypt(const uint8_t key[NTP_SIV_KEY_LEN],
                     const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                     const uint8_t siv[NTP_SIV_TAG_LEN],
                     const uint8_t *ct, size_t ct_len, uint8_t *pt_out) {
    if (!key || !siv || !inputs_valid(ad, ad_len, ad_count, ct, ct_len)) return false;
    if (ct_len > 0 && (!pt_out || ranges_overlap(ct, ct_len, pt_out, ct_len))) return false;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    const uint8_t *mac_key = key;
    const uint8_t *enc_key = key + BLK;
    if (!siv_ctr(enc_key, siv, ct, ct_len, pt_out)) return false;

    uint8_t check[BLK];
    bool ok = s2v(mac_key, ad, ad_len, ad_count, pt_out, ct_len, check);
    if (!ok) {
        if (ct_len > 0) mbedtls_platform_zeroize(pt_out, ct_len);
        mbedtls_platform_zeroize(check, sizeof(check));
        return false;
    }

    // Constant-time tag compare: accumulate differences so timing doesn't leak
    // how many leading bytes matched.
    uint8_t diff = 0;
    for (int i = 0; i < BLK; i++) diff |= (uint8_t)(check[i] ^ siv[i]);
    ok = diff == 0;
    if (!ok && ct_len > 0) mbedtls_platform_zeroize(pt_out, ct_len);
    mbedtls_platform_zeroize(check, sizeof(check));
    return ok;
}

bool ntp_siv_selftest(void) {
    // RFC 5297 Appendix A.1 deterministic vector (one AD component).
    static const uint8_t key[32] = {
        0xff,0xfe,0xfd,0xfc,0xfb,0xfa,0xf9,0xf8,0xf7,0xf6,0xf5,0xf4,0xf3,0xf2,0xf1,0xf0,
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
    };
    static const uint8_t ad[24] = {
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,
        0x1c,0x1d,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    };
    static const uint8_t pt[14] = {
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,
    };
    static const uint8_t want_siv[16] = {
        0x85,0x63,0x2d,0x07,0xc6,0xe8,0xf3,0x7f,0x95,0x0a,0xcd,0x32,0x0a,0x2e,0xcc,0x93,
    };
    static const uint8_t want_ct[14] = {
        0x40,0xc0,0x2b,0x96,0x90,0xc4,0xdc,0x04,0xda,0xef,0x7f,0x6a,0xfe,0x5c,
    };

    const uint8_t *advec[1]  = { ad };
    const size_t   adlens[1] = { sizeof(ad) };
    uint8_t siv[16], ct[14], pt_back[14];

    if (!ntp_siv_encrypt(key, advec, adlens, 1, pt, sizeof(pt), siv, ct)) return false;
    if (memcmp(siv, want_siv, 16) != 0) return false;
    if (memcmp(ct, want_ct, sizeof(ct)) != 0) return false;
    if (!ntp_siv_decrypt(key, advec, adlens, 1, siv, ct, sizeof(ct), pt_back)) return false;
    if (memcmp(pt_back, pt, sizeof(pt)) != 0) return false;

    // RFC 5297 Appendix A.2 nonce-based vector: multiple AD components plus
    // nonce, with plaintext long enough to exercise the S2V xorend branch.
    static const uint8_t key2[32] = {
        0x7f,0x7e,0x7d,0x7c,0x7b,0x7a,0x79,0x78,0x77,0x76,0x75,0x74,0x73,0x72,0x71,0x70,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
    };
    static const uint8_t ad1_2[40] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
        0xde,0xad,0xda,0xda,0xde,0xad,0xda,0xda,0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,0x88,
        0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00,
    };
    static const uint8_t ad2_2[10] = {
        0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,0x90,0xa0,
    };
    static const uint8_t nonce2[16] = {
        0x09,0xf9,0x11,0x02,0x9d,0x74,0xe3,0x5b,0xd8,0x41,0x56,0xc5,0x63,0x56,0x88,0xc0,
    };
    static const uint8_t pt2[47] = {
        0x74,0x68,0x69,0x73,0x20,0x69,0x73,0x20,0x73,0x6f,0x6d,0x65,0x20,0x70,0x6c,0x61,
        0x69,0x6e,0x74,0x65,0x78,0x74,0x20,0x74,0x6f,0x20,0x65,0x6e,0x63,0x72,0x79,0x70,
        0x74,0x20,0x75,0x73,0x69,0x6e,0x67,0x20,0x53,0x49,0x56,0x2d,0x41,0x45,0x53,
    };
    static const uint8_t want_siv2[16] = {
        0x7b,0xdb,0x6e,0x3b,0x43,0x26,0x67,0xeb,0x06,0xf4,0xd1,0x4b,0xff,0x2f,0xbd,0x0f,
    };
    static const uint8_t want_ct2[47] = {
        0xcb,0x90,0x0f,0x2f,0xdd,0xbe,0x40,0x43,0x26,0x60,0x19,0x65,0xc8,0x89,0xbf,0x17,
        0xdb,0xa7,0x7c,0xeb,0x09,0x4f,0xa6,0x63,0xb7,0xa3,0xf7,0x48,0xba,0x8a,0xf8,0x29,
        0xea,0x64,0xad,0x54,0x4a,0x27,0x2e,0x9c,0x48,0x5b,0x62,0xa3,0xfd,0x5c,0x0d,
    };
    const uint8_t *advec2[3]  = { ad1_2, ad2_2, nonce2 };
    const size_t   adlens2[3] = { sizeof(ad1_2), sizeof(ad2_2), sizeof(nonce2) };
    uint8_t siv2[16], ct2[47], back2[47];

    if (!ntp_siv_encrypt(key2, advec2, adlens2, 3, pt2, sizeof(pt2), siv2, ct2)) return false;
    if (memcmp(siv2, want_siv2, sizeof(siv2)) != 0) return false;
    if (memcmp(ct2, want_ct2, sizeof(ct2)) != 0) return false;
    if (!ntp_siv_decrypt(key2, advec2, adlens2, 3, siv2, ct2, sizeof(ct2), back2)) return false;
    if (memcmp(back2, pt2, sizeof(pt2)) != 0) return false;

    // A compact NTS-shaped tamper check with multiple AD components.
    static const uint8_t ad2[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    };
    static const uint8_t pt16[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
    };
    const uint8_t *tamper_advec[2]  = { ad, ad2 };
    const size_t   tamper_adlens[2] = { sizeof(ad), sizeof(ad2) };
    uint8_t siv16[16], ct16[16], back16[16];

    if (!ntp_siv_encrypt(key, tamper_advec, tamper_adlens, 2,
                         pt16, sizeof(pt16), siv16, ct16)) return false;
    if (!ntp_siv_decrypt(key, tamper_advec, tamper_adlens, 2,
                         siv16, ct16, sizeof(ct16), back16)) return false;
    if (memcmp(back16, pt16, sizeof(pt16)) != 0) return false;
    ct16[0] ^= 0x01;
    if (ntp_siv_decrypt(key, tamper_advec, tamper_adlens, 2,
                        siv16, ct16, sizeof(ct16), back16)) return false;
    ct16[0] ^= 0x01;
    siv16[0] ^= 0x01;
    if (ntp_siv_decrypt(key, tamper_advec, tamper_adlens, 2,
                        siv16, ct16, sizeof(ct16), back16)) return false;

    return true;
}
