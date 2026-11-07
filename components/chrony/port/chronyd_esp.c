/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2012-2020
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

  chronyd entry point for ESP-IDF, in place of upstream main.c. The module
  initialisation order follows main(); the differences are:

  - Configuration comes from constant directives (no file), and sources
    are added and removed at run time through ces_add_source() and
    ces_remove_source(), parsed by the same code chronyc's "add" uses.
  - There is no daemon/fork, pidfile, privilege drop, signal handling or
    command socket.
  - Other tasks reach chronyd through a one-slot command mailbox whose
    wakeup is a file handler in chronyd's own select() loop, so every
    chrony function runs on the chronyd task.

  Also here: pipe() for nameserv_async.c, and the getaddrinfo() wrapper
  that netdb.h redirects chrony's calls to.

  */

#include "config.h"

#include "sysincl.h"

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "chronyd_esp.h"
#include "nvs_file.h"

#include "clientlog.h"
#include "cmdmon.h"
#include "cmdparse.h"
#include "conf.h"
#include "keys.h"
#include "leapdb.h"
#include "local.h"
#include "logging.h"
#include "main.h"
#include "manual.h"
#include "memory.h"
#include "nameserv.h"
#include "ntp_core.h"
#include "ntp_io.h"
#include "ntp_signd.h"
#include "ntp_sources.h"
#include "nts_ke_server.h"
#include "nts_ntp_server.h"
#include "privops.h"
#include "refclock.h"
#include "reference.h"
#include "rtc.h"
#include "sched.h"
#include "smooth.h"
#include "socket.h"
#include "sources.h"
#include "sourcestats.h"
#include "sys.h"
#include "tempcomp.h"
#include "util.h"

/* ================================================== */

#define TASK_STACK_BYTES 16384
#define TASK_PRIORITY 5
#define TASK_CORE 0

/* Interval of the status snapshot refresh */
#define STATUS_INTERVAL 1.0

/* Directives applied before the modules are initialised, from chrony's
   examples/chrony.conf.example1 less "rtcsync" (no RTC driver is built),
   plus "nocerttimecheck".
   The driftfile lives on the NVS-backed filesystem in nvs_file.c. "port 0"
   (no NTP server socket) and "cmdport 0" come from CNF_Initialise()'s
   client-only mode. Sources are added at run time (ces_add_source()). */
static const char *const config_lines[] = {
  "driftfile " CES_FILE_ROOT "/drift",
  "makestep 1.0 3",
  /* No RTC: the clock starts in 1970, so NTS-KE certificates cannot be
     date-checked until the first clock update (chrony.conf(5)
     "nocerttimecheck") */
  "nocerttimecheck 1",
};

/* ================================================== */

typedef enum {
  CMD_ADD_SOURCE,
  CMD_REMOVE_SOURCE,
} CommandType;

typedef struct {
  CommandType type;
  const char *directive;
  uint32_t group;
  int result;
} Command;

static TaskHandle_t task;
static atomic_int initialised;

static SemaphoreHandle_t command_lock;
static SemaphoreHandle_t command_done;
static Command *pending_command;
static int wake_fds[2];

static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static ces_status_t status;
static ces_status_t scratch_status;

/* Group id of each added directive, keyed by the source name chrony keeps
   for every address resolved from it (NSR_GetName()). */
typedef struct {
  uint32_t conf_id;
  char name[CES_NAME_LEN];
} Group;

static Group groups[CES_MAX_SOURCES];

/* ================================================== */

void
MAI_CleanupAndExit(void)
{
  /* Reached only from fatal paths; LOG_FATAL() exits after logging. */
  exit(1);
}

/* ================================================== */

static void
copy_ip(char *dst, size_t len, IPAddr *ip)
{
  snprintf(dst, len, "%s", UTI_IPToString(ip));
}

/* ================================================== */

static void
fill_source(int index, struct timespec *now, ces_source_t *src)
{
  RPT_SourceReport sr;
  RPT_AuthReport ar;
  const char *name;
  int i;

  memset(src, 0, sizeof (*src));
  memset(&sr, 0, sizeof (sr));
  if (!SRC_ReportSource(index, &sr, now))
    return;
  /* SRC_ReportSource() leaves poll and mode to the NTP layer, as in
     cmdmon.c's handle_source_data() */
  NSR_ReportSource(&sr.ip_addr, &sr);

  copy_ip(src->addr, sizeof (src->addr), &sr.ip_addr);
  src->resolved = sr.ip_addr.family == IPADDR_INET4 ||
                  sr.ip_addr.family == IPADDR_INET6;
  name = NSR_GetName(&sr.ip_addr);
  snprintf(src->name, sizeof (src->name), "%s", name ? name : src->addr);

  /* process_cmd_sources() in client.c, through cmdmon.c's RPY_SD_* codes */
  switch (sr.mode) {
    case RPT_NTP_CLIENT:      src->mode = '^'; break;
    case RPT_NTP_PEER:        src->mode = '='; break;
    case RPT_LOCAL_REFERENCE: src->mode = '#'; break;
    default:                  src->mode = ' '; break;
  }
  switch (sr.state) {
    case RPT_SELECTED:        src->state = '*'; break;
    case RPT_NONSELECTABLE:   src->state = '?'; break;
    case RPT_FALSETICKER:     src->state = 'x'; break;
    case RPT_JITTERY:         src->state = '~'; break;
    case RPT_UNSELECTED:      src->state = '+'; break;
    case RPT_SELECTABLE:      src->state = '-'; break;
    default:                  src->state = ' '; break;
  }
  src->stratum = sr.stratum;
  src->poll = sr.poll;
  src->reach = sr.reachability;
  src->since_sample = sr.latest_meas_ago;
  src->latest_meas = sr.latest_meas;
  src->orig_latest_meas = sr.orig_latest_meas;
  src->latest_meas_err = sr.latest_meas_err;

  /* handle_auth_data() in cmdmon.c */
  src->auth_mode = "-";
  if (NSR_GetAuthReport(&sr.ip_addr, &ar)) {
    switch (ar.mode) {
      case NTP_AUTH_NONE:      src->auth_mode = "-"; break;
      case NTP_AUTH_SYMMETRIC: src->auth_mode = "SK"; break;
      case NTP_AUTH_NTS:       src->auth_mode = "NTS"; break;
      default:                 src->auth_mode = "?"; break;
    }
    src->key_id = ar.key_id;
    src->key_type = ar.key_type;
    src->key_length = ar.key_length;
    src->last_ke_ago = ar.last_ke_ago;
    src->ke_attempts = ar.ke_attempts;
    src->nak = ar.nak;
    src->cookies = ar.cookies;
    src->cookie_length = ar.cookie_length;
  }

  for (i = 0; i < CES_MAX_SOURCES; i++) {
    if (groups[i].conf_id && name && strcmp(groups[i].name, name) == 0) {
      src->group = groups[i].conf_id;
      break;
    }
  }
}

/* ================================================== */

static void
refresh_status(void)
{
  RPT_TrackingReport tr;
  struct timespec now;
  ces_status_t *s = &scratch_status;
  const char *name;
  int i, n;

  memset(s, 0, sizeof (*s));
  SCH_GetLastEventTime(&now, NULL, NULL);

  REF_GetTrackingReport(&tr);
  s->running = true;
  s->ref_id = tr.ref_id;
  if (tr.ip_addr.family == IPADDR_INET4 || tr.ip_addr.family == IPADDR_INET6) {
    name = NSR_GetName(&tr.ip_addr);
    snprintf(s->ref_name, sizeof (s->ref_name), "%s",
             name ? name : UTI_IPToString(&tr.ip_addr));
  }
  s->stratum = tr.stratum;
  s->ref_time = tr.ref_time;
  s->current_correction = tr.current_correction;
  s->last_offset = tr.last_offset;
  s->rms_offset = tr.rms_offset;
  s->freq_ppm = tr.freq_ppm;
  s->resid_freq_ppm = tr.resid_freq_ppm;
  s->skew_ppm = tr.skew_ppm;
  s->root_delay = tr.root_delay;
  s->root_dispersion = tr.root_dispersion;
  s->last_update_interval = tr.last_update_interval;
  s->leap_status = tr.leap_status;

  n = MIN(SRC_ReadNumberOfSources(), CES_MAX_SOURCES);
  for (i = 0; i < n; i++)
    fill_source(i, &now, &s->sources[i]);
  s->n_sources = n;

  taskENTER_CRITICAL(&status_mux);
  status = *s;
  taskEXIT_CRITICAL(&status_mux);
}

/* ================================================== */

static void
status_timeout(void *arg)
{
  refresh_status();
  SCH_AddTimeoutByDelay(STATUS_INTERVAL, status_timeout, NULL);
}

/* ================================================== */

static int
add_source(const char *directive)
{
  CPS_NTP_Source source;
  NSR_Status s;
  char *line, *args;
  uint32_t conf_id;
  int i, pool;

  line = Strdup(directive);
  args = CPS_SplitWord(line);

  if (strcasecmp(line, "pool") == 0) {
    pool = 1;
  } else if (strcasecmp(line, "server") == 0) {
    pool = 0;
  } else {
    LOG(LOGS_ERR, "Unsupported source directive %s", line);
    Free(line);
    return -1;
  }

  memset(&source, 0, sizeof (source));
  if (CPS_ParseNTPSourceAdd(args, &source) != CPS_Success) {
    LOG(LOGS_ERR, "Invalid source directive \"%s\"", directive);
    Free(line);
    return -1;
  }

  /* Groups are told apart by name, so a name is configured at most once.
     (A duplicate address would be rejected by chrony only after the
     asynchronous resolution, with the group already returned.) */
  for (i = 0; i < CES_MAX_SOURCES; i++) {
    if (groups[i].conf_id && strcmp(groups[i].name, source.name) == 0) {
      LOG(LOGS_WARN, "Source %s already configured", source.name);
      Free(line);
      return -1;
    }
  }

  for (i = 0; i < CES_MAX_SOURCES && groups[i].conf_id; i++)
    ;
  if (i == CES_MAX_SOURCES) {
    LOG(LOGS_ERR, "Too many source directives");
    Free(line);
    return -1;
  }

  s = NSR_AddSourceByName(source.name, source.family, source.port, pool,
                          NTP_SERVER, &source.params, &conf_id);
  if (s == NSR_Success || s == NSR_UnresolvedName) {
    groups[i].conf_id = conf_id;
    snprintf(groups[i].name, sizeof (groups[i].name), "%s", source.name);
  }
  Free(line);

  switch (s) {
    case NSR_Success:
      break;
    case NSR_UnresolvedName:
      NSR_ResolveSources();
      break;
    default:
      LOG(LOGS_ERR, "Could not add source \"%s\" : %s", directive,
          NSR_StatusToString(s));
      return -1;
  }

  return conf_id;
}

/* ================================================== */

static void
command_handler(int fd, int event, void *arg)
{
  char buf[16];
  Command *cmd;
  int i;

  while (recv(fd, buf, sizeof (buf), MSG_DONTWAIT) > 0)
    ;

  cmd = pending_command;
  if (!cmd)
    return;
  pending_command = NULL;

  switch (cmd->type) {
    case CMD_ADD_SOURCE:
      cmd->result = add_source(cmd->directive);
      break;
    case CMD_REMOVE_SOURCE:
      NSR_RemoveSourcesById(cmd->group);
      for (i = 0; i < CES_MAX_SOURCES; i++) {
        if (groups[i].conf_id == cmd->group)
          memset(&groups[i], 0, sizeof (groups[i]));
      }
      cmd->result = 0;
      break;
  }

  refresh_status();
  xSemaphoreGive(command_done);
}

/* ================================================== */

static int
run_command(Command *cmd)
{
  if (!initialised)
    return -1;

  xSemaphoreTake(command_lock, portMAX_DELAY);
  pending_command = cmd;
  if (send(wake_fds[1], "", 1, 0) < 0) {
    pending_command = NULL;
    xSemaphoreGive(command_lock);
    return -1;
  }
  xSemaphoreTake(command_done, portMAX_DELAY);
  xSemaphoreGive(command_lock);

  return cmd->result;
}

/* ================================================== */

static void
chronyd_task(void *arg)
{
  unsigned int i;
  char *line;

  LOG_Initialise();
  LOG_SetMinSeverity(LOGS_INFO);

  SCK_PreInitialise();
  DNS_SetAddressFamily(IPADDR_UNSPEC);
  CNF_Initialise(0, 1);

  for (i = 0; i < sizeof (config_lines) / sizeof (config_lines[0]); i++) {
    line = Strdup(config_lines[i]);
    CNF_ParseLine(NULL, i + 1, line);
    Free(line);
  }

  PRV_Initialise(0);
  LCL_Initialise();
  SCH_Initialise();
  SCK_Initialise(IPADDR_UNSPEC);
  SYS_Initialise(1);
  RTC_Initialise(0);
  SRC_Initialise();
  RCL_Initialise();
  KEY_Initialise();
  CAM_Initialise();
  NIO_Initialise();
  NCR_Initialise();
  CNF_SetupAccessRestrictions();
  LDB_Initialise();
  REF_Initialise();
  SST_Initialise();
  NSR_Initialise();
  NSD_Initialise();
  NNS_Initialise();
  NKS_Initialise();
  CLG_Initialise();
  MNL_Initialise();
  TMC_Initialise();
  SMT_Initialise();

  REF_SetMode(REF_ModeNormal);

  /* As at the end of main.c's post_init_ntp_hook() and
     ntp_source_resolving_end() with no configured sources: from here on,
     added sources start as soon as their address is known. */
  NSR_StartSources();
  NSR_AutoStartSources();

  SCH_AddFileHandler(wake_fds[0], SCH_FILE_INPUT, command_handler, NULL);
  refresh_status();
  SCH_AddTimeoutByDelay(STATUS_INTERVAL, status_timeout, NULL);

  initialised = 1;
  LOG(LOGS_INFO, "chronyd version %s starting (%s)", CHRONY_VERSION, CHRONYD_FEATURES);

  SCH_MainLoop();

  LOG_FATAL("chronyd main loop exited");
}

/* ================================================== */

bool
ces_start(void)
{
  if (task)
    return true;

  if (!CES_MountFiles())
    return false;

  command_lock = xSemaphoreCreateMutex();
  command_done = xSemaphoreCreateBinary();
  if (!command_lock || !command_done || pipe(wake_fds) < 0)
    return false;

  if (xTaskCreatePinnedToCore(chronyd_task, "chronyd", TASK_STACK_BYTES, NULL,
                              TASK_PRIORITY, &task, TASK_CORE) != pdPASS) {
    task = NULL;
    return false;
  }

  /* The mailbox is usable once the task has registered its handler. */
  while (!initialised)
    vTaskDelay(pdMS_TO_TICKS(10));

  return true;
}

/* ================================================== */

bool
ces_add_source(const char *directive, uint32_t *group)
{
  Command cmd = { .type = CMD_ADD_SOURCE, .directive = directive };
  int r = run_command(&cmd);

  if (r < 0)
    return false;
  if (group)
    *group = r;
  return true;
}

/* ================================================== */

void
ces_remove_source(uint32_t group)
{
  Command cmd = { .type = CMD_REMOVE_SOURCE, .group = group };

  run_command(&cmd);
}


/* ================================================== */

void
ces_get_status(ces_status_t *out)
{
  taskENTER_CRITICAL(&status_mux);
  *out = status;
  taskEXIT_CRITICAL(&status_mux);
}

/* ================================================== */

const char *
ces_version(void)
{
  return CHRONY_VERSION;
}

/* ================================================== */

bool
ces_valid_host(const char *host)
{
  const char *p;

  if (!host || !*host || strlen(host) >= CES_NAME_LEN)
    return false;

  for (p = host; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' &&
        *p != ':' && *p != '_')
      return false;
  }

  return true;
}

/* ================================================== */

/* pipe() for nameserv_async.c and the command mailbox: two loopback UDP
   sockets connected to each other. Only the "readable" event and
   one-byte writes are needed; select(), read(), write() and close() work
   on lwIP socket descriptors through the VFS. */

int
pipe(int fds[2])
{
  struct sockaddr_in a, b;
  socklen_t len = sizeof (a);
  int r = -1, w = -1;

  memset(&a, 0, sizeof (a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  b = a;

  r = socket(AF_INET, SOCK_DGRAM, 0);
  w = socket(AF_INET, SOCK_DGRAM, 0);
  if (r < 0 || w < 0)
    goto fail;

  if (bind(r, (struct sockaddr *)&a, sizeof (a)) < 0 ||
      getsockname(r, (struct sockaddr *)&a, &len) < 0 ||
      bind(w, (struct sockaddr *)&b, sizeof (b)) < 0 ||
      getsockname(w, (struct sockaddr *)&b, &len) < 0 ||
      connect(w, (struct sockaddr *)&a, sizeof (a)) < 0 ||
      connect(r, (struct sockaddr *)&b, sizeof (b)) < 0)
    goto fail;

  fds[0] = r;
  fds[1] = w;
  return 0;

fail:
  if (r >= 0)
    close(r);
  if (w >= 0)
    close(w);
  errno = EMFILE;
  return -1;
}

/* ================================================== */

/* Whether the default interface has an address that can reach public
   servers in each family: a non-zero IPv4 address, a global IPv6 one.
   chrony's connectability check (a UDP connect()) passes without a route,
   so an AAAA answer on a link-local-only network would otherwise fill
   pool slots with sources that never answer. */

static void
usable_families(int *ipv4, int *ipv6)
{
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info;
  esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  int i, n;

  *ipv4 = *ipv6 = 0;
  if (!netif)
    return;

  if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0)
    *ipv4 = 1;

  n = esp_netif_get_all_ip6(netif, ip6);
  for (i = 0; i < n; i++) {
    if (esp_netif_ip6_get_addr_type(&ip6[i]) == ESP_IP6_ADDR_IS_GLOBAL)
      *ipv6 = 1;
  }
}

/* ================================================== */

/* For AF_UNSPEC, query the usable families (both, if neither is known),
   IPv6 answers before IPv4 as with the RFC 6724 default policy table a
   libc resolver applies. lwIP answers AF_UNSPEC with one family only.
   Each node comes from lwIP's MEMP_NETDB pool and lwip_freeaddrinfo()
   frees node by node, so a concatenated chain is freed correctly. */

int
ces_getaddrinfo(const char *nodename, const char *servname,
                const struct addrinfo *hints, struct addrinfo **res)
{
  struct addrinfo h, *v6 = NULL, *v4 = NULL, **tail;
  int r6 = -1, r4 = -1, ipv4, ipv6;

  if (!hints || hints->ai_family != AF_UNSPEC)
    return lwip_getaddrinfo(nodename, servname, hints, res);

  usable_families(&ipv4, &ipv6);
  if (!ipv4 && !ipv6)
    ipv4 = ipv6 = 1;

  h = *hints;
  if (ipv6) {
    h.ai_family = AF_INET6;
    r6 = lwip_getaddrinfo(nodename, servname, &h, &v6);
  }
  if (ipv4) {
    h.ai_family = AF_INET;
    r4 = lwip_getaddrinfo(nodename, servname, &h, &v4);
  }

  if (r6 != 0 && r4 != 0)
    return ipv6 ? r6 : r4;
  if (r6 != 0)
    v6 = NULL;
  if (r4 != 0)
    v4 = NULL;

  for (tail = &v6; *tail; tail = &(*tail)->ai_next)
    ;
  *tail = v4;

  *res = v6;
  return 0;
}

/* ================================================== */

void
ces_freeaddrinfo(struct addrinfo *ai)
{
  lwip_freeaddrinfo(ai);
}
