#ifndef CYD_NTP_SIV_H
#define CYD_NTP_SIV_H

// AES-SIV-CMAC-256 (RFC 5297) - the AEAD that NTS (RFC 8915) mandates,
// registered as AEAD_AES_SIV_CMAC_256 (IANA AEAD id 15). The 32-byte key is
// split into two AES-128 halves: the left half drives S2V (CMAC), the right
// half drives CTR. mbedTLS ships CMAC and AES-CTR but no SIV mode, so this is
// a thin, self-contained build on top of those primitives.
//
// The associated data is passed as a vector of components in order; for the
// nonce-based NTS use it is {packet_prefix, nonce}. The synthetic IV (the
// 16-byte authentication tag) is returned separately from the ciphertext so
// callers can lay them out per the NTS Authenticator extension field.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTP_SIV_KEY_LEN 32
#define NTP_SIV_TAG_LEN 16

// Encrypt: writes the 16-byte synthetic IV to siv_out and pt_len ciphertext
// bytes to ct_out (ct_out may equal plaintext for in-place; may be NULL iff
// pt_len == 0). Returns true on success.
bool ntp_siv_encrypt(const uint8_t key[NTP_SIV_KEY_LEN],
                     const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                     const uint8_t *plaintext, size_t pt_len,
                     uint8_t siv_out[NTP_SIV_TAG_LEN], uint8_t *ct_out);

// Decrypt + verify: recovers pt_len plaintext bytes into pt_out and checks the
// synthetic IV in constant time. Returns true only if authentication passes;
// on failure pt_out contents are undefined and must not be trusted.
bool ntp_siv_decrypt(const uint8_t key[NTP_SIV_KEY_LEN],
                     const uint8_t *const ad[], const size_t ad_len[], size_t ad_count,
                     const uint8_t siv[NTP_SIV_TAG_LEN],
                     const uint8_t *ct, size_t ct_len, uint8_t *pt_out);

// RFC 5297 Appendix A.1 deterministic test vector. Returns true if the
// implementation reproduces the published output. Cheap; call once to guard
// against a miscompiled CMAC/CTR path before trusting authenticated time.
bool ntp_siv_selftest(void);

#endif
