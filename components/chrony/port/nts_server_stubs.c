/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2014-2016
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

  NTS server stubs, built in place of nts_ke_server.c and nts_ntp_server.c.
  The device is an NTS client only: it opens no NTP server port and has no
  server keys, so the server halves are never used. The bodies are those of
  the !FEAT_NTS section of upstream stubs.c, which cannot be compiled here
  because the client half needs FEAT_NTS.

  */

#include "config.h"

#include "sysincl.h"

#include "nts_ke_server.h"
#include "nts_ntp_server.h"

/* ================================================== */

void
NNS_Initialise(void)
{
}

/* ================================================== */

void
NNS_Finalise(void)
{
}

/* ================================================== */

int
NNS_CheckRequestAuth(NTP_Packet *packet, NTP_PacketInfo *info, uint32_t *kod)
{
  *kod = 0;
  return 0;
}

/* ================================================== */

int
NNS_GenerateResponseAuth(NTP_Packet *request, NTP_PacketInfo *req_info,
                         NTP_Packet *response, NTP_PacketInfo *res_info,
                         uint32_t kod)
{
  return 0;
}

/* ================================================== */

void
NKS_PreInitialise(uid_t uid, gid_t gid, int scfilter_level)
{
}

/* ================================================== */

void
NKS_Initialise(void)
{
}

/* ================================================== */

void
NKS_Finalise(void)
{
}

/* ================================================== */

void
NKS_DumpKeys(void)
{
}

/* ================================================== */

void
NKS_ReloadKeys(void)
{
}
