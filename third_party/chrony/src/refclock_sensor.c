/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Atanas Vladimirov  2026
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

  OpenBSD hw.sensors timedelta refclock driver.

  Timedelta sensors are provided by serial line time sources attached
  with ldattach(8), such as nmea(4), msts(4) and endrun(4), and by
  dedicated radio clock devices like udcf(4) and mbg(4).  When the
  nmea(4) line discipline is timestamped by a PPS signal on the DCD
  line (ldattach -t dcd), the kernel timestamps the pulse at interrupt
  time and the sensor is accurate to a few microseconds.

  The sensor value is the difference between the system time and the
  reference time in nanoseconds (positive if the system clock is ahead)
  and the sensor timestamp is the system time of the measurement.

  */

#include "config.h"

#include "refclock.h"

#ifdef FEAT_SENSOR

#include "sysincl.h"

#include <sys/sysctl.h>
#include <sys/sensors.h>

#include "logging.h"
#include "memory.h"
#include "util.h"

struct sensor_instance {
  int dev;
  char *name;
  struct timeval last;
};

static int get_sensordev(int dev, struct sensordev *sd)
{
  int mib[3] = { CTL_HW, HW_SENSORS, dev };
  size_t len = sizeof (*sd);

  return sysctl(mib, 3, sd, &len, NULL, 0);
}

static int find_sensordev(const char *name)
{
  struct sensordev sd;
  int dev;

  for (dev = 0; ; dev++) {
    if (get_sensordev(dev, &sd) < 0) {
      if (errno == ENOENT)      /* past the last device */
        break;
      if (errno == ENXIO)       /* empty slot, keep scanning */
        continue;
      return -1;
    }
    if (strcmp(sd.xname, name) == 0)
      return dev;
  }

  return -1;
}

/* Read the timedelta sensor after verifying the device name and presence of
   the sensor, as the kernel reuses indices of detached devices and a
   different device could take the place of the configured device */
static int read_timedelta(int dev, const char *name, struct sensor *s)
{
  int mib[5] = { CTL_HW, HW_SENSORS, dev, SENSOR_TIMEDELTA, 0 };
  size_t len = sizeof (*s);
  struct sensordev sd;

  if (dev < 0 || get_sensordev(dev, &sd) < 0 ||
      strcmp(sd.xname, name) != 0 || sd.maxnumt[SENSOR_TIMEDELTA] < 1)
    return -1;

  return sysctl(mib, 5, s, &len, NULL, 0);
}

static int sensor_initialise(RCL_Instance instance)
{
  struct sensor_instance *sensor;
  struct sensor s;
  char *name;
  int dev;

  RCL_CheckDriverOptions(instance, NULL);

  name = RCL_GetDriverParameter(instance);

  /* The device may not be attached yet (e.g. chronyd started at boot
     before ldattach(8)) - do not fail, wait for it to appear */
  dev = find_sensordev(name);
  if (dev < 0 || read_timedelta(dev, name, &s) < 0) {
    LOG(LOGS_WARN, "Could not find timedelta sensor of device %s (waiting for it to appear)",
        name);
    dev = -1;
  }

  sensor = MallocNew(struct sensor_instance);
  sensor->dev = dev;
  sensor->name = Strdup(name);
  sensor->last.tv_sec = 0;
  sensor->last.tv_usec = 0;

  RCL_SetDriverData(instance, sensor);
  return 1;
}

static void sensor_finalise(RCL_Instance instance)
{
  struct sensor_instance *sensor;

  sensor = (struct sensor_instance *)RCL_GetDriverData(instance);
  Free(sensor->name);
  Free(sensor);
}

static int sensor_poll(RCL_Instance instance)
{
  struct sensor_instance *sensor;
  struct timespec sys_ts, ref_ts;
  struct sensor s;
  double offset;
  int dev;

  sensor = (struct sensor_instance *)RCL_GetDriverData(instance);

  if (read_timedelta(sensor->dev, sensor->name, &s) < 0) {
    /* the device may not be attached yet, or was reattached with
       a different index */
    dev = find_sensordev(sensor->name);
    if (dev >= 0 && sensor->dev < 0)
      LOG(LOGS_INFO, "Found sensor device %s", sensor->name);
    sensor->dev = dev;
    DEBUG_LOG("Could not read sensor %s", sensor->name);
    return 0;
  }

  /* Ignore the sensor while it is invalid or degraded */
  if (s.flags & SENSOR_FINVALID || s.status != SENSOR_S_OK) {
    DEBUG_LOG("Ignoring sensor %s flags=%x status=%d",
              sensor->name, (unsigned int)s.flags, (int)s.status);
    return 0;
  }

  /* Wait for a new measurement */
  if (s.tv.tv_sec == sensor->last.tv_sec &&
      s.tv.tv_usec == sensor->last.tv_usec)
    return 0;
  sensor->last = s.tv;

  if (!UTI_IsTimevalNormal(&s.tv)) {
    DEBUG_LOG("Invalid timestamp from sensor %s", sensor->name);
    return 0;
  }

  UTI_TimevalToTimespec(&s.tv, &sys_ts);

  /* The sensor value is system time minus reference time in nanoseconds */
  offset = -(double)s.value / 1.0e9;

  if (!UTI_IsTimeOffsetSane(&sys_ts, offset))
    return 0;

  UTI_AddDoubleToTimespec(&sys_ts, offset, &ref_ts);

  DEBUG_LOG("SENSOR %s sample offset=%.9f", sensor->name, offset);

  return RCL_AddSample(instance, &sys_ts, &ref_ts, LEAP_Normal, 1);
}

RefclockDriver RCL_SENSOR_driver = {
  sensor_initialise,
  sensor_finalise,
  sensor_poll
};

#else

RefclockDriver RCL_SENSOR_driver = { NULL, NULL, NULL };

#endif
