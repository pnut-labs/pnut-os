/****************************************************************************
 * pnut-os/src/sensors/hub.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Motion's hub on the device: the uORB topics of NuttX's BHI260AP driver,
 * instance 0.  Opening a topic switches its sensor on; the first one
 * uploads the hub's firmware.  A sensor the firmware lacks fails to open
 * with ENODEV; a device without the hub has no topics (ENOENT).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/sensors/bhi260ap.h>
#include <nuttx/sensors/ioctl.h>
#include <nuttx/uorb.h>

#include "motion.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HUB_ACCEL_US  100000      /* The accelerometer, read for a facing */
#define HUB_STEPS_US  1000000     /* The step counter: at most each second */
#define KIND(k)       PNUT_MOTION_ACTIVITY_KIND_MOTION_ACTIVITY_##k

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Each sensor's topic: MOTION_DETECT's NuttX names significant_motion */

static FAR const char * const g_hub_paths[MOTION_NSENSORS] =
{
  "/dev/uorb/sensor_step_counter0",
  "/dev/uorb/sensor_" BHI260AP_ACTIVITY "0",
  "/dev/uorb/sensor_pickup_gesture0",
  "/dev/uorb/sensor_glance_gesture0",
  "/dev/uorb/sensor_" BHI260AP_STATIONARY "0",
  "/dev/uorb/sensor_significant_motion0",
  "/dev/uorb/sensor_tilt_detector0",
  "/dev/uorb/sensor_accel0",
};

/* The hub's activities as Motion's kinds */

static const uint8_t g_hub_kinds[][2] =
{
  { BHI260AP_ACTIVITY_STILL,   KIND(STILL)   },
  { BHI260AP_ACTIVITY_WALKING, KIND(WALKING) },
  { BHI260AP_ACTIVITY_RUNNING, KIND(RUNNING) },
  { BHI260AP_ACTIVITY_BICYCLE, KIND(BICYCLE) },
  { BHI260AP_ACTIVITY_VEHICLE, KIND(VEHICLE) },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int hub_batch(FAR void *arg, int sensor, int fd, uint32_t latency_ms)
{
  uint32_t us = latency_ms * 1000;

  return ioctl(fd, SNIOC_BATCH, (unsigned long)&us) < 0 ? -errno : OK;
}

static int hub_open(FAR void *arg, int sensor, uint32_t latency_ms)
{
  int fd;

  fd = open(g_hub_paths[sensor], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    {
      return errno == ENOENT ? -ENODEV : -errno;
    }

  /* uORB passes a reader's batch latency on only once it set an
   * interval
   */

  if (sensor == MOTION_SENSOR_ACCEL || sensor == MOTION_SENSOR_STEPS)
    {
      uint32_t us = sensor == MOTION_SENSOR_ACCEL ? HUB_ACCEL_US :
                    HUB_STEPS_US;

      ioctl(fd, SNIOC_SET_INTERVAL, (unsigned long)&us);
    }

  if (latency_ms > 0)
    {
      hub_batch(arg, sensor, fd, latency_ms);
    }

  return fd;
}

static void hub_close(FAR void *arg, int sensor, int fd)
{
  close(fd);
}

static uint32_t hub_activity(uint32_t bits)
{
  uint32_t value = 0;
  size_t i;

  for (i = 0; i < sizeof(g_hub_kinds) / sizeof(g_hub_kinds[0]); i++)
    {
      if ((bits & BHI260AP_ACTIVITY_STARTED(g_hub_kinds[i][0])) != 0)
        {
          value |= MOTION_STARTED(g_hub_kinds[i][1]);
        }

      if ((bits & BHI260AP_ACTIVITY_ENDED(g_hub_kinds[i][0])) != 0)
        {
          value |= MOTION_ENDED(g_hub_kinds[i][1]);
        }
    }

  return value;
}

static int hub_read(FAR void *arg, int sensor, int fd,
                    FAR struct motion_event_s *ev)
{
  union
  {
    struct sensor_step_counter steps;
    struct sensor_accel accel;
    struct sensor_event event;
  } u;

  size_t size = sensor == MOTION_SENSOR_STEPS ? sizeof(u.steps) :
                sensor == MOTION_SENSOR_ACCEL ? sizeof(u.accel) :
                sizeof(u.event);
  ssize_t n;

  /* A topic with nothing new says ENODATA */

  n = read(fd, &u, size);
  if (n < 0)
    {
      return errno == EAGAIN || errno == ENODATA ? 0 : -errno;
    }

  if ((size_t)n != size)
    {
      return 0;
    }

  ev->value = 1;
  ev->x     = 0;
  ev->y     = 0;
  ev->z     = 0;
  switch (sensor)
    {
      case MOTION_SENSOR_STEPS:
        ev->value = u.steps.steps;
        break;

      case MOTION_SENSOR_ACTIVITY:
        ev->value = hub_activity(u.event.event);
        break;

      case MOTION_SENSOR_ACCEL:

        /* The BHI260AP's z axis on the T-Deck Max points out of the back:
         * lying screen up, it reads about -1 g (seen on the phone).  Of the
         * three signs only z's matters to the facing; x and y are as the
         * chip reads them, so one of them is mirrored against motion.h's
         * frame (which one is not known).
         */

        ev->x = u.accel.x;
        ev->y = u.accel.y;
        ev->z = -u.accel.z;
        break;
    }

  return 1;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct motion_hub_ops_s g_motion_hub =
{
  hub_open,
  hub_close,
  hub_read,
  hub_batch
};
