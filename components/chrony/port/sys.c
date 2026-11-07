/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  System driver dispatch for ESP-IDF. Built in place of upstream sys.c,
  whose platform list ends in "#error Unknown system". There is one
  process, no root and no scheduler policy to set, so the privilege and
  scheduling entry points are unreachable with this port's main loop.

  */

#include "config.h"

#include "sysincl.h"

#include "sys.h"
#include "sys_null.h"
#include "sys_esp32.h"
#include "logging.h"

/* ================================================== */

static int null_driver;

/* ================================================== */

void
SYS_Initialise(int clock_control)
{
  null_driver = !clock_control;
  if (null_driver) {
    SYS_Null_Initialise();
    return;
  }
  SYS_ESP32_Initialise();
}

/* ================================================== */

void
SYS_Finalise(void)
{
  if (null_driver) {
    SYS_Null_Finalise();
    return;
  }
  SYS_ESP32_Finalise();
}

/* ================================================== */

void SYS_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context)
{
  LOG_FATAL("dropping root privileges not supported");
}

/* ================================================== */

void SYS_EnableSystemCallFilter(int level, SYS_ProcessContext context)
{
  LOG_FATAL("system call filter not supported");
}

/* ================================================== */

void SYS_SetScheduler(int SchedPriority)
{
  LOG_FATAL("scheduler priority setting not supported");
}

/* ================================================== */

void SYS_LockMemory(void)
{
  LOG_FATAL("memory locking not supported");
}
