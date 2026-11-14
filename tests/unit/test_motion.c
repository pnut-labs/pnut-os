/****************************************************************************
 * pnut-os/tests/unit/test_motion.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Motion (RFC 0036) against a fake hub, whose sensors the test makes say
 * things.  What the hub lacks, Motion says it cannot tell; still or
 * moving, and the facing read from the accelerometer, which is on only
 * meanwhile; today's steps counted from the hub's count, kept across a
 * restart of Motion and of the hub, and started again at local midnight;
 * activity; gestures; the settings turning sensors off.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/settings.pnut.h"

#include "motion.h"
#include "settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MOTION_SERVICE "motion"
#define BIT(s)         (1u << (s))
#define G              9.80665f

#define ACT(k)         PNUT_MOTION_ACTIVITY_KIND_MOTION_ACTIVITY_##k
#define MOVEMENT(k)    PNUT_MOTION_MOVEMENT_MOTION_MOVEMENT_##k
#define FACING(k)      PNUT_MOTION_FACING_MOTION_FACING_##k

/* 10 s before local midnight, an hour east of UTC */

#define EVENING        (20405 * 86400 + 86390 - 3600)
#define SUMMER_DAY     20300      /* 2025-07-31, in CEST */
#define SUMMER_EVENING (SUMMER_DAY * 86400 + 86390 - 7200)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mot_s;
typedef CODE bool (*mot_step_t)(FAR struct mot_s *t);

struct mot_s
{
  FAR struct pnut_loop_s *loop;
  mot_step_t steps;
  int last;
  int step;
  struct motion_s mo;
  struct settings_s settings;
  struct pnut_module_s module;
  struct pnut_motion_client_s client;
  struct pnut_settings_client_s set;
  struct pnut_motion_info_reader_s reader;
  struct pnut_motion_gesture_reader_s greader;
  FAR struct pnut_publisher_s *time_pub;
  char dir[64];
  char sdir[80];
  bool connected;                 /* To Motion and Settings */
  bool set_connected;

  bool answered;                  /* The last call's answer came */
  int status;
  uint32_t code;
  pnut_motion_steps_t steps_out;
  pnut_motion_activity_t act_out;
  pnut_motion_abilities_t abilities;
  bool set_done;

  pnut_motion_info_t info;
  pnut_motion_gesture_t gesture;
  int gestures;

  /* A stream following steps or the activity */

  uint16_t stream;
  int items;
  int ended;                      /* Its end's status, once it ended */
  pnut_motion_steps_t step_item;
  pnut_motion_activity_t act_item;

  /* The fake hub: a pipe for each sensor open, written by the worker */

  pthread_mutex_t lock;
  uint32_t absent;
  uint32_t fail_open;             /* Fail an open once: -EIO */
  uint32_t fail_read;             /* Fail a read once: -EIO */
  int pipe[MOTION_NSENSORS][2];
  bool opened[MOTION_NSENSORS];
  int opens[MOTION_NSENSORS];
  uint32_t latency[MOTION_NSENSORS];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static time_t g_now;
static uint32_t g_save_ms = 100;  /* Motion's saves of a changed count */
static uint32_t g_fail_open;      /* The fake hub's next run's */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t fake_now(void)
{
  return g_now != 0 ? g_now : time(NULL);
}

static int fake_open(FAR void *arg, int sensor, uint32_t latency_ms)
{
  FAR struct mot_s *t = arg;
  int ret;

  pthread_mutex_lock(&t->lock);
  if ((t->absent & BIT(sensor)) != 0)
    {
      ret = -ENODEV;
    }
  else if ((t->fail_open & BIT(sensor)) != 0)
    {
      t->fail_open &= ~BIT(sensor);
      ret = -EIO;
    }
  else
    {
      assert_false(t->opened[sensor]);
      assert_int_equal(pipe(t->pipe[sensor]), 0);
      assert_int_equal(fcntl(t->pipe[sensor][0], F_SETFL, O_NONBLOCK), 0);
      t->opened[sensor]  = true;
      t->latency[sensor] = latency_ms;
      t->opens[sensor]++;
      ret = t->pipe[sensor][0];
    }

  pthread_mutex_unlock(&t->lock);
  return ret;
}

static void fake_close(FAR void *arg, int sensor, int fd)
{
  FAR struct mot_s *t = arg;

  pthread_mutex_lock(&t->lock);
  assert_true(t->opened[sensor]);
  assert_int_equal(fd, t->pipe[sensor][0]);
  close(t->pipe[sensor][0]);
  close(t->pipe[sensor][1]);
  t->opened[sensor] = false;
  pthread_mutex_unlock(&t->lock);
}

static int fake_read(FAR void *arg, int sensor, int fd,
                     FAR struct motion_event_s *ev)
{
  FAR struct mot_s *t = arg;
  bool fail;
  ssize_t n;

  pthread_mutex_lock(&t->lock);
  fail = (t->fail_read & BIT(sensor)) != 0;
  t->fail_read &= ~BIT(sensor);
  pthread_mutex_unlock(&t->lock);
  if (fail)
    {
      return -EIO;
    }

  n = read(fd, ev, sizeof(*ev));
  return n == sizeof(*ev) ? 1 : 0;
}

static int fake_batch(FAR void *arg, int sensor, int fd, uint32_t latency_ms)
{
  FAR struct mot_s *t = arg;

  pthread_mutex_lock(&t->lock);
  t->latency[sensor] = latency_ms;
  pthread_mutex_unlock(&t->lock);
  return 0;
}

static const struct motion_hub_ops_s g_fake =
{
  fake_open,
  fake_close,
  fake_read,
  fake_batch
};

static bool is_open(FAR struct mot_s *t, int sensor)
{
  bool open;

  pthread_mutex_lock(&t->lock);
  open = t->opened[sensor];
  pthread_mutex_unlock(&t->lock);
  return open;
}

static uint32_t latency(FAR struct mot_s *t, int sensor)
{
  uint32_t ms;

  pthread_mutex_lock(&t->lock);
  ms = t->latency[sensor];
  pthread_mutex_unlock(&t->lock);
  return ms;
}

/* What the hub's sensor says */

static void say(FAR struct mot_s *t, int sensor, uint32_t value, float x,
                float y, float z)
{
  struct motion_event_s ev;

  assert_true(is_open(t, sensor));
  memset(&ev, 0, sizeof(ev));
  ev.value = value;
  ev.x     = x;
  ev.y     = y;
  ev.z     = z;
  assert_int_equal(write(t->pipe[sensor][1], &ev, sizeof(ev)),
                   sizeof(ev));
}

static void say_accel(FAR struct mot_s *t, float x, float y, float z)
{
  int i;

  for (i = 0; i < MOTION_SAMPLES; i++)
    {
      say(t, MOTION_SENSOR_ACCEL, 0, x, y, z);
    }
}

/* Calls */

static void mot_failed(FAR struct mot_s *t, int status,
                       FAR const pnut_error_t *err)
{
  t->status   = status;
  t->code     = err != NULL ? err->code : 0;
  t->answered = true;
}

static void mot_steps_done(FAR struct pnut_motion_client_s *client,
                           int status, FAR const pnut_motion_steps_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (out != NULL)
    {
      t->steps_out = *out;
    }

  mot_failed(t, status, err);
}

static void mot_steps(FAR struct mot_s *t)
{
  pnut_motion_steps_request_t req;

  memset(&req, 0, sizeof(req));
  memset(&t->steps_out, 0, sizeof(t->steps_out));
  t->answered = false;
  assert_int_equal(pnut_motion_steps(&t->client, &req, 5000,
                                     mot_steps_done, t), 0);
}

static void mot_activity_done(FAR struct pnut_motion_client_s *client,
                              int status,
                              FAR const pnut_motion_activity_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (out != NULL)
    {
      t->act_out = *out;
    }

  mot_failed(t, status, err);
}

static void mot_activity(FAR struct mot_s *t)
{
  pnut_motion_activity_request_t req;

  memset(&req, 0, sizeof(req));
  memset(&t->act_out, 0, sizeof(t->act_out));
  t->answered = false;
  assert_int_equal(pnut_motion_activity(&t->client, &req, 5000,
                                        mot_activity_done, t), 0);
}

static void mot_abilities_done(FAR struct pnut_motion_client_s *client,
                               int status,
                               FAR const pnut_motion_abilities_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (out != NULL)
    {
      t->abilities = *out;
    }

  mot_failed(t, status, err);
}

static void mot_abilities(FAR struct mot_s *t)
{
  pnut_motion_abilities_request_t req;

  memset(&req, 0, sizeof(req));
  memset(&t->abilities, 0, sizeof(t->abilities));
  t->answered = false;
  assert_int_equal(pnut_motion_abilities(&t->client, &req, 5000,
                                         mot_abilities_done, t), 0);
}

static void mot_opened(FAR struct pnut_motion_client_s *client, int status,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  mot_failed(arg, status, err);
}

static void mot_step_item(FAR struct pnut_motion_client_s *client,
                          int status, FAR const pnut_motion_steps_t *item,
                          FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (item == NULL)
    {
      t->ended = status;
      return;
    }

  t->step_item = *item;
  t->items++;
  pnut_motion_follow_steps_credit(client, t->stream, 1);
}

static void mot_follow_steps(FAR struct mot_s *t)
{
  pnut_motion_steps_request_t req;

  memset(&req, 0, sizeof(req));
  t->answered = false;
  t->items    = 0;
  t->ended    = 0;
  assert_int_equal(pnut_motion_follow_steps(&t->client, &req, 2, 5000,
                                            mot_opened, mot_step_item, t,
                                            &t->stream), 0);
}

static void mot_act_item(FAR struct pnut_motion_client_s *client,
                         int status, FAR const pnut_motion_activity_t *item,
                         FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (item == NULL)
    {
      t->ended = status;
      return;
    }

  t->act_item = *item;
  t->items++;
  pnut_motion_follow_activity_credit(client, t->stream, 1);
}

static void mot_follow_activity(FAR struct mot_s *t)
{
  pnut_motion_activity_request_t req;

  memset(&req, 0, sizeof(req));
  t->answered = false;
  t->items    = 0;
  t->ended    = 0;
  assert_int_equal(pnut_motion_follow_activity(&t->client, &req, 2, 5000,
                                               mot_opened, mot_act_item, t,
                                               &t->stream), 0);
}

static void mot_set_done(FAR struct pnut_settings_client_s *client,
                         int status, FAR const pnut_settings_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  t->set_done = true;
}

static void mot_set(FAR struct mot_s *t, FAR const char *key, bool on)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_MOTION_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, key, sizeof(req.key));
  req.has_value               = true;
  req.value.which_value       = PNUT_SETTING_VALUE_BOOLEAN_TAG;
  req.value.value.boolean     = on;
  t->set_done = false;
  assert_int_equal(pnut_settings_set(&t->set, &req, 0, mot_set_done, t),
                   0);
}

static void mot_time_rule(FAR struct mot_s *t, int source, int32_t offset,
                          FAR const char *rule)
{
  pnut_time_change_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.source = source;
  msg.offset = offset;
  msg.seq    = 1;
  strlcpy(msg.rule, rule, sizeof(msg.rule));
  assert_int_equal(pnut_time_change_publish(t->time_pub, &msg), 0);
}

static void mot_time(FAR struct mot_s *t, int source, int32_t offset)
{
  mot_time_rule(t, source, offset, "");
}

/****************************************************************************
 * Name: mot_abilities_steps
 *
 * Description:
 *   A hub without activity and glances: Motion says so, and opens the rest
 *   the settings want; the accelerometer only until it tells the facing.
 *
 ****************************************************************************/

static bool mot_lacks(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_abilities(t);
        return true;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->abilities.still);
        assert_true(t->abilities.facing);
        assert_true(t->abilities.steps);
        assert_false(t->abilities.activity);
        assert_true(t->abilities.picked_up);
        assert_false(t->abilities.glance);
        mot_activity(t);
        return true;

      case 2:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_MOTION_ERROR_CODE_NOT_ABLE);
        assert_true(is_open(t, MOTION_SENSOR_STILL));
        assert_true(is_open(t, MOTION_SENSOR_MOVING));
        assert_true(is_open(t, MOTION_SENSOR_TILT));
        assert_true(is_open(t, MOTION_SENSOR_STEPS));
        assert_true(is_open(t, MOTION_SENSOR_PICKUP));
        assert_true(is_open(t, MOTION_SENSOR_ACCEL));
        assert_int_equal(latency(t, MOTION_SENSOR_STEPS), 60000);
        say_accel(t, 0.3f, -0.2f, G);
        return true;

      case 3:
        if (t->info.facing != FACING(UP))
          {
            return false;
          }

        assert_false(is_open(t, MOTION_SENSOR_ACCEL));
        assert_int_equal(t->info.movement, MOVEMENT(UNKNOWN));
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_still
 *
 * Description:
 *   Moving, then still: the accelerometer read again for the facing; an
 *   angle changed with no readings given up after a while.
 *
 ****************************************************************************/

static bool mot_still(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started || !is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        say_accel(t, 0.5f, G, 0.4f);
        return true;

      case 1:
        if (t->info.facing != FACING(UPRIGHT) ||
            is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        say(t, MOTION_SENSOR_MOVING, 1, 0, 0, 0);
        return true;

      case 2:
        if (t->info.movement != MOVEMENT(MOVING))
          {
            return false;
          }

        assert_false(is_open(t, MOTION_SENSOR_ACCEL));
        say(t, MOTION_SENSOR_STILL, 1, 0, 0, 0);
        return true;

      case 3:
        if (t->info.movement != MOVEMENT(STILL) ||
            !is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        say_accel(t, 0.1f, 0.2f, -G);
        return true;

      case 4:
        if (t->info.facing != FACING(DOWN) ||
            is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        say(t, MOTION_SENSOR_TILT, 1, 0, 0, 0);
        return true;

      case 5:
        if (!is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        return true;

      case 6:

        /* No readings: closed after a while, the facing as it was */

        if (is_open(t, MOTION_SENSOR_ACCEL))
          {
            return false;
          }

        assert_int_equal(t->info.facing, FACING(DOWN));
        assert_int_equal(t->opens[MOTION_SENSOR_ACCEL], 3);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_count, mot_recount
 *
 * Description:
 *   Steps from the hub's count; followed, the hub holds them less long.
 *   Then, Motion started again: the count kept, the hub's going on, and
 *   the hub started again.
 *
 ****************************************************************************/

static bool mot_count(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_steps(t);
        return true;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->steps_out.count, 0);
        assert_int_equal(t->steps_out.changed, 0);
        say(t, MOTION_SENSOR_STEPS, 10, 0, 0, 0);
        mot_follow_steps(t);
        return true;

      case 2:
        if (t->items < 1 || t->step_item.count != 10)
          {
            return false;
          }

        assert_int_equal(latency(t, MOTION_SENSOR_STEPS), 2000);
        assert_int_equal(t->step_item.changed, g_now);
        say(t, MOTION_SENSOR_STEPS, 25, 0, 0, 0);
        return true;

      case 3:
        if (t->step_item.count != 25)
          {
            return false;
          }

        assert_int_equal(pnut_motion_follow_steps_cancel(&t->client,
                                                         t->stream), 0);
        return true;

      case 4:
        if (latency(t, MOTION_SENSOR_STEPS) != 60000)
          {
            return false;
          }

        mot_steps(t);
        return true;

      case 5:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->steps_out.count, 25);
        return true;
    }

  return false;
}

static bool mot_recount(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_steps(t);
        return true;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->steps_out.count, 25);
        say(t, MOTION_SENSOR_STEPS, 31, 0, 0, 0);    /* The hub went on */
        say(t, MOTION_SENSOR_STEPS, 4, 0, 0, 0);     /* It started again */
        return true;

      case 2:
        if (t->mo.steps.count != 35)
          {
            return false;
          }

        assert_int_equal(t->mo.steps.hub_last, 4);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_midnight
 *
 * Description:
 *   No day while the clock has no source, then today's; the count starts
 *   again at local midnight, which a follower hears.
 *
 ****************************************************************************/

static bool mot_midnight(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        say(t, MOTION_SENSOR_STEPS, 100, 0, 0, 0);
        return true;

      case 1:
        if (t->mo.steps.count != 100)
          {
            return false;
          }

        assert_int_equal(t->mo.steps.day, -1);
        mot_time(t, PNUT_TIME_INFO_SOURCE_NONE, 3600);
        t->items = 0;
        return true;

      case 2:

        /* A clock never set tells no day, however long it waits */

        if (++t->items < 10)
          {
            return false;
          }

        assert_int_equal(t->mo.steps.day, -1);
        mot_time(t, PNUT_TIME_INFO_SOURCE_NETWORK, 3600);
        return true;

      case 3:
        if (t->mo.steps.day != 20405)
          {
            return false;
          }

        /* Counted before the day was known: today's */

        mot_follow_steps(t);
        return true;

      case 4:
        if (t->items < 1)
          {
            return false;
          }

        assert_int_equal(t->step_item.count, 100);
        say(t, MOTION_SENSOR_STEPS, 150, 0, 0, 0);
        return true;

      case 5:
        if (t->step_item.count != 150)
          {
            return false;
          }

        g_now += 20;                /* Past midnight */
        return true;

      case 6:
        if (t->step_item.count != 0 || t->mo.steps.day != 20406)
          {
            return false;
          }

        say(t, MOTION_SENSOR_STEPS, 157, 0, 0, 0);
        return true;

      case 7:
        if (t->step_item.count != 7)
          {
            return false;
          }

        assert_int_equal(t->step_item.changed, g_now);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_glitch
 *
 * Description:
 *   The hub failing (not a sensor it lacks) leaves nothing absent: the
 *   sensors are opened again a little later; so too after a read fails.
 *
 ****************************************************************************/

static int opens(FAR struct mot_s *t, int sensor)
{
  int n;

  pthread_mutex_lock(&t->lock);
  n = t->opens[sensor];
  pthread_mutex_unlock(&t->lock);
  return n;
}

static bool mot_glitch(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        assert_false(is_open(t, MOTION_SENSOR_STEPS));
        mot_abilities(t);
        return true;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_true(t->abilities.steps);
        assert_true(t->abilities.still);
        return true;

      case 2:
        if (!is_open(t, MOTION_SENSOR_STILL))
          {
            return false;
          }

        assert_true(is_open(t, MOTION_SENSOR_STEPS));
        assert_int_equal(t->mo.absent, 0);
        pthread_mutex_lock(&t->lock);
        t->fail_read = BIT(MOTION_SENSOR_STILL);
        pthread_mutex_unlock(&t->lock);
        say(t, MOTION_SENSOR_STILL, 1, 0, 0, 0);
        return true;

      case 3:
        if (opens(t, MOTION_SENSOR_STILL) < 2 ||
            !is_open(t, MOTION_SENSOR_STILL))
          {
            return false;
          }

        assert_int_equal(t->mo.absent, 0);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_summer
 *
 * Description:
 *   Summer time begun since Time last announced the offset (it announces
 *   none for that): the day turns at the zone's rule's midnight, an hour
 *   before the stale offset's.
 *
 ****************************************************************************/

static bool mot_summer(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_time_rule(t, PNUT_TIME_INFO_SOURCE_NTP, 3600,
                      "CET-1CEST,M3.5.0,M10.5.0/3");
        say(t, MOTION_SENSOR_STEPS, 100, 0, 0, 0);
        return true;

      case 1:
        if (t->mo.steps.count != 100 || t->mo.steps.day != SUMMER_DAY)
          {
            return false;
          }

        g_now += 20;                /* Past midnight in summer time */
        return true;

      case 2:
        if (t->mo.steps.day != SUMMER_DAY + 1)
          {
            return false;
          }

        assert_int_equal(t->mo.steps.count, 0);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_activities
 ****************************************************************************/

static bool mot_activities(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        say(t, MOTION_SENSOR_ACTIVITY, MOTION_STARTED(ACT(WALKING)), 0, 0,
            0);
        return true;

      case 1:
        if (t->mo.activity != ACT(WALKING))
          {
            return false;
          }

        mot_activity(t);
        return true;

      case 2:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->act_out.kind, ACT(WALKING));
        assert_int_equal(t->act_out.since, g_now);
        mot_follow_activity(t);
        return true;

      case 3:
        if (t->items < 1)
          {
            return false;
          }

        assert_int_equal(t->act_item.kind, ACT(WALKING));
        say(t, MOTION_SENSOR_ACTIVITY,
            MOTION_ENDED(ACT(WALKING)) | MOTION_STARTED(ACT(STILL)) |
            MOTION_STARTED(ACT(VEHICLE)), 0, 0, 0);
        return true;

      case 4:
        if (t->act_item.kind != ACT(VEHICLE))
          {
            return false;
          }

        say(t, MOTION_SENSOR_ACTIVITY, MOTION_ENDED(ACT(STILL)), 0, 0, 0);
        say(t, MOTION_SENSOR_ACTIVITY, MOTION_ENDED(ACT(VEHICLE)), 0, 0,
            0);
        return true;

      case 5:
        if (t->act_item.kind != ACT(UNKNOWN))
          {
            return false;
          }

        assert_int_equal(t->items, 3);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_settings
 *
 * Description:
 *   Gestures while motion.wake is on; steps and activity off with
 *   motion.steps, which ends who follows, and back on.
 *
 ****************************************************************************/

static bool mot_settings(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_follow_steps(t);
        say(t, MOTION_SENSOR_PICKUP, 1, 0, 0, 0);
        say(t, MOTION_SENSOR_GLANCE, 1, 0, 0, 0);
        return true;

      case 1:
        if (t->items < 1 || t->gestures < 2)
          {
            return false;
          }

        assert_int_equal(t->gesture.kind, PNUT_MOTION_GESTURE_KIND_GLANCE);
        mot_set(t, "steps", false);
        return true;

      case 2:
        if (!t->set_done || is_open(t, MOTION_SENSOR_STEPS) ||
            is_open(t, MOTION_SENSOR_ACTIVITY) || t->ended == 0)
          {
            return false;
          }

        assert_int_equal(t->ended, PNUT_STATUS_UNAVAILABLE);
        mot_steps(t);
        return true;

      case 3:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_MOTION_ERROR_CODE_OFF);
        mot_set(t, "wake", false);
        return true;

      case 4:
        if (!t->set_done || is_open(t, MOTION_SENSOR_PICKUP) ||
            is_open(t, MOTION_SENSOR_GLANCE))
          {
            return false;
          }

        assert_true(is_open(t, MOTION_SENSOR_STILL));
        mot_set(t, "steps", true);
        return true;

      case 5:
        if (!is_open(t, MOTION_SENSOR_STEPS) ||
            !is_open(t, MOTION_SENSOR_ACTIVITY))
          {
            return false;
          }

        assert_int_equal(t->opens[MOTION_SENSOR_STEPS], 2);
        assert_false(is_open(t, MOTION_SENSOR_PICKUP));
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_nohub
 ****************************************************************************/

static bool mot_nohub(FAR struct mot_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!t->mo.started)
          {
            return false;
          }

        mot_abilities(t);
        return true;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_false(t->abilities.still || t->abilities.facing ||
                     t->abilities.steps || t->abilities.activity ||
                     t->abilities.picked_up || t->abilities.glance);
        mot_steps(t);
        return true;

      case 2:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->code, PNUT_MOTION_ERROR_CODE_NOT_ABLE);
        assert_int_equal(t->info.movement, MOVEMENT(UNKNOWN));
        assert_int_equal(t->info.facing, FACING(UNKNOWN));
        assert_false(t->mo.job);
        return true;
    }

  return false;
}

/****************************************************************************
 * Name: mot_tick, mot_start, mot_stop, mot_run
 ****************************************************************************/

static void mot_tick(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  if (!t->connected || !t->set_connected || t->step > t->last ||
      !t->steps(t))
    {
      return;
    }

  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void mot_info(FAR struct pnut_motion_info_reader_s *reader,
                     FAR const pnut_motion_info_t *msg, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  t->info = *msg;
}

static void mot_gesture(FAR struct pnut_motion_gesture_reader_s *reader,
                        FAR const pnut_motion_gesture_t *msg, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  t->gesture = *msg;
  t->gestures++;
}

static void mot_connected(FAR struct pnut_motion_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  t->connected = connected;
}

static void mot_set_connected(FAR struct pnut_settings_client_s *client,
                              bool connected, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  t->set_connected = connected;
}

static int mot_start(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct mot_s *t = (FAR struct mot_s *)
    ((FAR uint8_t *)module - offsetof(struct mot_s, module));

  assert_int_equal(pnut_motion_info_subscribe(loop, mot_info, t,
                                              &t->reader), 0);
  assert_int_equal(pnut_motion_gesture_subscribe(loop, mot_gesture, t,
                                                 &t->greader), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &t->time_pub), 0);
  assert_int_equal(pnut_motion_connect(loop, MOTION_SERVICE, mot_connected,
                                       t, &t->client), 0);
  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE,
                                         mot_set_connected, t, &t->set), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, mot_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void mot_stop(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct mot_s *t = (FAR struct mot_s *)
    ((FAR uint8_t *)module - offsetof(struct mot_s, module));

  pnut_motion_info_unsubscribe(&t->reader);
  pnut_motion_gesture_unsubscribe(&t->greader);
  pnut_topic_unadvertise(t->time_pub);
  pnut_motion_disconnect(&t->client);
  pnut_settings_disconnect(&t->set);
}

static void mot_timeout(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct mot_s *t = arg;

  print_error("Stuck at step %d (answered %d, started %d, items %d, "
              "connected %d %d)\n", t->step, t->answered, t->mo.started,
              t->items, t->connected, t->set_connected);
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/* Motion's settings are the sensors program's */

static int mot_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  *programp = "sensors";
  return OK;
}

static bool mot_runs(FAR void *arg, FAR const char *program,
                     FAR const char *service)
{
  return strcmp(program, "sensors") == 0;
}

/* Run Motion and the test's steps; fresh, a new directory for its steps
 * and settings, else the last run's
 */

static void mot_run(mot_step_t steps, int last, uint32_t absent, bool fresh)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 16,
    .texts    = 8,
    .choices  = 16,
    .secrets  = 2,
  };

  FAR struct mot_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct motion_config_s mconfig;
  char cmd[96];
  int ret;
  int s;

  assert_non_null(t);
  t->steps     = steps;
  t->last      = last;
  t->absent    = absent;
  t->fail_open = g_fail_open;
  g_fail_open  = 0;
  pthread_mutex_init(&t->lock, NULL);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-motion-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  if (fresh)
    {
      assert_int_equal(system(cmd), 0);
      assert_int_equal(mkdir(t->dir, 0700), 0);
      assert_int_equal(mkdir(t->sdir, 0700), 0);
    }

  pnut_loop_defaults(&config);
  config.rundir  = t->dir;
  config.workers = 1;
  config.jobs    = 2;
  config.fds    += MOTION_NSENSORS;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  sconfig.who    = mot_who;
  sconfig.runs   = mot_runs;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&mconfig, 0, sizeof(mconfig));
  mconfig.ops     = &g_fake;
  mconfig.arg     = t;
  mconfig.dir     = t->dir;
  mconfig.save_ms = g_save_ms;
  mconfig.retry_ms = 100;
  mconfig.now     = fake_now;
  motion_init(&t->mo, &mconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->mo.module), 0);

  t->module.name  = "test";
  t->module.start = mot_start;
  t->module.stop  = mot_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 10000, 0, mot_timeout, t, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  motion_deinit(&t->mo);
  settings_deinit(&t->settings);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);
  for (s = 0; s < MOTION_NSENSORS; s++)
    {
      assert_false(t->opened[s]);
    }

  pthread_mutex_destroy(&t->lock);
  free(t);
}

static void test_motion_lacks(void **state)
{
  g_now = EVENING;
  mot_run(mot_lacks, 3, BIT(MOTION_SENSOR_ACTIVITY) |
          BIT(MOTION_SENSOR_GLANCE), true);
}

static void test_motion_still(void **state)
{
  g_now = EVENING;
  mot_run(mot_still, 6, 0, true);
}

static void test_motion_steps(void **state)
{
  /* Saved as Motion stops alone */

  g_now     = EVENING;
  g_save_ms = 3600000;
  mot_run(mot_count, 5, 0, true);
  mot_run(mot_recount, 2, 0, false);
  g_save_ms = 100;
}

static void test_motion_midnight(void **state)
{
  g_now = EVENING;
  mot_run(mot_midnight, 7, 0, true);
}

static void test_motion_glitch(void **state)
{
  g_now       = EVENING;
  g_fail_open = BIT(MOTION_SENSOR_STEPS);
  mot_run(mot_glitch, 3, 0, true);
}

static void test_motion_summer(void **state)
{
  g_now = SUMMER_EVENING;
  mot_run(mot_summer, 2, 0, true);
}

static void test_motion_activity(void **state)
{
  g_now = EVENING;
  mot_run(mot_activities, 5, 0, true);
}

static void test_motion_settings(void **state)
{
  g_now = EVENING;
  mot_run(mot_settings, 5, 0, true);
}

static void test_motion_nohub(void **state)
{
  g_now = EVENING;
  mot_run(mot_nohub, 2, BIT(MOTION_NSENSORS) - 1, true);
}

static void test_motion_facing(void **state)
{
  assert_int_equal(motion_facing(0, 0, G), FACING(UP));
  assert_int_equal(motion_facing(0.2f, -0.3f, -G), FACING(DOWN));
  assert_int_equal(motion_facing(0, G, 0), FACING(UPRIGHT));
  assert_int_equal(motion_facing(0, -G, 0), FACING(UPRIGHT));
  assert_int_equal(motion_facing(-G, 0, 0.1f), FACING(SIDE));

  /* Held at 45 degrees: the screen's up wins a tie */

  assert_int_equal(motion_facing(0, 0.7071f * G, 0.7071f * G), FACING(UP));

  /* Between three axes, or falling: unknown */

  assert_int_equal(motion_facing(0.55f * G, 0.55f * G, 0.55f * G),
                   FACING(UNKNOWN));
  assert_int_equal(motion_facing(0, 0, 0.1f), FACING(UNKNOWN));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_motion_facing),
    cmocka_unit_test(test_motion_lacks),
    cmocka_unit_test(test_motion_still),
    cmocka_unit_test(test_motion_steps),
    cmocka_unit_test(test_motion_midnight),
    cmocka_unit_test(test_motion_summer),
    cmocka_unit_test(test_motion_glitch),
    cmocka_unit_test(test_motion_activity),
    cmocka_unit_test(test_motion_settings),
    cmocka_unit_test(test_motion_nohub),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
