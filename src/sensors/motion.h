/****************************************************************************
 * pnut-os/src/sensors/motion.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SENSORS_MOTION_H
#define __PNUT_OS_SENSORS_MOTION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>
#include <pnut/tz.h>

#include "pnut/motion.pnut.h"
#include "pnut/motion.settings.h"
#include "pnut/motion.topics.h"
#include "pnut/time.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MOTION_FOLLOWS     2      /* FollowSteps and FollowActivity streams,
                                   * each */

#define MOTION_WAITING     4      /* Calls waiting for the hub's start */
#define MOTION_SAMPLES     3      /* Accelerometer readings for a facing */

/* An activity event's value: bit k for MotionActivityKind k started,
 * bit 16 + k for k ended
 */

#define MOTION_STARTED(k)  (1u << (k))
#define MOTION_ENDED(k)    (1u << (16 + (k)))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The hub's sensors Motion uses */

enum motion_sensor_e
{
  MOTION_SENSOR_STEPS = 0,        /* value: steps since the hub started */
  MOTION_SENSOR_ACTIVITY,         /* value: MOTION_STARTED/ENDED bits */
  MOTION_SENSOR_PICKUP,           /* Picked up */
  MOTION_SENSOR_GLANCE,           /* Lifted to be looked at */
  MOTION_SENSOR_STILL,            /* Became still */
  MOTION_SENSOR_MOVING,           /* Started moving */
  MOTION_SENSOR_TILT,             /* Its angle changed */
  MOTION_SENSOR_ACCEL,            /* x, y, z: m/s^2; +z out of the screen */
  MOTION_NSENSORS
};

/* An event of a sensor */

struct motion_event_s
{
  uint32_t value;
  float x;                        /* +z out of the screen; x and y as the
                                   * hub reads them: only their size is
                                   * relied on */
  float y;
  float z;
};

/* The hub: NuttX's uORB topics on the device (hub.c), a fake in the
 * tests.  open may take seconds (the hub's firmware upload): it is called
 * on a worker; the others on the loop.
 */

struct motion_hub_ops_s
{
  /* Open a sensor, its events held up to latency_ms (0: none): a
   * descriptor readable while events wait, or a negated errno value,
   * -ENODEV when the device has no such sensor
   */

  CODE int (*open)(FAR void *arg, int sensor, uint32_t latency_ms);
  CODE void (*close)(FAR void *arg, int sensor, int fd);

  /* The next event: 1, 0 when none waits, or a negated errno value */

  CODE int (*read)(FAR void *arg, int sensor, int fd,
                   FAR struct motion_event_s *ev);

  /* How long its events may be held from now on */

  CODE int (*batch)(FAR void *arg, int sensor, int fd, uint32_t latency_ms);
};

struct motion_config_s
{
  FAR const struct motion_hub_ops_s *ops;
  FAR void *arg;
  FAR const char *dir;            /* Today's steps, kept */
  uint32_t save_ms;               /* Between saves of a changed count */
  uint32_t retry_ms;              /* After the hub failed, at first; 0 for
                                   * the default */
  CODE time_t (*now)(void);       /* The wall clock; NULL: time() */
};

/* A stream following steps or the activity */

struct motion_follow_s
{
  bool used;
  bool pending;                   /* A change not pushed: no credit */
  struct pnut_stream_s stream;
};

/* A call that came before the hub started */

struct motion_waiting_s
{
  uint8_t method;                 /* 0: none */
  struct pnut_request_s req;
};

/* Today's steps, as kept */

struct motion_steps_s
{
  int32_t day;                    /* Local days since 1970; -1: none */
  uint32_t count;
  uint32_t hub_last;              /* The hub's count, seen last */
  int64_t changed;                /* The wall clock, when count changed */
};

struct motion_s
{
  struct pnut_module_s module;    /* First */
  struct motion_config_s config;

  struct pnut_motion_server_s server;
  bool serving;
  struct pnut_motion_settings_s settings;
  bool settings_open;
  FAR struct pnut_publisher_s *topic;
  FAR struct pnut_publisher_s *gestures;
  struct pnut_time_change_reader_s time_reader;
  bool time_open;
  FAR struct pnut_timer_s *save_timer;
  FAR struct pnut_timer_s *sample_timer;

  /* The hub failed: nothing is opened until retry_timer fires, after
   * retry_ms, which doubles each time
   */

  FAR struct pnut_timer_s *retry_timer;
  uint32_t retry_ms;

  /* The hub: each sensor's descriptor (-1: closed), which ones it lacks,
   * whether it was looked at yet
   */

  int fd[MOTION_NSENSORS];
  uint32_t absent;                /* Bits of enum motion_sensor_e */
  uint32_t latency;               /* The step counter's, ms */
  bool started;                   /* Its sensors looked at, steps read */
  bool stopping;

  /* What it tells */

  uint8_t movement;               /* MotionMovement */
  uint8_t facing;                 /* MotionFacing */
  uint8_t samples;                /* Accelerometer readings taken: the
                                   * accelerometer is on while fewer than
                                   * MOTION_SAMPLES */
  float sum[3];
  uint8_t activity;               /* MotionActivityKind */
  int64_t activity_since;

  /* Steps: kept, and the time zone's offset to tell today */

  struct motion_steps_s steps;
  bool dirty;                     /* Not saved */
  bool clock_set;
  int32_t offset;                 /* Seconds east of UTC */
  struct pnut_tz_s tz;            /* The zone's rule, when tz_ok: Time
                                   * announces no summer time's start */
  bool tz_ok;

  struct motion_follow_s follow_steps[MOTION_FOLLOWS];
  struct motion_follow_s follow_activity[MOTION_FOLLOWS];
  struct motion_waiting_s waiting[MOTION_WAITING];

  /* A job on the worker: opening sensors (and reading the steps kept, the
   * first time), or saving the steps
   */

  bool job;
  bool job_save;                  /* The job saves */
  bool job_load;                  /* The job reads the steps kept */
  uint32_t job_open;              /* Bits: to open */
  uint32_t job_latency;           /* The step counter's, ms */
  int job_fd[MOTION_NSENSORS];
  uint32_t job_absent;
  bool job_loaded;
  struct motion_steps_s job_steps;
  int job_ret;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's uORB topics of the BHI260AP driver (hub.c) */

extern const struct motion_hub_ops_s g_motion_hub;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: motion_init, motion_deinit
 *
 * Description:
 *   Set Motion up, as a module to add to a loop; after the loop is
 *   destroyed, close what it kept open.
 *
 ****************************************************************************/

void motion_init(FAR struct motion_s *mo,
                 FAR const struct motion_config_s *config);
void motion_deinit(FAR struct motion_s *mo);

/****************************************************************************
 * Name: motion_facing
 *
 * Description:
 *   Which way the screen faces, from the accelerometer's average reading
 *   while still (m/s^2): MotionFacing.
 *
 ****************************************************************************/

int motion_facing(float x, float y, float z);

#endif /* __PNUT_OS_SENSORS_MOTION_H */
