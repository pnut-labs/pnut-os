/****************************************************************************
 * pnut-os/tests/unit/test_wallclock.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Time's module (RFC 0028) on a fake clock: readings taken and held, the
 * zone from its settings and the network's offset, the owner's time, and
 * the state file across restarts.  On the simulator the topics are the
 * system's own, which the system program's Time also reads: the tests
 * look at their module through its interface and the fake clock alone.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pb_encode.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "board.h"
#include "settings.h"
#include "wallclock.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 23 October 2026, 12:00 UTC: summer time in Warsaw */

#define T0       1792756800

/* The kernel's start date, 6 December 2011 */

#define T_START  1323129600

/* The fake monotonic clock, in milliseconds: far ahead of the simulator's
 * own, so that the system program's Time there takes the tests' readings
 * for another boot's, and leaves them aside.  Each test starts ten hours
 * after the last: on the simulator the topics outlive a test, and give
 * the next one's Time their last readings, which are old by then.
 */

#define T_MONO   1000000000000ull
#define T_APART  (10 * 3600000ull)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wall_test_s;
typedef CODE bool (*wall_step_t)(FAR struct wall_test_s *t);

/* A Time module on a fake clock, Settings if the test wants it, and the
 * test's own module, which calls Time, publishes readings, and runs the
 * test's steps on a timer, each once the one before it is done
 */

struct wall_test_s
{
  FAR struct pnut_loop_s *loop;
  struct settings_s settings;
  struct wallclock_s wall;
  struct pnut_module_s module;
  struct pnut_time_client_s client;
  struct pnut_settings_client_s other;
  FAR struct pnut_publisher_s *network;
  FAR struct pnut_publisher_s *gnss;
  struct pnut_time_change_reader_s changes;
  bool with_settings;
  bool connected;
  bool waiting;                   /* A call not answered yet */
  char dir[64];                   /* The run directory, Time's socket */
  char sdir[80];                  /* Settings' files */
  char state[80];
  int64_t clock;                  /* The fake clock */
  uint64_t mono;                  /* The fake monotonic clock, in ms */
  uint32_t uncertainty;           /* The readings', in ms */
  int sets;                       /* Times it was set */
  FAR const char *program;        /* Who calls; NULL: nobody known */
  int who_ret;                    /* Or who cannot tell: -EAGAIN */
  wall_step_t steps;
  int step;
  int status;                     /* The last call's answer */
  pnut_time_info_t info;          /* The last Get's */
  bool saw_warsaw;                /* The topic told of Warsaw's rule */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int wall_test_gettime(FAR void *arg, clockid_t id,
                             FAR struct timespec *ts)
{
  FAR struct wall_test_s *t = arg;

  if (id == CLOCK_MONOTONIC)
    {
      ts->tv_sec  = (time_t)(t->mono / 1000);
      ts->tv_nsec = (long)(t->mono % 1000) * 1000000;
    }
  else
    {
      ts->tv_sec  = (time_t)t->clock;
      ts->tv_nsec = 0;
    }

  return 0;
}

static int wall_test_settime(FAR void *arg, FAR const struct timespec *ts)
{
  FAR struct wall_test_s *t = arg;

  t->clock = ts->tv_sec;
  t->sets++;
  return 0;
}

static int wall_test_who(FAR void *arg, pid_t pid,
                         FAR const char **programp)
{
  FAR struct wall_test_s *t = arg;

  if (t->who_ret < 0)
    {
      return t->who_ret;
    }

  if (t->program == NULL)
    {
      return -ESRCH;
    }

  *programp = t->program;
  return 0;
}

static void wall_test_connected(FAR struct pnut_time_client_s *client,
                                bool connected, FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  t->connected = connected;
}

static void wall_test_got(FAR struct pnut_time_client_s *client, int status,
                          FAR const pnut_time_info_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  t->status  = status;
  t->waiting = false;
  if (out != NULL)
    {
      t->info = *out;
    }
}

static void wall_test_was_set(FAR struct pnut_time_client_s *client,
                              int status, FAR const pnut_time_reply_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  t->status  = status;
  t->waiting = false;
}

static void wall_test_setting_done(FAR struct pnut_settings_client_s *client,
                                   int status,
                                   FAR const pnut_settings_reply_t *out,
                                   FAR const pnut_error_t *err,
                                   FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  t->status  = status;
  t->waiting = false;
}

static void wall_test_change(FAR struct pnut_time_change_reader_s *reader,
                             FAR const pnut_time_change_t *msg,
                             FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  if (strcmp(msg->zone, "Europe/Warsaw") == 0 &&
      strcmp(msg->rule, "CET-1CEST,M3.5.0,M10.5.0/3") == 0)
    {
      t->saw_warsaw = true;
    }
}

/* Ask Time for its state */

static void wall_test_get(FAR struct wall_test_s *t)
{
  pnut_time_get_request_t req;

  memset(&req, 0, sizeof(req));
  memset(&t->info, 0, sizeof(t->info));
  t->waiting = true;
  assert_int_equal(pnut_time_get(&t->client, &req, 0, wall_test_got, t),
                   0);
}

/* Set the owner's time */

static void wall_test_set(FAR struct wall_test_s *t, int64_t utc)
{
  pnut_time_set_request_t req;

  memset(&req, 0, sizeof(req));
  req.utc    = utc;
  t->waiting = true;
  assert_int_equal(pnut_time_set(&t->client, &req, 0, wall_test_was_set,
                                 t), 0);
}

/* Change one of Time's settings, as the system UI would */

static void wall_test_setting(FAR struct wall_test_s *t,
                              FAR const char *key, FAR const char *text,
                              int boolean)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_TIME_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, key, sizeof(req.key));
  req.has_value = true;
  if (text != NULL)
    {
      req.value.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
      strlcpy(req.value.value.text, text, sizeof(req.value.value.text));
    }
  else
    {
      req.value.which_value   = PNUT_SETTING_VALUE_BOOLEAN_TAG;
      req.value.value.boolean = boolean != 0;
    }

  t->waiting = true;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0,
                                     wall_test_setting_done, t), 0);
}

/* Publish a reading of the network's time; utc 0 for its offset alone */

static void wall_test_network(FAR struct wall_test_s *t, int64_t utc,
                              bool has_offset, int32_t offset, bool dst)
{
  pnut_network_time_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.utc         = utc;
  msg.has_offset  = has_offset;
  msg.offset      = offset;
  msg.dst         = dst;
  msg.taken       = t->mono;
  msg.uncertainty = t->uncertainty;
  assert_int_equal(pnut_network_time_publish(t->network, &msg), 0);
}

static void wall_test_gnss(FAR struct wall_test_s *t, int64_t utc)
{
  pnut_gnss_time_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.utc         = utc;
  msg.taken       = t->mono;
  msg.uncertainty = t->uncertainty;
  assert_int_equal(pnut_gnss_time_publish(t->gnss, &msg), 0);
}

/* The steps run every 50 ms while no call is waiting: a step that returns
 * true is done, and the next runs at the next tick
 */

static void wall_test_tick(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wall_test_s *t = arg;

  if (!t->connected || t->waiting)
    {
      return;
    }

  if (t->steps(t))
    {
      t->step++;
    }
}

static void wall_test_stop(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static int wall_test_start(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct wall_test_s *t = (FAR struct wall_test_s *)
    ((FAR uint8_t *)module - offsetof(struct wall_test_s, module));

  assert_int_equal(pnut_network_time_advertise(loop, &t->network), 0);
  assert_int_equal(pnut_gnss_time_advertise(loop, &t->gnss), 0);
  assert_int_equal(pnut_time_change_subscribe(loop, wall_test_change, t,
                                              &t->changes), 0);
  assert_int_equal(pnut_time_connect(loop, WALLCLOCK_SERVICE,
                                     wall_test_connected, t, &t->client),
                   0);
  if (t->with_settings)
    {
      assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE, NULL,
                                             NULL, &t->other), 0);
    }

  assert_int_equal(pnut_timer_start(loop, 50, 50, wall_test_tick, t,
                                    NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void wall_test_stop_module(FAR struct pnut_module_s *module,
                                  FAR struct pnut_loop_s *loop)
{
  FAR struct wall_test_s *t = (FAR struct wall_test_s *)
    ((FAR uint8_t *)module - offsetof(struct wall_test_s, module));

  if (t->with_settings)
    {
      pnut_settings_disconnect(&t->other);
    }

  pnut_time_disconnect(&t->client);
  pnut_time_change_unsubscribe(&t->changes);
  pnut_topic_unadvertise(t->gnss);
  pnut_topic_unadvertise(t->network);
}

static void wall_test_run(FAR struct wall_test_s *t, wall_step_t steps)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 32,
    .texts    = 8,
    .choices  = 16,
  };

  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct wallclock_config_s wconfig;

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  if (t->with_settings)
    {
      memset(&sconfig, 0, sizeof(sconfig));
      sconfig.dir    = t->sdir;
      sconfig.limits = limits;
      sconfig.delay  = 20;
      settings_init(&t->settings, &sconfig);
      assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);
    }

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.state     = t->state;
  wconfig.tolerance = 120;
  wconfig.who       = wall_test_who;
  wconfig.identity  = t;
  wconfig.gettime   = wall_test_gettime;
  wconfig.settime   = wall_test_settime;
  wconfig.clock     = t;
  wallclock_init(&t->wall, &wconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->wall.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = wall_test_start;
  t->module.stop  = wall_test_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  t->steps     = steps;
  t->step      = 0;
  t->connected = false;
  t->waiting   = false;
  pnut_timer_start(t->loop, 5000, 0, wall_test_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(t->loop), 0);

  pnut_loop_destroy(t->loop);
  if (t->with_settings)
    {
      settings_deinit(&t->settings);
    }
}

static FAR struct wall_test_s *wall_test_new(bool with_settings)
{
  static uint64_t mono = T_MONO;
  FAR struct wall_test_s *t = calloc(1, sizeof(*t));

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-wall-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(t->state, sizeof(t->state), "%s/state", t->dir);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  assert_int_equal(mkdir(t->sdir, 0700), 0);
  t->with_settings = with_settings;
  t->clock         = T0;
  t->mono          = mono;
  mono            += T_APART;
  return t;
}

/* Remove a test's directory: the state file, Settings' file, and the
 * topics' directories on the computer
 */

static void wall_test_free(FAR struct wall_test_s *t)
{
  static FAR const char * const topics[] =
  {
    "time", "network_time", "gnss_time", "settings"
  };

  char path[160];
  size_t i;

  unlink(t->state);
  snprintf(path, sizeof(path), "%s/%s.pb", t->sdir,
           PNUT_TIME_SETTINGS_OWNER);
  unlink(path);
  rmdir(t->sdir);
  for (i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    {
      snprintf(path, sizeof(path), "%s/topic.%s/last", t->dir, topics[i]);
      unlink(path);
      snprintf(path, sizeof(path), "%s/topic.%s", t->dir, topics[i]);
      rmdir(path);
    }

  rmdir(t->dir);
  free(t);
}

/* The first reading is taken; one far off is held, and a second from the
 * same source changes nothing; another source agreeing lets it in; a
 * near one is taken
 */

static bool wall_test_readings_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        wall_test_network(t, T0 + 100, false, 0, false);
        return true;

      case 1:
        if (t->sets == 0)
          {
            return false;
          }

        assert_true(t->clock == T0 + 100);
        wall_test_network(t, t->clock + 1000, false, 0, false);
        return true;

      case 2:
        wall_test_get(t);
        return true;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_NETWORK);
        assert_true(t->info.since == T0 + 100);
        assert_true(t->clock == T0 + 100);
        assert_int_equal(t->sets, 1);
        wall_test_network(t, t->clock + 1000, false, 0, false);
        return true;

      case 4:
        wall_test_get(t);
        return true;

      case 5:
        assert_true(t->clock == T0 + 100);
        assert_int_equal(t->sets, 1);
        wall_test_gnss(t, t->clock + 1010);
        return true;

      case 6:
        if (t->sets == 1)
          {
            return false;
          }

        assert_true(t->clock == T0 + 1110);
        wall_test_network(t, t->clock + 30, false, 0, false);
        return true;

      case 7:
        if (t->sets == 2)
          {
            return false;
          }

        assert_true(t->clock == T0 + 1140);
        wall_test_get(t);
        return true;

      case 8:
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_NETWORK);
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

static void test_wallclock_readings(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(false);

  wall_test_run(t, wall_test_readings_steps);
  assert_int_equal(t->step, 9);
  assert_int_equal(t->sets, 3);
  wall_test_free(t);
}

/* The order of preference: a GNSS reading near the clock gives way to
 * the network's of the last hour, and moves the clock once that is older;
 * a reading within its own uncertainty confirms the clock without moving
 * it
 */

static bool wall_test_preference_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        wall_test_network(t, T0 + 100, false, 0, false);
        return true;

      case 1:
        if (t->sets == 0)
          {
            return false;
          }

        assert_true(t->clock == T0 + 100);
        wall_test_gnss(t, t->clock + 30);
        return true;

      case 2:
        wall_test_get(t);
        return true;

      case 3:
        assert_int_equal(t->sets, 1);
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_NETWORK);
        assert_true(t->info.since == T0 + 100);

        /* An hour and more on, the network's reading is no longer at
         * hand
         */

        t->clock += 3700;
        wall_test_gnss(t, t->clock + 30);
        return true;

      case 4:
        if (t->sets == 1)
          {
            return false;
          }

        assert_true(t->clock == T0 + 100 + 3700 + 30);
        wall_test_get(t);
        return true;

      case 5:
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_GNSS);

        /* The network's reading, a second off but a second and a half
         * uncertain: preferred, it becomes the source, the clock unmoved
         */

        t->uncertainty = 1500;
        wall_test_network(t, t->clock + 1, false, 0, false);
        return true;

      case 6:
        wall_test_get(t);
        return true;

      case 7:
        if (t->info.source != PNUT_TIME_INFO_SOURCE_NETWORK)
          {
            wall_test_get(t);
            return false;
          }

        assert_int_equal(t->sets, 2);
        assert_true(t->clock == T0 + 100 + 3700 + 30);
        assert_true(t->info.since == t->clock);

        /* Three seconds off is beyond it: the clock moves */

        wall_test_network(t, t->clock + 3, false, 0, false);
        return true;

      case 8:
        if (t->sets == 2)
          {
            return false;
          }

        assert_true(t->clock == T0 + 100 + 3700 + 33);
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

static void test_wallclock_preference(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(false);

  wall_test_run(t, wall_test_preference_steps);
  assert_int_equal(t->step, 9);
  wall_test_free(t);
}

/* The zone: the named one's rule; the network's offset while zone.auto
 * is on, at any offset; the named one's again when it is turned off
 */

static bool wall_test_zone_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        if (!pnut_settings_cache_ready(&t->wall.settings.cache))
          {
            return false;
          }

        wall_test_get(t);
        return true;

      case 1:
        assert_string_equal(t->info.zone, "UTC");
        assert_string_equal(t->info.rule, "UTC0");
        assert_int_equal(t->info.offset, 0);
        assert_false(t->info.from_network);
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_NONE);
        wall_test_setting(t, "zone", "Europe/Warsaw", 0);
        return true;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (strcmp(t->wall.zone, "Europe/Warsaw") != 0)
          {
            return false;
          }

        wall_test_get(t);
        return true;

      case 3:
        assert_string_equal(t->info.zone, "Europe/Warsaw");
        assert_string_equal(t->info.rule, "CET-1CEST,M3.5.0,M10.5.0/3");
        assert_int_equal(t->info.offset, 7200);
        assert_true(t->info.dst);
        wall_test_network(t, 0, true, 5 * 3600 + 45 * 60, false);
        return true;

      case 4:
        if (!t->wall.from_network)
          {
            return false;
          }

        wall_test_get(t);
        return true;

      case 5:
        assert_string_equal(t->info.zone, "Europe/Warsaw");
        assert_string_equal(t->info.rule, "<+0545>-5:45");
        assert_int_equal(t->info.offset, 20700);
        assert_true(t->info.from_network);
        assert_int_equal(t->sets, 0);
        wall_test_network(t, 0, true, -3 * 3600, true);
        return true;

      case 6:
        if (t->wall.net_offset != -3 * 3600)
          {
            return false;
          }

        wall_test_get(t);
        return true;

      case 7:
        assert_string_equal(t->info.rule, "<-03>3");
        assert_int_equal(t->info.offset, -10800);
        assert_true(t->info.dst);
        wall_test_setting(t, "zone.auto", NULL, 0);
        return true;

      case 8:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->wall.from_network)
          {
            return false;
          }

        wall_test_get(t);
        return true;

      case 9:
        assert_string_equal(t->info.rule, "CET-1CEST,M3.5.0,M10.5.0/3");
        assert_false(t->info.from_network);

        /* A name the table does not have: UTC */

        wall_test_setting(t, "zone", "Europe/Nowhere", 0);
        return true;

      case 10:
        if (strcmp(t->wall.zone, "UTC") != 0)
          {
            return false;
          }

        assert_string_equal(t->wall.rule, "UTC0");
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

static void test_wallclock_zone(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(true);

  wall_test_run(t, wall_test_zone_steps);
  assert_int_equal(t->step, 11);
  assert_true(t->saw_warsaw);
  wall_test_free(t);
}

/* The owner's time: from the system UI alone, not before 2020 */

static bool wall_test_owner_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        t->program = "shell";
        wall_test_set(t, T0 + 3600);
        return true;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        t->program = NULL;
        wall_test_set(t, T0 + 3600);
        return true;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->sets, 0);
        t->who_ret = -EAGAIN;
        wall_test_set(t, T0 + 3600);
        return true;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        t->who_ret = 0;
        t->program = "ui";
        wall_test_set(t, 1000);
        return true;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        wall_test_set(t, 4102444801);
        return true;

      case 5:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->sets, 0);
        wall_test_set(t, T0 + 3600);
        return true;

      case 6:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->clock == T0 + 3600);
        wall_test_get(t);
        return true;

      case 7:
        assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_OWNER);

        /* A reading two minutes old is left aside, and so is one taken
         * after now, by another boot's clock
         */

        t->mono -= 120000;
        wall_test_network(t, T0 + 7000, false, 0, false);
        t->mono += 125000;
        wall_test_gnss(t, T0 + 7000);
        t->mono -= 5000;
        wall_test_get(t);
        return true;

      case 8:
        assert_int_equal(t->sets, 1);
        assert_true(t->clock == T0 + 3600);

        /* The owner's time is a fallback: a reading far from it is taken
         * at once, its age added
         */

        t->mono -= 3000;
        wall_test_network(t, T0 + 7200, false, 0, false);
        t->mono += 3000;
        return true;

      case 9:
        if (t->sets < 2)
          {
            return false;
          }

        assert_true(t->clock == T0 + 7203);
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

static void test_wallclock_owner(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(false);

  wall_test_run(t, wall_test_owner_steps);
  assert_int_equal(t->step, 10);
  wall_test_free(t);
}

/* The state file: a clock behind it after a restart (a power loss) is
 * put at its time, not confirmed; one that kept counting keeps its source
 */

static bool wall_test_restart_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        wall_test_get(t);
        return true;

      case 1:
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

/* Write a state file, as Time or a hand would */

static void wall_test_write(FAR const char *path, FAR const char *text)
{
  int fd;

  unlink(path);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert_true(fd >= 0);
  assert_int_equal(write(fd, text, strlen(text)), (ssize_t)strlen(text));
  close(fd);
}

/* Start Time on a clock at the kernel's start date, after the topics'
 * last readings have grown old
 */

static void wall_test_restart(FAR struct wall_test_s *t)
{
  t->clock = T_START;
  t->sets  = 0;
  t->mono += 3600000;
  wall_test_run(t, wall_test_restart_steps);
}

static void test_wallclock_state(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(false);
  char line[96];
  ssize_t n;
  int fd;

  /* The first run: a reading, saved as the module stops */

  wall_test_run(t, wall_test_readings_steps);
  assert_true(t->clock == T0 + 1140);

  fd = open(t->state, O_RDONLY);
  assert_true(fd >= 0);
  n = read(fd, line, sizeof(line) - 1);
  close(fd);
  assert_true(n > 0);
  line[n] = '\0';
  assert_string_equal(line, "pnut-time 1 1792757940 3 1792757940\n");

  /* After a power loss, the clock at the kernel's start date; the
   * topics' last readings, given to Time as it subscribes, are an hour
   * old by now
   */

  t->clock = T_START;
  t->sets  = 0;
  t->mono += 3600000;
  wall_test_run(t, wall_test_restart_steps);
  assert_true(t->clock == T0 + 1140);
  assert_int_equal(t->sets, 1);
  assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_SAVED);

  /* After a restart that kept the clock: its source stays */

  t->clock = T0 + 5000;
  t->sets  = 0;
  t->mono += 3600000;
  wall_test_run(t, wall_test_restart_steps);
  assert_true(t->clock == T0 + 5000);
  assert_int_equal(t->sets, 0);
  assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_SAVED);

  wall_test_free(t);
}

/* After a power loss the clock is only the state file's time: the first
 * reading is taken as it is, however uncertain
 */

static bool wall_test_uncertain_steps(FAR struct wall_test_s *t)
{
  switch (t->step)
    {
      case 0:
        t->uncertainty = 30000;
        wall_test_network(t, T0 + 1020, false, 0, false);
        return true;

      case 1:
        if (t->sets < 2)
          {
            return false;
          }

        assert_true(t->clock == T0 + 1020);
        pnut_loop_stop(t->loop, 0);
        return true;

      default:
        return false;
    }
}

static void test_wallclock_uncertain(void **state)
{
  FAR struct wall_test_s *t = wall_test_new(false);

  wall_test_write(t->state, "pnut-time 1 1792757800 3 1792757800\n");
  t->clock = T_START;
  wall_test_run(t, wall_test_uncertain_steps);
  assert_int_equal(t->step, 2);
  assert_int_equal(t->sets, 2);
  wall_test_free(t);
}

/* State files that do not parse, or name a time out of range, a source
 * that never set the clock, or a reading after the time saved, are left
 * aside; a new file left under its temporary name is taken
 */

static void test_wallclock_bad_state(void **state)
{
  static FAR const char * const bad[] =
  {
    "garbage\n",
    "pnut-time 2 1792757940 3 1792757940\n",
    "pnut-time 1 1792757940 9 1792757940\n",
    "pnut-time 1 1792757940 0 0\n",
    "pnut-time 1 1792757940 3 1792757941\n",
    "pnut-time 1 1000 3 500\n",
    "pnut-time 1 4102444801 3 1\n",
  };

  FAR struct wall_test_s *t = wall_test_new(false);
  char temp[96];
  size_t i;

  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
      wall_test_write(t->state, bad[i]);
      wall_test_restart(t);
      assert_true(t->clock == T_START);
      assert_int_equal(t->sets, 0);
      assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_NONE);
    }

  /* A clock never set leaves no file of its own */

  unlink(t->state);
  wall_test_restart(t);
  assert_int_equal(access(t->state, F_OK), -1);

  snprintf(temp, sizeof(temp), "%s.new", t->state);
  wall_test_write(temp, "pnut-time 1 1792757300 3 1792757300\n");
  wall_test_restart(t);
  assert_true(t->clock == T0 + 500);
  assert_int_equal(t->info.source, PNUT_TIME_INFO_SOURCE_SAVED);
  unlink(temp);

  wall_test_free(t);
}

/* The sleep clock's rate: Board, with a description, Time, and a module
 * reading what Time announces.  A topic keeps its last message, in the
 * simulator from earlier tests too: the test publishes a marker first, and
 * counts only what differs from it.
 */

#define RATE_MARKER  (-1)

struct rate_test_s
{
  struct board_s board;
  struct wallclock_s wall;
  struct pnut_module_s module;
  struct pnut_sleep_rate_reader_s reader;
  int messages;
  pnut_sleep_rate_t msg;
};

static void rate_test_stop(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void rate_test_got(FAR struct pnut_sleep_rate_reader_s *reader,
                          FAR const pnut_sleep_rate_t *msg, FAR void *arg)
{
  FAR struct rate_test_s *t = arg;

  if (msg->ppb != RATE_MARKER)
    {
      /* Time asked Board once, and let go of it as the answer came */

      assert_false(t->wall.board_open);
      t->messages++;
      t->msg = *msg;
    }
}

static int rate_test_start(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct rate_test_s *t = (FAR struct rate_test_s *)
    ((FAR uint8_t *)module - offsetof(struct rate_test_s, module));

  assert_int_equal(pnut_sleep_rate_subscribe(loop, rate_test_got, t,
                                             &t->reader), 0);
  pnut_module_ready(module);
  return 0;
}

static void rate_test_stop_module(FAR struct pnut_module_s *module,
                                  FAR struct pnut_loop_s *loop)
{
  FAR struct rate_test_s *t = (FAR struct rate_test_s *)
    ((FAR uint8_t *)module - offsetof(struct rate_test_s, module));

  pnut_sleep_rate_unsubscribe(&t->reader);
}

/* Run with a description that has a sleep clock of rate ppb, or none */

static void rate_test_run(FAR struct rate_test_s *t, bool clock,
                          int32_t ppb)
{
  FAR pnut_board_description_t *d = calloc(1, sizeof(*d));
  uint8_t buffer[PNUT_BOARD_DESCRIPTION_SIZE];
  struct pnut_loop_config_s config;
  struct wallclock_config_s wconfig;
  struct board_config_s bconfig;
  FAR struct pnut_publisher_s *marker;
  FAR struct pnut_loop_s *loop;
  pnut_sleep_rate_t msg;
  pb_ostream_t stream;
  char dir[48];
  char path[96];
  size_t i;
  FILE *f;
  static FAR const char * const topics[] =
  {
    "sleep_rate", "time", "network_time", "gnss_time", "settings"
  };

  snprintf(dir, sizeof(dir), "/tmp/pnut-rate-%d", (int)getpid());
  assert_int_equal(mkdir(dir, 0700), 0);
  snprintf(path, sizeof(path), "%s/board.pb", dir);

  assert_non_null(d);
  d->has_identity = true;
  strlcpy(d->identity.model, "Model", sizeof(d->identity.model));
  if (clock)
    {
      d->parts[0].kind         = PNUT_BOARD_PART_KIND_SLEEP_CLOCK;
      d->parts[0].which_detail = PNUT_BOARD_PART_SLEEP_CLOCK_TAG;
      d->parts[0].detail.sleep_clock.rate_ppb = ppb;
      d->parts_count = 1;
    }

  stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
  assert_true(pb_encode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, d));
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buffer, 1, stream.bytes_written, f),
                   stream.bytes_written);
  fclose(f);
  free(d);

  pnut_loop_defaults(&config);
  config.rundir = dir;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);

  assert_int_equal(pnut_sleep_rate_advertise(loop, &marker), 0);
  memset(&msg, 0, sizeof(msg));
  msg.ppb = RATE_MARKER;
  assert_int_equal(pnut_sleep_rate_publish(marker, &msg), 0);

  bconfig.path = path;
  board_init(&t->board, &bconfig);
  assert_int_equal(pnut_module_add(loop, &t->board.module), 0);

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.board = BOARD_SERVICE;
  wallclock_init(&t->wall, &wconfig);
  assert_int_equal(pnut_module_add(loop, &t->wall.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = rate_test_start;
  t->module.stop  = rate_test_stop_module;
  assert_int_equal(pnut_module_add(loop, &t->module), 0);

  t->messages = 0;
  assert_int_equal(pnut_timer_start(loop, 500, 0, rate_test_stop, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_topic_unadvertise(marker);
  pnut_loop_destroy(loop);

  unlink(path);
  for (i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    {
      snprintf(path, sizeof(path), "%s/topic.%s/last", dir, topics[i]);
      unlink(path);
      snprintf(path, sizeof(path), "%s/topic.%s", dir, topics[i]);
      rmdir(path);
    }

  rmdir(dir);
}

/* Time announces the sleep clock's rate Board's description gives, once;
 * without one, none
 */

static void test_wallclock_sleep_rate(void **state)
{
  FAR struct rate_test_s *t = calloc(1, sizeof(*t));

  assert_non_null(t);
  rate_test_run(t, true, 8500000);
  assert_int_equal(t->messages, 1);
  assert_int_equal(t->msg.ppb, 8500000);
  assert_false(t->msg.learnt);
  assert_false(t->wall.board_open);

  rate_test_run(t, false, 0);
  assert_int_equal(t->messages, 0);
  assert_false(t->wall.board_open);
  free(t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_wallclock_readings),
    cmocka_unit_test(test_wallclock_preference),
    cmocka_unit_test(test_wallclock_zone),
    cmocka_unit_test(test_wallclock_owner),
    cmocka_unit_test(test_wallclock_state),
    cmocka_unit_test(test_wallclock_bad_state),
    cmocka_unit_test(test_wallclock_uncertain),
    cmocka_unit_test(test_wallclock_sleep_rate),
  };

  return cmocka_run_group_tests_name("wallclock", tests, NULL, NULL);
}
