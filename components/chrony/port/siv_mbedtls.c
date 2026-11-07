/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2020, 2023
 * Copyright (C) Domaine Nyquist, A Limited Liability Company  2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 **********************************************************************

  =======================================================================

  SIV ciphers using mbedTLS 4 (PSA Crypto).  Port for the ESP32 build of
  chronyd, derived from siv_gnutls.c (instance handling and argument
  checks) with the cipher itself built from PSA primitives.

  Only AEAD_AES_SIV_CMAC_256 (RFC 5297, the AEAD that RFC 8915 mandates)
  is provided.  mbedTLS has no AES-GCM-SIV, so SIV_CreateInstance() and
  SIV_GetKeyLength() report AEAD_AES_128_GCM_SIV as unsupported, which
  chrony handles by negotiating AES-SIV-CMAC-256.

  mbedTLS ships CMAC and AES-CTR but no SIV mode.  The 32-byte key is
  split into two AES-128 halves: the left half drives S2V (CMAC), the
  right half drives CTR.  Both halves are imported as PSA keys once per
  SIV_SetKey() call rather than per message.

  Message layout matches nettle and gnutls: the ciphertext is the 16-byte
  synthetic IV followed by the encrypted plaintext, and S2V runs over the
  vector (associated data, nonce, plaintext).
  */

#include "config.h"

#include "sysincl.h"

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include "logging.h"
#include "memory.h"
#include "siv.h"

#define BLK 16

#define SIV_CMAC_256_KEY_LENGTH 32
#define SIV_CMAC_256_TAG_LENGTH BLK

struct SIV_Instance_Record {
  SIV_Algorithm algorithm;
  int key_set;
  mbedtls_svc_key_id_t mac_key;
  mbedtls_svc_key_id_t ctr_key;
};

/* ================================================== */

static int
init_psa(void)
{
  psa_status_t r = psa_crypto_init();

  if (r != PSA_SUCCESS) {
    DEBUG_LOG("Could not initialise %s : %d", "PSA", (int)r);
    return 0;
  }

  return 1;
}

/* ================================================== */

static void
destroy_keys(SIV_Instance instance)
{
  if (!instance->key_set)
    return;

  psa_destroy_key(instance->mac_key);
  psa_destroy_key(instance->ctr_key);
  instance->key_set = 0;
}

/* ================================================== */

static int
import_key(const unsigned char *key, psa_algorithm_t alg, psa_key_usage_t usage,
           mbedtls_svc_key_id_t *id)
{
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;

  psa_set_key_usage_flags(&a, usage);
  psa_set_key_algorithm(&a, alg);
  psa_set_key_type(&a, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&a, BLK * 8);

  return psa_import_key(&a, key, BLK, id) == PSA_SUCCESS;
}

/* ================================================== */

static int
cmac(mbedtls_svc_key_id_t id, const unsigned char *in, size_t len, unsigned char *out)
{
  size_t olen = 0;

  return psa_mac_compute(id, PSA_ALG_CMAC, in, len, out, BLK, &olen) == PSA_SUCCESS &&
         olen == BLK;
}

/* ================================================== */

/* dbl() over GF(2^128) per RFC 5297: left-shift by one bit, and if the bit
   shifted out of the top was 1, XOR in the Rb constant (0x87 in the last
   byte).  The mask -(int)carry is 0x00 or all-ones, so the secret bit
   selects the constant without a branch. */

static void
dbl(unsigned char *b)
{
  unsigned char carry = b[0] >> 7;
  int i;

  for (i = 0; i < BLK - 1; i++)
    b[i] = (unsigned char)((b[i] << 1) | (b[i + 1] >> 7));
  b[BLK - 1] = (unsigned char)(b[BLK - 1] << 1);
  b[BLK - 1] ^= (unsigned char)(0x87 & -(int)carry);
}

/* ================================================== */

static void
xor_block(unsigned char *dst, const unsigned char *a, const unsigned char *b)
{
  int i;

  for (i = 0; i < BLK; i++)
    dst[i] = a[i] ^ b[i];
}

/* ================================================== */

static int
ranges_overlap(const void *a, size_t alen, const void *b, size_t blen)
{
  uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;

  if (alen == 0 || blen == 0)
    return 0;

  return ap < bp ? bp - ap < alen : ap - bp < blen;
}

/* ================================================== */

/* S2V(K, AD, N, P) per RFC 5297 section 2.4 with the associated data and
   the nonce as the two leading components and the plaintext as the last */

static int
s2v(mbedtls_svc_key_id_t id, const unsigned char *assoc, size_t assoc_length,
    const unsigned char *nonce, size_t nonce_length,
    const unsigned char *pt, size_t pt_length, unsigned char *out)
{
  static const unsigned char zero[BLK] = {0};
  const unsigned char *components[2] = { assoc, nonce };
  size_t lengths[2] = { assoc_length, nonce_length };
  unsigned char d[BLK], c[BLK], t[BLK];
  int i, ok;

  ok = cmac(id, zero, BLK, d);

  for (i = 0; ok && i < 2; i++) {
    ok = cmac(id, components[i], lengths[i], c);
    dbl(d);
    xor_block(d, d, c);
  }

  if (ok && pt_length >= BLK) {
    /* T = xorend(P, D): CMAC over the whole plaintext with D XORed into
       its final block, streamed so no plaintext-sized copy is needed */
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t head = pt_length - BLK, olen = 0;

    xor_block(t, pt + head, d);
    ok = psa_mac_sign_setup(&op, id, PSA_ALG_CMAC) == PSA_SUCCESS &&
         psa_mac_update(&op, pt, head) == PSA_SUCCESS &&
         psa_mac_update(&op, t, BLK) == PSA_SUCCESS &&
         psa_mac_sign_finish(&op, out, BLK, &olen) == PSA_SUCCESS &&
         olen == BLK;
    if (!ok)
      psa_mac_abort(&op);
  } else if (ok) {
    /* T = dbl(D) XOR pad(P): pad appends 0x80 then zeros to a block */
    dbl(d);
    memset(t, 0, BLK);
    if (pt_length > 0)
      memcpy(t, pt, pt_length);
    t[pt_length] = 0x80;
    xor_block(t, t, d);
    ok = cmac(id, t, BLK, out);
  }

  mbedtls_platform_zeroize(d, sizeof (d));
  mbedtls_platform_zeroize(c, sizeof (c));
  mbedtls_platform_zeroize(t, sizeof (t));

  return ok;
}

/* ================================================== */

/* AES-CTR with the counter derived from the synthetic IV: bits 63 and 31
   (the top bits of bytes 8 and 12) are cleared per RFC 5297 section 2.6.
   Encryption and decryption are the same keystream XOR. */

static int
siv_ctr(mbedtls_svc_key_id_t id, const unsigned char *siv,
        const unsigned char *in, size_t len, unsigned char *out)
{
  psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
  unsigned char ctr[BLK];
  size_t o1 = 0, o2 = 0;
  int ok;

  if (len == 0)
    return 1;

  memcpy(ctr, siv, BLK);
  ctr[8] &= 0x7f;
  ctr[12] &= 0x7f;

  ok = psa_cipher_encrypt_setup(&op, id, PSA_ALG_CTR) == PSA_SUCCESS &&
       psa_cipher_set_iv(&op, ctr, BLK) == PSA_SUCCESS &&
       psa_cipher_update(&op, in, len, out, len, &o1) == PSA_SUCCESS &&
       psa_cipher_finish(&op, out + o1, len - o1, &o2) == PSA_SUCCESS &&
       o1 + o2 == len;
  if (!ok)
    psa_cipher_abort(&op);

  mbedtls_platform_zeroize(ctr, sizeof (ctr));

  return ok;
}

/* ================================================== */

SIV_Instance
SIV_CreateInstance(SIV_Algorithm algorithm)
{
  SIV_Instance instance;

  if (SIV_GetKeyLength(algorithm) <= 0)
    return NULL;

  if (!init_psa())
    return NULL;

  instance = MallocNew(struct SIV_Instance_Record);
  instance->algorithm = algorithm;
  instance->key_set = 0;
  instance->mac_key = MBEDTLS_SVC_KEY_ID_INIT;
  instance->ctr_key = MBEDTLS_SVC_KEY_ID_INIT;

  return instance;
}

/* ================================================== */

void
SIV_DestroyInstance(SIV_Instance instance)
{
  destroy_keys(instance);
  Free(instance);
}

/* ================================================== */

int
SIV_GetKeyLength(SIV_Algorithm algorithm)
{
  switch (algorithm) {
    case AEAD_AES_SIV_CMAC_256:
      return SIV_CMAC_256_KEY_LENGTH;
    default:
      return 0;
  }
}

/* ================================================== */

int
SIV_SetKey(SIV_Instance instance, const unsigned char *key, int length)
{
  mbedtls_svc_key_id_t mac_key, ctr_key;

  if (length <= 0 || length != SIV_GetKeyLength(instance->algorithm))
    return 0;

  /* Left half: S2V (CMAC).  Right half: CTR. */
  if (!import_key(key, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, &mac_key)) {
    DEBUG_LOG("Could not import %s key", "CMAC");
    return 0;
  }

  if (!import_key(key + BLK, PSA_ALG_CTR, PSA_KEY_USAGE_ENCRYPT, &ctr_key)) {
    DEBUG_LOG("Could not import %s key", "CTR");
    psa_destroy_key(mac_key);
    return 0;
  }

  destroy_keys(instance);

  instance->mac_key = mac_key;
  instance->ctr_key = ctr_key;
  instance->key_set = 1;

  return 1;
}

/* ================================================== */

int
SIV_GetMinNonceLength(SIV_Instance instance)
{
  return 1;
}

/* ================================================== */

int
SIV_GetMaxNonceLength(SIV_Instance instance)
{
  return INT_MAX;
}

/* ================================================== */

int
SIV_GetTagLength(SIV_Instance instance)
{
  return SIV_CMAC_256_TAG_LENGTH;
}

/* ================================================== */

int
SIV_Encrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const void *plaintext, int plaintext_length,
            unsigned char *ciphertext, int ciphertext_length)
{
  if (!instance->key_set)
    return 0;

  if (nonce_length < SIV_GetMinNonceLength(instance) || assoc_length < 0 ||
      plaintext_length < 0 ||
      plaintext_length + SIV_GetTagLength(instance) != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  /* The CTR pass reads the plaintext while writing the ciphertext */
  if (ranges_overlap(plaintext, plaintext_length, ciphertext, ciphertext_length))
    return 0;

  if (!s2v(instance->mac_key, assoc, assoc_length, nonce, nonce_length,
           plaintext, plaintext_length, ciphertext))
    return 0;

  if (!siv_ctr(instance->ctr_key, ciphertext, plaintext, plaintext_length,
               ciphertext + BLK))
    return 0;

  return 1;
}

/* ================================================== */

int
SIV_Decrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const unsigned char *ciphertext, int ciphertext_length,
            void *plaintext, int plaintext_length)
{
  unsigned char check[BLK], diff;
  int i, ok;

  if (!instance->key_set)
    return 0;

  if (nonce_length < SIV_GetMinNonceLength(instance) || assoc_length < 0 ||
      plaintext_length < 0 ||
      plaintext_length + SIV_GetTagLength(instance) != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  if (ranges_overlap(plaintext, plaintext_length, ciphertext, ciphertext_length))
    return 0;

  if (!siv_ctr(instance->ctr_key, ciphertext, ciphertext + BLK, plaintext_length,
               plaintext))
    return 0;

  ok = s2v(instance->mac_key, assoc, assoc_length, nonce, nonce_length,
           plaintext, plaintext_length, check);

  /* Constant-time tag comparison: the differences are accumulated so the
     timing does not depend on how many leading bytes matched */
  for (i = 0, diff = 0; i < BLK; i++)
    diff |= check[i] ^ ciphertext[i];
  ok = ok && diff == 0;

  if (!ok)
    mbedtls_platform_zeroize(plaintext, plaintext_length);
  mbedtls_platform_zeroize(check, sizeof (check));

  return ok;
}
