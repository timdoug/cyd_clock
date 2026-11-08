/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2020-2021
 * Copyright (C) Anthony Brandon  2025
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

  TLS session handling using mbedTLS 4.  Port for the ESP32 build of
  chronyd, derived from tls_gnutls.c.

  Client mode only: NTS-KE server credentials (certificate and key files)
  are rejected, since the device runs no NTS-KE server.  Server trust
  comes from the ESP-IDF certificate bundle (the vendored Mozilla roots);
  ntstrustedcerts files are not supported because there is no filesystem.

  Certificate validity periods (CONFIG_MBEDTLS_HAVE_TIME_DATE) are checked
  against the system clock, which chronyd steers; disable_time_checks (the
  "nocerttimecheck" directive) masks the expiry flags out of the
  verification result, and also skips the date check of a pinned
  certificate.  The get_time hook is not installed: mbedtls_platform_set_time()
  is global, so it would also apply to the firmware's other TLS clients (OTA)
  on other tasks, where chronyd's LCL_ReadCookedTime() must not be called.
  The cooked time differs from the system clock only by the correction
  being slewed, which is irrelevant at certificate-date resolution.
  */

#include "config.h"

#include "sysincl.h"

#include "tls.h"

#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "sched.h"
#include "util.h"

#include "esp_crt_bundle.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

struct TLS_Instance_Record {
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  int sock_fd;
  int server;
  int pinned;
  int ignore_time_flags;
  int time_checks;
  char *server_name;
  char *label;
  char *alpn_name;
  const char *alpn_list[2];
};

/* Credentials only record whether the certificate bundle is trusted */
struct Credentials {
  int use_bundle;
};

/* ================================================== */

/* Pinned NTS-KE server leaf certificates.  Some NTS-KE servers (Cloudflare)
   close connections about 2 s after accept; a full ESP32 handshake against
   an ECDSA chain cannot reliably fit (a single software P-384 verify
   measures ~1.3 s).  The handshake itself still completes and verifies the
   chain against the certificate bundle - only the request afterwards is
   lost - so the freshly verified leaf is stored and the retry runs
   with mbedTLS chain verification disabled, requiring instead that the
   server presents byte-for-byte the same certificate.  That transfers the
   bundle attempt's chain and hostname verification to the retry, and
   TLS 1.3's CertificateVerify still proves the server holds the
   certificate's private key.  Pins live only in RAM (across TLS_Finalise(),
   which chronyd calls between sessions), expire after a day
   (forcing a fresh bundle verification, e.g. for certificate expiry), and
   any mismatch clears the pin and falls back to the bundle.

   All TLS calls run in the chronyd task, so the table needs no lock. */

#define PIN_SLOTS 4
#define PIN_MAX_DER 1600
#define PIN_TTL (24 * 3600.0)

struct Pin {
  char *host;
  unsigned char *der;
  size_t der_length;
  double born;
};

static struct Pin pins[PIN_SLOTS];

/* ================================================== */

static void
clear_pin(struct Pin *pin)
{
  Free(pin->host);
  Free(pin->der);
  memset(pin, 0, sizeof (*pin));
}

/* ================================================== */

static struct Pin *
find_pin(const char *host)
{
  double now = SCH_GetLastEventMonoTime();
  int i;

  for (i = 0; i < PIN_SLOTS; i++) {
    if (!pins[i].host || strcmp(pins[i].host, host) != 0)
      continue;
    if (now - pins[i].born < PIN_TTL && now >= pins[i].born)
      return &pins[i];
    clear_pin(&pins[i]);
  }

  return NULL;
}

/* ================================================== */

static void
store_pin(const char *host, const unsigned char *der, size_t der_length)
{
  struct Pin *pin = NULL;
  int i;

  if (der_length == 0 || der_length > PIN_MAX_DER)
    return;

  /* Replace an existing pin for the host, else take a free slot, else
     evict the oldest */
  for (i = 0; i < PIN_SLOTS; i++) {
    if (pins[i].host && strcmp(pins[i].host, host) == 0) {
      pin = &pins[i];
      break;
    }
    if (!pin && !pins[i].host)
      pin = &pins[i];
  }
  if (!pin) {
    pin = &pins[0];
    for (i = 1; i < PIN_SLOTS; i++) {
      if (pins[i].born < pin->born)
        pin = &pins[i];
    }
  }

  clear_pin(pin);
  pin->host = Strdup(host);
  pin->der = Malloc(der_length);
  memcpy(pin->der, der, der_length);
  pin->der_length = der_length;
  pin->born = SCH_GetLastEventMonoTime();
}

/* ================================================== */

/* Check the peer certificate against the pin (pinned session) or record it
   as a new pin (bundle-verified session).  Return 0 if a pinned session
   presented a different certificate. */

static int
check_pin(TLS_Instance inst)
{
  const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&inst->ssl);
  struct Pin *pin;

  if (!inst->pinned) {
    if (peer)
      store_pin(inst->server_name, peer->raw.p, peer->raw.len);
    return 1;
  }

  pin = find_pin(inst->server_name);
  if (pin && peer && peer->raw.len == pin->der_length &&
      memcmp(peer->raw.p, pin->der, pin->der_length) == 0) {
#if defined(MBEDTLS_HAVE_TIME_DATE)
    /* The pin stands in for chain verification, not for the validity
       period, which is checked against the current time as in a bundle
       session */
    if (inst->time_checks && (mbedtls_x509_time_is_past(&peer->valid_to) ||
                              mbedtls_x509_time_is_future(&peer->valid_from))) {
      LOG(LOGS_WARN, "Pinned certificate for %s outside its validity period",
          inst->label);
      clear_pin(pin);
      return 0;
    }
#endif
    return 1;
  }

  LOG(LOGS_WARN, "Pinned certificate mismatch for %s", inst->label);
  if (pin)
    clear_pin(pin);

  return 0;
}

/* ================================================== */

/* Check for a FIN or RST already received on the socket */

static int
peer_closed(TLS_Instance inst)
{
  char c;
  int r = recv(inst->sock_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);

  return r == 0 || (r < 0 && (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE));
}

/* ================================================== */

static int
bio_send(void *ctx, const unsigned char *buf, size_t len)
{
  TLS_Instance inst = ctx;
  int r = send(inst->sock_fd, buf, len, 0);

  if (r >= 0)
    return r;

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  if (errno == EPIPE || errno == ECONNRESET)
    return MBEDTLS_ERR_NET_CONN_RESET;

  return MBEDTLS_ERR_NET_SEND_FAILED;
}

/* ================================================== */

static int
bio_recv(void *ctx, unsigned char *buf, size_t len)
{
  TLS_Instance inst = ctx;
  int r = recv(inst->sock_fd, buf, len, 0);

  if (r >= 0)
    return r;

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    return MBEDTLS_ERR_SSL_WANT_READ;
  if (errno == EPIPE || errno == ECONNRESET)
    return MBEDTLS_ERR_NET_CONN_RESET;

  return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ================================================== */

/* Errors which mean the other end closed or reset the connection
   (gnutls: GNUTLS_E_PULL_ERROR and GNUTLS_E_PREMATURE_TERMINATION) */

static int
is_closed_error(int r)
{
  return r == MBEDTLS_ERR_SSL_CONN_EOF || r == MBEDTLS_ERR_NET_CONN_RESET ||
         r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
}

/* ================================================== */

static TLS_Status
again_status(int r)
{
  return r == MBEDTLS_ERR_SSL_WANT_WRITE ? TLS_AGAIN_OUTPUT : TLS_AGAIN_INPUT;
}

/* ================================================== */

static int
is_again_error(int r)
{
  return r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE;
}

/* ================================================== */

int
TLS_Initialise(time_t (*get_time)(time_t *t))
{
  psa_status_t s = psa_crypto_init();

  if (s != PSA_SUCCESS) {
    LOG(LOGS_ERR, "Could not initialise %s : %d", "PSA", (int)s);
    return 0;
  }

  return 1;
}

/* ================================================== */

void
TLS_Finalise(void)
{
  /* The pins are kept: chronyd finalises the TLS layer whenever its last
     NTS-KE session ends (deinit_tls() in nts_ke_session.c), which is
     exactly before the retry that the pin exists for.  They expire with
     PIN_TTL instead. */
}

/* ================================================== */

TLS_Credentials
TLS_CreateCredentials(const char **certs, const char **keys, int n_certs_keys,
                      const char **trusted_certs, uint32_t *trusted_certs_ids,
                      int n_trusted_certs, uint32_t trusted_cert_set)
{
  struct Credentials *credentials;
  int i;

  if (certs || keys) {
    LOG(LOGS_ERR, "NTS server is not supported");
    return NULL;
  }

  BRIEF_ASSERT(n_certs_keys <= 0);

  if (trusted_certs && trusted_certs_ids) {
    for (i = 0; i < n_trusted_certs; i++) {
      if (trusted_certs_ids[i] != trusted_cert_set)
        continue;
      LOG(LOGS_ERR, "Could not load trusted certificates from %s : %s",
          trusted_certs[i], "not supported");
      return NULL;
    }
  }

  credentials = MallocNew(struct Credentials);
  credentials->use_bundle = trusted_cert_set == 0 && !CNF_GetNoSystemCert();

  if (credentials->use_bundle && esp_crt_bundle_attach(NULL) != 0) {
    LOG(LOGS_ERR, "Could not set credentials : %s", "certificate bundle");
    Free(credentials);
    return NULL;
  }

  return credentials;
}

/* ================================================== */

void
TLS_DestroyCredentials(TLS_Credentials credentials)
{
  Free(credentials);
}

/* ================================================== */

/* Concurrent TLS sessions.  Each NTS-KE session holds a 16 KB message
   buffer (NKE_MAX_MESSAGE_LENGTH, allocated by nts_ke_session.c) and about
   15 KB of TLS state, plus about 8 KB more during the handshake, and
   chronyd starts the sessions of all sources of a pool at once.  Four at
   once exhausted the heap (a failed Malloc() is fatal in chronyd).  A
   refused session fails NKC_Start(), which chronyd retries after
   RETRY_INTERVAL_KE_START (2 s), so sessions beyond the limit queue. */
#define MAX_SESSIONS 2

static int active_sessions;

/* ================================================== */

TLS_Instance
TLS_CreateInstance(int server_mode, int sock_fd, const char *server_name, const char *label,
                   const char *alpn_name, TLS_Credentials credentials, int disable_time_checks)
{
  struct Credentials *creds = credentials;
  int r = 0, name_len;

  if (active_sessions >= MAX_SESSIONS) {
    DEBUG_LOG("Deferring TLS session with %s (%d active)", label, active_sessions);
    return NULL;
  }

  TLS_Instance inst = MallocNew(struct TLS_Instance_Record);
  active_sessions++;

  mbedtls_ssl_config_init(&inst->conf);
  mbedtls_ssl_init(&inst->ssl);
  inst->sock_fd = sock_fd;
  inst->server = server_mode;
  inst->pinned = 0;
  inst->ignore_time_flags = 0;
  inst->time_checks = !disable_time_checks;
  inst->server_name = NULL;
  inst->label = Strdup(label);
  inst->alpn_name = Strdup(alpn_name);
  inst->alpn_list[0] = inst->alpn_name;
  inst->alpn_list[1] = NULL;

  if (server_mode) {
    LOG(LOGS_ERR, "NTS server is not supported");
    goto error;
  }

  assert(server_name);

  inst->server_name = Strdup(server_name);

  /* Remove the trailing dot from the hostname if present, as is done for
     gnutls_server_name_set() and gnutls_session_set_verify_cert() */
  name_len = strlen(inst->server_name);
  if (name_len > 1 && inst->server_name[name_len - 1] == '.')
    inst->server_name[name_len - 1] = '\0';

  r = mbedtls_ssl_config_defaults(&inst->conf, MBEDTLS_SSL_IS_CLIENT,
                                  MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (r != 0)
    goto error;

  /* The NTS specification requires TLS1.3 or later */
  mbedtls_ssl_conf_min_tls_version(&inst->conf, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_max_tls_version(&inst->conf, MBEDTLS_SSL_VERSION_TLS1_3);

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
  /* As GNUTLS_NO_TICKETS: sessions are not resumed */
  mbedtls_ssl_conf_session_tickets(&inst->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif

  r = mbedtls_ssl_conf_alpn_protocols(&inst->conf, inst->alpn_list);
  if (r != 0)
    goto error;

  if (creds->use_bundle && find_pin(inst->server_name)) {
    /* The exact-DER check after the handshake is the verification */
    inst->pinned = 1;
    mbedtls_ssl_conf_authmode(&inst->conf, MBEDTLS_SSL_VERIFY_NONE);
    DEBUG_LOG("Using pinned certificate for %s", inst->server_name);
  } else {
    /* Without the bundle (nosystemcert) no certificate is trusted and the
       handshake fails verification, as with gnutls and no trusted certs */
    if (creds->use_bundle) {
      r = esp_crt_bundle_attach(&inst->conf);
      if (r != 0)
        goto error;
    }
    mbedtls_ssl_conf_authmode(&inst->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

#if defined(MBEDTLS_HAVE_TIME_DATE)
    if (disable_time_checks) {
      /* Verification failures are checked after the handshake with the
         expiry flags masked */
      mbedtls_ssl_conf_authmode(&inst->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
      inst->ignore_time_flags = 1;
      DEBUG_LOG("Disabled time checks");
    }
#endif
  }

  r = mbedtls_ssl_setup(&inst->ssl, &inst->conf);
  if (r != 0)
    goto error;

  /* The hostname is used for SNI and certificate name verification
     (including IP address SANs).  Unlike gnutls_server_name_set(), which
     is skipped for an IP address, mbedTLS has a single hostname setting,
     so an IP address is also sent as SNI. */
  r = mbedtls_ssl_set_hostname(&inst->ssl, inst->server_name);
  if (r != 0)
    goto error;

  mbedtls_ssl_set_bio(&inst->ssl, inst, bio_send, bio_recv, NULL);

  return inst;

error:
  LOG(LOGS_ERR, "Could not %s TLS session : %d", "set", r);
  TLS_DestroyInstance(inst);
  return NULL;
}

/* ================================================== */

void
TLS_DestroyInstance(TLS_Instance inst)
{
  active_sessions--;
  mbedtls_ssl_free(&inst->ssl);
  mbedtls_ssl_config_free(&inst->conf);

  if (inst->server_name)
    Free(inst->server_name);
  Free(inst->label);
  Free(inst->alpn_name);

  Free(inst);
}

/* ================================================== */

static int
check_alpn(TLS_Instance inst)
{
  const char *alpn = mbedtls_ssl_get_alpn_protocol(&inst->ssl);

  return alpn && strcmp(alpn, inst->alpn_name) == 0;
}

/* ================================================== */

static int
check_verify_result(TLS_Instance inst)
{
#if defined(MBEDTLS_HAVE_TIME_DATE)
  uint32_t flags;

  if (!inst->ignore_time_flags)
    return 1;

  flags = mbedtls_ssl_get_verify_result(&inst->ssl);
  flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
  if (flags != 0) {
    LOG(LOGS_ERR, "TLS handshake with %s failed : %s %#"PRIx32, inst->label,
        "certificate verification", flags);
    return 0;
  }
#endif

  return 1;
}

/* ================================================== */

TLS_Status
TLS_DoHandshake(TLS_Instance inst)
{
  int r = mbedtls_ssl_handshake(&inst->ssl);

  if (r != 0) {
    if (is_again_error(r))
      return again_status(r);

    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
      LOG(LOGS_ERR, "TLS handshake with %s failed : %d verify %#"PRIx32, inst->label, r,
          mbedtls_ssl_get_verify_result(&inst->ssl));
    else
      LOG(LOGS_ERR, "TLS handshake with %s failed : %d", inst->label, r);

    /* A pinned session that failed other than by the server closing the
       connection falls back to the bundle on the next try */
    if (inst->pinned && !is_closed_error(r)) {
      struct Pin *pin = find_pin(inst->server_name);
      if (pin)
        clear_pin(pin);
    }

    /* Increase the retry interval if the handshake did not fail due
       to the other end closing the connection */
    if (!is_closed_error(r))
      return TLS_FAILED;

    return TLS_CLOSED;
  }

  DEBUG_LOG("Handshake with %s completed %s %s", inst->label,
            mbedtls_ssl_get_version(&inst->ssl), mbedtls_ssl_get_ciphersuite(&inst->ssl));

  if (!check_verify_result(inst) || !check_pin(inst))
    return TLS_FAILED;

  if (!check_alpn(inst)) {
    LOG(LOGS_ERR, "NTS-KE not supported by %s", inst->label);
    return TLS_FAILED;
  }

  /* A server that closes connections shortly after accepting them (see the
     pin comment above) can do so while the bundle-verified handshake is
     still being completed here.  The certificate is pinned by now, but
     reporting success would make chronyd send the request into the closed
     connection and then back off for 2^NKE_RETRY_FACTOR2_TLS seconds;
     reporting the close makes it retry with the short interval it uses for
     a handshake that the other end closed, and the retry uses the pin. */
  if (!inst->pinned && peer_closed(inst)) {
    LOG(LOGS_INFO, "%s closed the connection during the handshake", inst->label);
    return TLS_CLOSED;
  }

  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Send(TLS_Instance inst, const void *data, int length, int *sent)
{
  int r;

  if (length < 0)
    return TLS_FAILED;

  r = mbedtls_ssl_write(&inst->ssl, data, length);

  if (r < 0) {
    if (is_again_error(r))
      return TLS_AGAIN_OUTPUT;

    LOG(LOGS_ERR, "Could not send NTS-KE message to %s : %d", inst->label, r);
    return TLS_FAILED;
  }

  *sent = r;

  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Receive(TLS_Instance inst, void *data, int length, int *received)
{
  int r;

  if (length < 0)
    return TLS_FAILED;

  do {
    r = mbedtls_ssl_read(&inst->ssl, data, length);
    /* A TLS1.3 NewSessionTicket can be reported even with tickets disabled;
       the read is repeated to get the application data */
  } while (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET);

  /* A close_notify is the end of the stream (as a zero-length read
     with gnutls) */
  if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    r = 0;

  if (r < 0) {
    if (is_again_error(r))
      return TLS_AGAIN_INPUT;

    LOG(LOGS_ERR, "Could not receive NTS-KE message from %s : %d", inst->label, r);
    return TLS_FAILED;
  }

  *received = r;

  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_CheckPending(TLS_Instance inst)
{
  /* Decrypted bytes, or records already read from the socket and not
     processed yet (no socket readiness event follows for those) */
  return mbedtls_ssl_get_bytes_avail(&inst->ssl) > 0 ||
         mbedtls_ssl_check_pending(&inst->ssl);
}

/* ================================================== */

TLS_Status
TLS_Shutdown(TLS_Instance inst)
{
  int r = mbedtls_ssl_close_notify(&inst->ssl);

  if (r != 0) {
    if (is_again_error(r))
      return again_status(r);

    DEBUG_LOG("Shutdown with %s failed : %d", inst->label, r);
    return TLS_FAILED;
  }

  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_ExportKey(TLS_Instance inst, int label_length, const char *label, int context_length,
              const void *context, int key_length, unsigned char *key)
{
  if (label_length < 0 || context_length < 0 || key_length < 0)
    return 0;

  return mbedtls_ssl_export_keying_material(&inst->ssl, key, key_length, label, label_length,
                                            context, context_length, 1) == 0;
}
