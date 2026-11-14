/****************************************************************************
 * pnut-os/src/sensors/motion.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Motion (RFC 0036): the motion sensor hub's sensors, and what they mean.
 * The hub works everything out and sends only changes: still or moving;
 * when the device became still or its angle changed, a few accelerometer
 * readings tell which way it faces, and the accelerometer is off again;
 * steps, held in the hub for a minute unless someone follows them;
 * activity; the gestures that wake the screen.  A sensor is open only
 * while its setting wants it (motion.steps: steps and activity;
 * motion.wake: the gestures).
 *
 * Opening the first sensor uploads the hub's firmware, seconds over I2C,
 * so sensors are opened on the worker; the first time, the worker also
 * reads today's steps as kept, and the calls that come meanwhile wait.
 * Today's steps are kept in config.dir: the day, the count, the hub's
 * count seen last (a count lower than it means the hub started again),
 * saved every config.save_ms while it changes and when Motion stops.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/worker.h>

#include "motion.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MOTION_SERVICE     "motion"
#define MOTION_SAMPLE_MS   2000     /* For the accelerometer's readings */
#define MOTION_LATENCY     60000    /* ms the hub holds steps, unfollowed */
#define MOTION_FOLLOWED    2000     /* And followed */
#define MOTION_RETRY_MS    30000    /* After the hub failed, at first */
#define MOTION_RETRY_MAX   600000   /* And at most */
#define MOTION_G           9.80665f
#define MOTION_PULL        (0.6f * MOTION_G)  /* Along the facing's axis */

#define MOTION_FILE        "steps"
#define MOTION_MAGIC       0x534d4e50 /* "PNMS" */
#define MOTION_VERSION     1
#define MOTION_SIZE        28

#define MOTION_BIT(s)      (1u << (s))
#define MOTION_ALL         (MOTION_BIT(MOTION_NSENSORS) - 1)

/* The kinds as the generated code names them */

#define ACT(k)             PNUT_MOTION_ACTIVITY_KIND_MOTION_ACTIVITY_##k
#define MOVEMENT(k)        PNUT_MOTION_MOVEMENT_MOTION_MOVEMENT_##k
#define FACING(k)          PNUT_MOTION_FACING_MOTION_FACING_##k

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void motion_reconcile(FAR struct motion_s *mo);
static void motion_failed(FAR struct motion_s *mo);
static void motion_answer(FAR struct motion_s *mo, uint8_t method,
                          FAR const struct pnut_request_s *req);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t motion_mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static time_t motion_wall(FAR struct motion_s *mo)
{
  return mo->config.now != NULL ? mo->config.now() : time(NULL);
}

/* Local days since 1970, or -1 while the clock has no source */

static int32_t motion_today(FAR struct motion_s *mo)
{
  time_t now = motion_wall(mo);
  int32_t offset = mo->offset;
  int64_t local;
  struct tm tm;

  if (!mo->clock_set)
    {
      return -1;
    }

  /* The zone's rule, which knows when summer time starts and ends; Time
   * announces an offset only when the zone or the clock changes
   */

  if (mo->tz_ok)
    {
      pnut_tz_local(&mo->tz, now, &tm, &offset);
    }

  local = (int64_t)now + offset;
  return (int32_t)(local >= 0 ? local / 86400 : (local - 86399) / 86400);
}

static bool motion_has(FAR struct motion_s *mo, int sensor)
{
  return (mo->absent & MOTION_BIT(sensor)) == 0;
}

/****************************************************************************
 * Name: motion_wanted
 *
 * Description:
 *   The sensors to have open: all of them until the hub was looked at;
 *   then stillness and the angle always, steps and activity, gestures as
 *   the settings say, the accelerometer while it is read.
 *
 ****************************************************************************/

static uint32_t motion_wanted(FAR struct motion_s *mo)
{
  FAR const pnut_motion_settings_t *v = &mo->settings.values;
  uint32_t want;

  if (mo->stopping)
    {
      return 0;
    }

  if (!mo->started)
    {
      return MOTION_ALL;
    }

  want = MOTION_BIT(MOTION_SENSOR_STILL) | MOTION_BIT(MOTION_SENSOR_MOVING) |
         MOTION_BIT(MOTION_SENSOR_TILT);
  if (v->steps)
    {
      want |= MOTION_BIT(MOTION_SENSOR_STEPS) |
              MOTION_BIT(MOTION_SENSOR_ACTIVITY);
    }

  if (v->wake)
    {
      want |= MOTION_BIT(MOTION_SENSOR_PICKUP) |
              MOTION_BIT(MOTION_SENSOR_GLANCE);
    }

  if (mo->samples < MOTION_SAMPLES)
    {
      want |= MOTION_BIT(MOTION_SENSOR_ACCEL);
    }

  return want & ~mo->absent;
}

static bool motion_followed(FAR struct motion_follow_s *follows)
{
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      if (follows[i].used)
        {
          return true;
        }
    }

  return false;
}

static uint32_t motion_latency(FAR struct motion_s *mo)
{
  return motion_followed(mo->follow_steps) ? MOTION_FOLLOWED :
                                             MOTION_LATENCY;
}

/****************************************************************************
 * Name: motion_publish
 ****************************************************************************/

static void motion_publish(FAR struct motion_s *mo)
{
  pnut_motion_info_t msg;

  if (mo->topic == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.movement = mo->movement;
  msg.facing   = mo->facing;
  pnut_motion_info_publish(mo->topic, &msg);
}

/****************************************************************************
 * Name: motion_steps_now, motion_push_steps, motion_push_activity
 ****************************************************************************/

static void motion_steps_now(FAR struct motion_s *mo,
                             FAR pnut_motion_steps_t *out)
{
  int32_t today = motion_today(mo);

  memset(out, 0, sizeof(*out));
  if (today < 0 || mo->steps.day == today)
    {
      out->count   = mo->steps.count;
      out->changed = mo->steps.changed;
    }
}

/* The present count or activity to one follower; one without credit
 * gets the latest when it has some again
 */

static void motion_push(FAR struct motion_s *mo,
                        FAR struct motion_follow_s *f, bool steps)
{
  int ret;

  if (!f->used)
    {
      return;
    }

  if (steps)
    {
      pnut_motion_steps_t item;

      motion_steps_now(mo, &item);
      ret = pnut_motion_follow_steps_push(&mo->server, &f->stream, &item);
    }
  else
    {
      pnut_motion_activity_t item;

      memset(&item, 0, sizeof(item));
      item.kind  = mo->activity;
      item.since = mo->activity_since;
      ret = pnut_motion_follow_activity_push(&mo->server, &f->stream,
                                             &item);
    }

  f->pending = ret == -EAGAIN || ret == -EBUSY;
  if (ret == -ENOTCONN)
    {
      f->used = false;
    }
}

static void motion_push_steps(FAR struct motion_s *mo)
{
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      motion_push(mo, &mo->follow_steps[i], true);
    }
}

static void motion_push_activity(FAR struct motion_s *mo)
{
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      motion_push(mo, &mo->follow_activity[i], false);
    }
}

/****************************************************************************
 * Name: motion_rollover
 *
 * Description:
 *   A new local day: today's count starts again.  The clock must have a
 *   source, else the count goes on; a count of no day yet (the first
 *   start) is today's.
 *
 ****************************************************************************/

static bool motion_rollover(FAR struct motion_s *mo)
{
  int32_t today = motion_today(mo);

  if (today < 0 || today == mo->steps.day)
    {
      return false;
    }

  if (mo->steps.day < 0)
    {
      mo->steps.day = today;
      mo->dirty     = true;
      return false;
    }

  mo->steps.day     = today;
  mo->steps.count   = 0;
  mo->steps.changed = 0;
  mo->dirty         = true;
  return true;
}

/****************************************************************************
 * Name: motion_count
 *
 * Description:
 *   The hub's count: the steps since the one seen last.  The first since
 *   Motion started is compared with the count kept: lower, the hub started
 *   again (its firmware uploaded anew) and counts from 0.
 *
 ****************************************************************************/

static void motion_count(FAR struct motion_s *mo, uint32_t hub)
{
  uint32_t delta;

  delta = hub >= mo->steps.hub_last ? hub - mo->steps.hub_last : hub;
  mo->steps.hub_last = hub;
  mo->dirty          = true;
  motion_rollover(mo);
  if (delta > 0)
    {
      mo->steps.count  += delta;
      mo->steps.changed = motion_wall(mo);
      motion_push_steps(mo);
    }
}

/****************************************************************************
 * Name: motion_activity_event
 *
 * Description:
 *   Activities started and ended: the most particular one started is the
 *   owner's now (a vehicle over walking); the present one ended with none
 *   started is unknown.
 *
 ****************************************************************************/

static void motion_activity_event(FAR struct motion_s *mo, uint32_t bits)
{
  uint8_t kind = mo->activity;
  int k;

  if ((bits & 0xffff) != 0)
    {
      for (k = ACT(VEHICLE); k >= ACT(STILL); k--)
        {
          if ((bits & MOTION_STARTED(k)) != 0)
            {
              kind = k;
              break;
            }
        }
    }
  else if ((bits & MOTION_ENDED(mo->activity)) != 0)
    {
      kind = ACT(UNKNOWN);
    }

  if (kind != mo->activity)
    {
      mo->activity       = kind;
      mo->activity_since = motion_wall(mo);
      motion_push_activity(mo);
    }
}

/****************************************************************************
 * Name: motion_sample, motion_sampled, motion_sample_late
 *
 * Description:
 *   Read the accelerometer a few times to tell the facing; given up when
 *   it says nothing in MOTION_SAMPLE_MS.
 *
 ****************************************************************************/

static void motion_sample_late(FAR struct pnut_loop_s *loop,
                               FAR struct pnut_timer_s *timer,
                               FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  mo->sample_timer = NULL;
  if (mo->samples < MOTION_SAMPLES)
    {
      pnut_log(&mo->module, PNUT_LOG_DEBUG, "No accelerometer readings");
      mo->samples = MOTION_SAMPLES;
      motion_reconcile(mo);
    }
}

static void motion_sample(FAR struct motion_s *mo)
{
  if (!motion_has(mo, MOTION_SENSOR_ACCEL) || mo->stopping)
    {
      return;
    }

  mo->samples = 0;
  memset(mo->sum, 0, sizeof(mo->sum));
  if (mo->sample_timer != NULL)
    {
      pnut_timer_cancel(mo->module.loop, mo->sample_timer);
      mo->sample_timer = NULL;
    }

  pnut_timer_start(mo->module.loop, MOTION_SAMPLE_MS, 0, motion_sample_late,
                   mo, &mo->sample_timer);
  motion_reconcile(mo);
}

static void motion_sampled(FAR struct motion_s *mo,
                           FAR const struct motion_event_s *ev)
{
  int facing;

  if (mo->samples >= MOTION_SAMPLES)
    {
      return;                     /* Read before it was closed */
    }

  mo->sum[0] += ev->x;
  mo->sum[1] += ev->y;
  mo->sum[2] += ev->z;
  if (++mo->samples < MOTION_SAMPLES)
    {
      return;
    }

  if (mo->sample_timer != NULL)
    {
      pnut_timer_cancel(mo->module.loop, mo->sample_timer);
      mo->sample_timer = NULL;
    }

  facing = motion_facing(mo->sum[0] / MOTION_SAMPLES,
                         mo->sum[1] / MOTION_SAMPLES,
                         mo->sum[2] / MOTION_SAMPLES);
  if (facing != mo->facing)
    {
      mo->facing = facing;
      motion_publish(mo);
    }

  motion_reconcile(mo);
}

/****************************************************************************
 * Name: motion_gesture
 ****************************************************************************/

static void motion_gesture(FAR struct motion_s *mo, int kind)
{
  pnut_motion_gesture_t msg;

  if (mo->gestures == NULL || !mo->settings.values.wake)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.kind     = kind;
  msg.taken_ms = motion_mono_ms();
  pnut_motion_gesture_publish(mo->gestures, &msg);
}

/****************************************************************************
 * Name: motion_event
 ****************************************************************************/

static void motion_event(FAR struct motion_s *mo, int sensor,
                         FAR const struct motion_event_s *ev)
{
  switch (sensor)
    {
      case MOTION_SENSOR_STEPS:
        motion_count(mo, ev->value);
        break;

      case MOTION_SENSOR_ACTIVITY:
        motion_activity_event(mo, ev->value);
        break;

      case MOTION_SENSOR_PICKUP:
        motion_gesture(mo, PNUT_MOTION_GESTURE_KIND_PICKED_UP);
        break;

      case MOTION_SENSOR_GLANCE:
        motion_gesture(mo, PNUT_MOTION_GESTURE_KIND_GLANCE);
        break;

      case MOTION_SENSOR_STILL:
      case MOTION_SENSOR_MOVING:
        {
          uint8_t movement = sensor == MOTION_SENSOR_STILL ?
                             MOVEMENT(STILL) : MOVEMENT(MOVING);

          if (movement != mo->movement)
            {
              mo->movement = movement;
              motion_publish(mo);
            }

          if (sensor == MOTION_SENSOR_STILL)
            {
              motion_sample(mo);
            }
        }
        break;

      case MOTION_SENSOR_TILT:
        motion_sample(mo);
        break;

      case MOTION_SENSOR_ACCEL:
        motion_sampled(mo, ev);
        break;
    }
}

/****************************************************************************
 * Name: motion_close_sensor, motion_readable
 ****************************************************************************/

static void motion_close_sensor(FAR struct motion_s *mo, int sensor)
{
  if (mo->fd[sensor] >= 0)
    {
      pnut_loop_unwatch(mo->module.loop, mo->fd[sensor]);
      mo->config.ops->close(mo->config.arg, sensor, mo->fd[sensor]);
      mo->fd[sensor] = -1;
    }
}

static void motion_readable(FAR struct pnut_loop_s *loop, int fd,
                            uint32_t events, FAR void *arg)
{
  FAR struct motion_s *mo = arg;
  struct motion_event_s ev;
  int sensor;
  int ret;

  for (sensor = 0; sensor < MOTION_NSENSORS; sensor++)
    {
      if (mo->fd[sensor] == fd)
        {
          break;
        }
    }

  if (sensor == MOTION_NSENSORS)
    {
      return;
    }

  /* An event may close it (the accelerometer's last reading) */

  while (mo->fd[sensor] == fd &&
         (ret = mo->config.ops->read(mo->config.arg, sensor, fd, &ev)) > 0)
    {
      motion_event(mo, sensor, &ev);
    }

  if (mo->fd[sensor] == fd && ret < 0)
    {
      pnut_log(&mo->module, PNUT_LOG_WARNING, "Sensor %d failed: %d",
               sensor, ret);
      motion_close_sensor(mo, sensor);
      motion_failed(mo);
      motion_reconcile(mo);
    }
}

/****************************************************************************
 * Name: motion_load, motion_store
 *
 * Description:
 *   Read today's steps as kept, or write them, on the worker or after the
 *   loop is gone.
 *
 ****************************************************************************/

static bool motion_load(FAR const char *dir, FAR struct motion_steps_s *s)
{
  uint8_t b[MOTION_SIZE];
  char path[96];
  ssize_t n;
  int fd;

  snprintf(path, sizeof(path), "%s/%s", dir, MOTION_FILE);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return false;
    }

  n = read(fd, b, sizeof(b));
  close(fd);
  if (n != MOTION_SIZE || get32(b) != MOTION_MAGIC ||
      b[4] != MOTION_VERSION)
    {
      return false;
    }

  s->day      = (int32_t)get32(&b[8]);
  s->count    = get32(&b[12]);
  s->hub_last = get32(&b[16]);
  s->changed  = (int64_t)(get32(&b[20]) | ((uint64_t)get32(&b[24]) << 32));
  return true;
}

static int motion_store(FAR const char *dir,
                        FAR const struct motion_steps_s *s)
{
  uint8_t b[MOTION_SIZE];
  char path[96];
  char tmp[100];
  int ret;
  int fd;

  memset(b, 0, sizeof(b));
  put32(&b[0], MOTION_MAGIC);
  b[4] = MOTION_VERSION;
  put32(&b[8], (uint32_t)s->day);
  put32(&b[12], s->count);
  put32(&b[16], s->hub_last);
  put32(&b[20], (uint32_t)s->changed);
  put32(&b[24], (uint32_t)((uint64_t)s->changed >> 32));

  mkdir(dir, 0700);
  snprintf(path, sizeof(path), "%s/%s", dir, MOTION_FILE);
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = write(fd, b, sizeof(b)) == sizeof(b) ? OK : -EIO;
  if (fsync(fd) < 0 && ret >= 0)
    {
      ret = -errno;
    }

  close(fd);
  if (ret >= 0 && rename(tmp, path) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(tmp);
    }

  return ret;
}

/****************************************************************************
 * Name: motion_open_run, motion_save_run, motion_job_done
 ****************************************************************************/

static void motion_open_run(FAR void *arg)
{
  FAR struct motion_s *mo = arg;
  int s;

  if (mo->job_load)
    {
      mo->job_loaded = motion_load(mo->config.dir, &mo->job_steps);
    }

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      mo->job_fd[s] = -1;
    }

  /* A sensor the hub's image lacks is absent for good; any other failure
   * (the hub not booting, the bus) stops here, to be tried again later
   */

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      int ret;

      if ((mo->job_open & MOTION_BIT(s)) == 0)
        {
          continue;
        }

      ret = mo->config.ops->open(mo->config.arg, s,
                                 s == MOTION_SENSOR_STEPS ?
                                 mo->job_latency : 0);
      if (ret >= 0)
        {
          mo->job_fd[s] = ret;
        }
      else if (ret == -ENODEV)
        {
          mo->job_absent |= MOTION_BIT(s);
        }
      else
        {
          mo->job_ret = ret;
          break;
        }
    }
}

static void motion_save_run(FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  mo->job_ret = motion_store(mo->config.dir, &mo->job_steps);
}

static void motion_started(FAR struct motion_s *mo)
{
  int i;

  if (mo->job_loaded)
    {
      mo->steps = mo->job_steps;
    }

  mo->started = true;
  motion_rollover(mo);
  pnut_log(&mo->module, PNUT_LOG_INFO,
           "The hub lacks sensors 0x%02x (steps %s, today %lu)",
           (unsigned int)mo->absent, mo->job_loaded ? "kept" : "new",
           (unsigned long)mo->steps.count);

  /* The facing, once (the accelerometer is open: everything was) */

  if (motion_has(mo, MOTION_SENSOR_ACCEL))
    {
      motion_sample(mo);
    }

  /* The calls that waited */

  for (i = 0; i < MOTION_WAITING; i++)
    {
      FAR struct motion_waiting_s *w = &mo->waiting[i];

      if (w->method != 0)
        {
          uint8_t method = w->method;

          w->method = 0;
          motion_answer(mo, method, &w->req);
        }
    }
}

static void motion_job_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct motion_s *mo = arg;
  int ret;
  int s;

  mo->job = false;
  if (mo->job_save)
    {
      mo->job_save = false;
      if (mo->job_ret < 0)
        {
          pnut_log(&mo->module, PNUT_LOG_WARNING, "Steps not kept: %d",
                   mo->job_ret);
          mo->dirty = true;
        }

      motion_reconcile(mo);
      return;
    }

  if (mo->job_ret < 0)
    {
      pnut_log(&mo->module, PNUT_LOG_WARNING, "The hub failed: %d",
               mo->job_ret);
      motion_failed(mo);
    }
  else
    {
      mo->retry_ms = 0;
    }

  mo->absent |= mo->job_absent;
  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      int fd = mo->job_fd[s];

      mo->job_fd[s] = -1;
      if (fd < 0)
        {
          continue;
        }

      if (mo->stopping || mo->fd[s] >= 0)
        {
          mo->config.ops->close(mo->config.arg, s, fd);
          continue;
        }

      ret = pnut_loop_watch(loop, fd, EPOLLIN, motion_readable, mo);
      if (ret < 0)
        {
          pnut_log(&mo->module, PNUT_LOG_WARNING,
                   "Sensor %d not watched: %d", s, ret);
          mo->config.ops->close(mo->config.arg, s, fd);
          motion_failed(mo);
          continue;
        }

      mo->fd[s] = fd;
    }

  if (!mo->started && !mo->stopping)
    {
      motion_started(mo);
    }

  motion_reconcile(mo);
}

/****************************************************************************
 * Name: motion_save
 *
 * Description:
 *   Keep today's steps, on the worker, if they changed.
 *
 ****************************************************************************/

static void motion_save(FAR struct motion_s *mo)
{
  if (!mo->dirty || mo->job || !mo->started || mo->stopping)
    {
      return;
    }

  mo->job_steps = mo->steps;
  mo->job_ret   = OK;
  mo->job_save  = true;
  mo->job       = true;
  mo->dirty     = false;
  if (pnut_job_submit(mo->module.loop, motion_save_run, motion_job_done,
                      mo) < 0)
    {
      mo->job      = false;
      mo->job_save = false;
      mo->dirty    = true;
    }
}

static void motion_save_tick(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  if (mo->started && motion_rollover(mo))
    {
      motion_push_steps(mo);
    }

  motion_save(mo);
}

/****************************************************************************
 * Name: motion_failed
 *
 * Description:
 *   The hub failed, not for a sensor its image lacks: nothing is opened
 *   for a while, a longer one each time until it works again.
 *
 ****************************************************************************/

static void motion_retry(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  mo->retry_timer = NULL;
  motion_reconcile(mo);
}

static void motion_failed(FAR struct motion_s *mo)
{
  uint32_t first = mo->config.retry_ms > 0 ? mo->config.retry_ms :
                   MOTION_RETRY_MS;

  if (mo->retry_timer != NULL || mo->stopping)
    {
      return;
    }

  mo->retry_ms = mo->retry_ms == 0 ? first :
                 MIN(mo->retry_ms * 2, MOTION_RETRY_MAX);
  if (pnut_timer_start(mo->module.loop, mo->retry_ms, 0, motion_retry, mo,
                       &mo->retry_timer) < 0)
    {
      mo->retry_timer = NULL;
      return;
    }

  pnut_log(&mo->module, PNUT_LOG_WARNING, "The hub tried again in %lu s",
           (unsigned long)(mo->retry_ms / 1000));
}

/****************************************************************************
 * Name: motion_reconcile
 *
 * Description:
 *   Close the sensors not wanted, have the worker open those wanted, and
 *   the step counter hold its events as long as nobody follows them.
 *
 ****************************************************************************/

static void motion_reconcile(FAR struct motion_s *mo)
{
  uint32_t want = motion_wanted(mo);
  uint32_t latency = motion_latency(mo);
  uint32_t open = 0;
  int s;

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      if (mo->fd[s] >= 0 && (want & MOTION_BIT(s)) == 0)
        {
          motion_close_sensor(mo, s);
        }
      else if (mo->fd[s] < 0 && (want & MOTION_BIT(s)) != 0)
        {
          open |= MOTION_BIT(s);
        }
    }

  if (mo->fd[MOTION_SENSOR_STEPS] >= 0 && latency != mo->latency)
    {
      mo->latency = latency;
      mo->config.ops->batch(mo->config.arg, MOTION_SENSOR_STEPS,
                            mo->fd[MOTION_SENSOR_STEPS], latency);
    }

  if (open == 0 || mo->job || mo->retry_timer != NULL)
    {
      return;
    }

  mo->job_open    = open;
  mo->job_latency = latency;
  mo->job_load    = !mo->started;
  mo->job_loaded  = false;
  mo->job_absent  = 0;
  mo->job_ret     = OK;
  mo->job         = true;
  if (pnut_job_submit(mo->module.loop, motion_open_run, motion_job_done,
                      mo) < 0)
    {
      pnut_log(&mo->module, PNUT_LOG_WARNING, "No worker for the hub");
      mo->job = false;
      return;
    }

  mo->latency = latency;
}

/****************************************************************************
 * Name: motion_ready, motion_closed
 ****************************************************************************/

static void motion_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  FAR struct motion_s *mo = arg;
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      if (mo->follow_steps[i].pending &&
          &mo->follow_steps[i].stream == stream)
        {
          motion_push(mo, &mo->follow_steps[i], true);
        }

      if (mo->follow_activity[i].pending &&
          &mo->follow_activity[i].stream == stream)
        {
          motion_push(mo, &mo->follow_activity[i], false);
        }
    }
}

static void motion_closed(FAR struct pnut_stream_s *stream, int status,
                          FAR void *arg)
{
  FAR struct motion_s *mo = arg;
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      if (&mo->follow_steps[i].stream == stream)
        {
          mo->follow_steps[i].used = false;
        }

      if (&mo->follow_activity[i].stream == stream)
        {
          mo->follow_activity[i].used = false;
        }
    }

  motion_reconcile(mo);
}

/****************************************************************************
 * Name: motion_answer
 *
 * Description:
 *   Answer a call, once the hub was looked at.
 *
 ****************************************************************************/

static FAR struct motion_follow_s *
motion_free_follow(FAR struct motion_follow_s *follows)
{
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      if (!follows[i].used)
        {
          return &follows[i];
        }
    }

  return NULL;
}

static void motion_answer(FAR struct motion_s *mo, uint8_t method,
                          FAR const struct pnut_request_s *req)
{
  bool steps = method == PNUT_MOTION_STEPS ||
               method == PNUT_MOTION_FOLLOW_STEPS;
  int sensor = steps ? MOTION_SENSOR_STEPS : MOTION_SENSOR_ACTIVITY;
  FAR struct motion_follow_s *f;

  if (method == PNUT_MOTION_ABILITIES)
    {
      pnut_motion_abilities_t out;

      memset(&out, 0, sizeof(out));
      out.still     = motion_has(mo, MOTION_SENSOR_STILL) &&
                      motion_has(mo, MOTION_SENSOR_MOVING);
      out.facing    = motion_has(mo, MOTION_SENSOR_ACCEL);
      out.steps     = motion_has(mo, MOTION_SENSOR_STEPS);
      out.activity  = motion_has(mo, MOTION_SENSOR_ACTIVITY);
      out.picked_up = motion_has(mo, MOTION_SENSOR_PICKUP);
      out.glance    = motion_has(mo, MOTION_SENSOR_GLANCE);
      pnut_motion_abilities_reply(&mo->server, req, PNUT_STATUS_OK, &out);
      return;
    }

  if (!motion_has(mo, sensor))
    {
      pnut_motion_fail(&mo->server, req, PNUT_STATUS_UNAVAILABLE,
                       PNUT_MOTION_ERROR_CODE_NOT_ABLE,
                       steps ? "no step counter" : "no activity");
      return;
    }

  if (!mo->settings.values.steps)
    {
      pnut_motion_fail(&mo->server, req, PNUT_STATUS_UNAVAILABLE,
                       PNUT_MOTION_ERROR_CODE_OFF, "turned off");
      return;
    }

  if (method == PNUT_MOTION_STEPS)
    {
      pnut_motion_steps_t out;

      motion_steps_now(mo, &out);
      pnut_motion_steps_reply(&mo->server, req, PNUT_STATUS_OK, &out);
      return;
    }

  if (method == PNUT_MOTION_ACTIVITY)
    {
      pnut_motion_activity_t out;

      memset(&out, 0, sizeof(out));
      out.kind  = mo->activity;
      out.since = mo->activity_since;
      pnut_motion_activity_reply(&mo->server, req, PNUT_STATUS_OK, &out);
      return;
    }

  f = motion_free_follow(steps ? mo->follow_steps : mo->follow_activity);
  if (f == NULL)
    {
      pnut_motion_fail(&mo->server, req, PNUT_STATUS_BUSY,
                       PNUT_MOTION_ERROR_CODE_BUSY, "too many following");
      return;
    }

  memset(f, 0, sizeof(*f));
  if ((steps ? pnut_motion_follow_steps_accept(&mo->server, req,
                                               &f->stream, motion_ready,
                                               motion_closed, mo) :
               pnut_motion_follow_activity_accept(&mo->server, req,
                                                  &f->stream, motion_ready,
                                                  motion_closed, mo)) < 0)
    {
      return;
    }

  f->used = true;
  motion_push(mo, f, steps);
  if (steps)
    {
      motion_reconcile(mo);       /* The step counter's latency */
    }
}

/****************************************************************************
 * Name: motion_call
 *
 * Description:
 *   A call: answered, or kept until the hub was looked at.
 *
 ****************************************************************************/

static void motion_call(FAR struct motion_s *mo, uint8_t method,
                        FAR const struct pnut_request_s *req)
{
  int i;

  if (mo->started)
    {
      motion_answer(mo, method, req);
      return;
    }

  for (i = 0; i < MOTION_WAITING; i++)
    {
      if (mo->waiting[i].method == 0)
        {
          mo->waiting[i].method = method;
          mo->waiting[i].req    = *req;
          return;
        }
    }

  pnut_motion_fail(&mo->server, req, PNUT_STATUS_BUSY,
                   PNUT_MOTION_ERROR_CODE_BUSY, "starting");
}

static void motion_on_steps(FAR struct pnut_motion_server_s *server,
                            FAR const struct pnut_request_s *req,
                            FAR const pnut_motion_steps_request_t *in,
                            FAR void *arg)
{
  motion_call(arg, PNUT_MOTION_STEPS, req);
}

static void motion_on_follow_steps(FAR struct pnut_motion_server_s *server,
                                   FAR const struct pnut_request_s *req,
                                   FAR const pnut_motion_steps_request_t *in,
                                   FAR void *arg)
{
  motion_call(arg, PNUT_MOTION_FOLLOW_STEPS, req);
}

static void motion_on_activity(FAR struct pnut_motion_server_s *server,
                               FAR const struct pnut_request_s *req,
                               FAR const pnut_motion_activity_request_t *in,
                               FAR void *arg)
{
  motion_call(arg, PNUT_MOTION_ACTIVITY, req);
}

static void
motion_on_follow_activity(FAR struct pnut_motion_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_motion_activity_request_t *in,
                          FAR void *arg)
{
  motion_call(arg, PNUT_MOTION_FOLLOW_ACTIVITY, req);
}

static void motion_on_abilities(FAR struct pnut_motion_server_s *server,
                                FAR const struct pnut_request_s *req,
                                FAR const pnut_motion_abilities_request_t
                                *in, FAR void *arg)
{
  motion_call(arg, PNUT_MOTION_ABILITIES, req);
}

static const struct pnut_motion_handlers_s g_motion_handlers =
{
  motion_on_steps,
  motion_on_follow_steps,
  motion_on_activity,
  motion_on_follow_activity,
  motion_on_abilities
};

/****************************************************************************
 * Name: motion_end_follows
 ****************************************************************************/

static void motion_end_follows(FAR struct motion_s *mo,
                               FAR struct motion_follow_s *follows)
{
  int i;

  for (i = 0; i < MOTION_FOLLOWS; i++)
    {
      if (follows[i].used)
        {
          follows[i].used = false;
          pnut_stream_end(&follows[i].stream, PNUT_STATUS_UNAVAILABLE);
        }
    }
}

/****************************************************************************
 * Name: motion_settings_changed, motion_time
 ****************************************************************************/

static void motion_settings_changed(FAR struct pnut_settings_cache_s *cache,
                                    int index, FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  if (!mo->settings.values.steps)
    {
      /* Turned off: who follows hears it ended */

      motion_end_follows(mo, mo->follow_steps);
      motion_end_follows(mo, mo->follow_activity);
      mo->activity       = ACT(UNKNOWN);
      mo->activity_since = 0;
    }

  motion_reconcile(mo);
}

static void motion_time(FAR struct pnut_time_change_reader_s *reader,
                        FAR const pnut_time_change_t *msg, FAR void *arg)
{
  FAR struct motion_s *mo = arg;

  /* The network's offset is the network's, whatever the rule says */

  mo->offset    = msg->offset;
  mo->tz_ok     = !msg->from_network && msg->rule[0] != '\0' &&
                  pnut_tz_parse(msg->rule, &mo->tz) == 0;
  mo->clock_set = msg->source != PNUT_TIME_INFO_SOURCE_NONE;
  if (mo->started && motion_rollover(mo))
    {
      motion_push_steps(mo);
    }
}

/****************************************************************************
 * Name: motion_module_start, motion_module_stop
 ****************************************************************************/

static int motion_module_start(FAR struct pnut_module_s *module,
                               FAR struct pnut_loop_s *loop)
{
  FAR struct motion_s *mo = (FAR struct motion_s *)module;
  int ret;

  ret = pnut_motion_settings_open(module, motion_settings_changed, mo,
                                  &mo->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
    }
  else
    {
      mo->settings_open = true;
    }

  ret = pnut_motion_info_advertise(loop, &mo->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No motion topic: %d", ret);
      mo->topic = NULL;
    }

  ret = pnut_motion_gesture_advertise(loop, &mo->gestures);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No gestures topic: %d", ret);
      mo->gestures = NULL;
    }

  ret = pnut_time_change_subscribe(loop, motion_time, mo, &mo->time_reader);
  mo->time_open = ret >= 0;
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No time topic: %d", ret);
    }

  ret = pnut_motion_serve(loop, MOTION_SERVICE, 0, &g_motion_handlers, mo,
                          &mo->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  mo->serving = true;
  pnut_timer_start(loop, mo->config.save_ms, mo->config.save_ms,
                   motion_save_tick, mo, &mo->save_timer);
  motion_publish(mo);
  motion_reconcile(mo);

  /* Calls before the hub is looked at wait for it */

  pnut_module_ready(module);
  return OK;
}

static void motion_module_stop(FAR struct pnut_module_s *module,
                               FAR struct pnut_loop_s *loop)
{
  FAR struct motion_s *mo = (FAR struct motion_s *)module;
  int s;

  mo->stopping = true;
  motion_end_follows(mo, mo->follow_steps);
  motion_end_follows(mo, mo->follow_activity);
  memset(mo->waiting, 0, sizeof(mo->waiting));

  if (mo->save_timer != NULL)
    {
      pnut_timer_cancel(loop, mo->save_timer);
      mo->save_timer = NULL;
    }

  if (mo->sample_timer != NULL)
    {
      pnut_timer_cancel(loop, mo->sample_timer);
      mo->sample_timer = NULL;
    }

  if (mo->retry_timer != NULL)
    {
      pnut_timer_cancel(loop, mo->retry_timer);
      mo->retry_timer = NULL;
    }

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      motion_close_sensor(mo, s);
    }

  if (mo->serving)
    {
      pnut_motion_close(&mo->server);
      mo->serving = false;
    }

  if (mo->time_open)
    {
      pnut_time_change_unsubscribe(&mo->time_reader);
      mo->time_open = false;
    }

  if (mo->settings_open)
    {
      pnut_motion_settings_close(&mo->settings);
      mo->settings_open = false;
    }

  if (mo->topic != NULL)
    {
      pnut_topic_unadvertise(mo->topic);
      mo->topic = NULL;
    }

  if (mo->gestures != NULL)
    {
      pnut_topic_unadvertise(mo->gestures);
      mo->gestures = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void motion_init(FAR struct motion_s *mo,
                 FAR const struct motion_config_s *config)
{
  int s;

  memset(mo, 0, sizeof(*mo));
  mo->config       = *config;
  mo->module.name  = "motion";
  mo->module.start = motion_module_start;
  mo->module.stop  = motion_module_stop;
  mo->steps.day    = -1;
  mo->samples      = MOTION_SAMPLES;

  /* Until Settings answers: the settings' starting values */

  mo->settings.values.steps     = true;
  mo->settings.values.wake      = true;
  mo->settings.values.face_down = true;

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      mo->fd[s]     = -1;
      mo->job_fd[s] = -1;
    }
}

void motion_deinit(FAR struct motion_s *mo)
{
  int s;
  int ret;

  /* What a job opened after the loop stopped, and the steps not kept: a
   * save whose done never came may have been dropped unrun
   */

  if (mo->job && mo->job_save)
    {
      mo->dirty = true;
    }

  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      if (mo->job_fd[s] >= 0)
        {
          mo->config.ops->close(mo->config.arg, s, mo->job_fd[s]);
          mo->job_fd[s] = -1;
        }

      if (mo->fd[s] >= 0)
        {
          mo->config.ops->close(mo->config.arg, s, mo->fd[s]);
          mo->fd[s] = -1;
        }
    }

  if (mo->started && mo->dirty)
    {
      ret = motion_store(mo->config.dir, &mo->steps);
      mo->dirty = ret < 0;
    }
}

int motion_facing(float x, float y, float z)
{
  float ax = fabsf(x);
  float ay = fabsf(y);
  float az = fabsf(z);

  /* The axis gravity pulls along most, when it pulls enough: not shaken,
   * falling, or halfway between two
   */

  if (az >= ax && az >= ay)
    {
      return az < MOTION_PULL ? FACING(UNKNOWN) :
             z > 0 ? FACING(UP) : FACING(DOWN);
    }

  if (ay >= ax)
    {
      return ay < MOTION_PULL ? FACING(UNKNOWN) : FACING(UPRIGHT);
    }

  return ax < MOTION_PULL ? FACING(UNKNOWN) : FACING(SIDE);
}
