/****************************************************************************
 * pnut-os/tests/unit/test_haptics.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Haptics (RFC 0033) against a fake motor, with Settings in the same loop:
 * the named effects as the DRV2605's library or as steps, the uses' order,
 * callers' patterns and their limits, and the settings.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
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

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/settings.pnut.h"

#include "haptics.h"
#include "settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HAPTICS_SERVICE  "haptics"
#define CALLS            64

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What the fake motor was told */

enum call_e
{
  CALL_LEVEL = 0,
  CALL_SEQUENCE,
  CALL_STOP
};

struct call_s
{
  uint8_t kind;
  uint8_t value;                  /* A level, or a library effect */
  uint32_t ms;
};

struct hapt_s;
typedef CODE bool (*hapt_step_t)(FAR struct hapt_s *t);

struct hapt_s
{
  FAR struct pnut_loop_s *loop;
  hapt_step_t steps;
  int last;
  struct settings_s settings;
  struct haptics_s haptics;
  struct pnut_module_s module;
  struct pnut_haptics_client_s client;
  struct pnut_settings_client_s other;
  struct pnut_haptics_info_reader_s reader;
  char dir[64];
  char sdir[80];
  int step;
  int ticks;
  bool waiting;
  int status;                     /* The last answer's */
  uint32_t code;
  pnut_haptics_info_t state;      /* The topic's last */
  int mark;                       /* A scenario's own: a call's index */

  bool open_fails;
  bool opened;
  struct call_s calls[CALLS];
  int ncalls;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The fake motor */

static void fake_call(FAR struct hapt_s *t, uint8_t kind, uint8_t value,
                      uint32_t ms)
{
  assert_true(t->ncalls < CALLS);
  t->calls[t->ncalls].kind  = kind;
  t->calls[t->ncalls].value = value;
  t->calls[t->ncalls].ms    = ms;
  t->ncalls++;
}

static int fake_open(FAR void *arg, FAR const char *path)
{
  FAR struct hapt_s *t = arg;

  assert_string_equal(path, "/dev/fake_ff");
  t->opened = !t->open_fails;
  return t->open_fails ? -ENODEV : OK;
}

static void fake_close(FAR void *arg)
{
  FAR struct hapt_s *t = arg;

  t->opened = false;
}

static int fake_level(FAR void *arg, uint8_t level, uint32_t ms)
{
  fake_call(arg, CALL_LEVEL, level, ms);
  return OK;
}

static int fake_sequence(FAR void *arg, FAR const uint8_t *effects,
                         size_t count)
{
  assert_int_equal(count, 1);
  fake_call(arg, CALL_SEQUENCE, effects[0], 0);
  return OK;
}

static int fake_stop(FAR void *arg)
{
  fake_call(arg, CALL_STOP, 0, 0);
  return OK;
}

static const struct haptics_motor_ops_s g_fake =
{
  fake_open,
  fake_close,
  fake_level,
  fake_sequence,
  fake_stop
};

/* Calls to Haptics and Settings */

static void hapt_replied(FAR struct pnut_haptics_client_s *client,
                         int status, FAR const pnut_haptics_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct hapt_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->waiting = false;
}

static void hapt_effect(FAR struct hapt_s *t, int effect, int use)
{
  pnut_haptics_effect_request_t req;

  memset(&req, 0, sizeof(req));
  req.effect = effect;
  req.use    = use;
  t->waiting = true;
  assert_int_equal(pnut_haptics_effect(&t->client, &req, 0, hapt_replied,
                                       t), 0);
}

static void hapt_pattern(FAR struct hapt_s *t, int use, int n,
                         FAR const uint32_t *ms, FAR const uint32_t *level)
{
  pnut_haptics_pattern_request_t req;
  int i;

  memset(&req, 0, sizeof(req));
  for (i = 0; i < n; i++)
    {
      req.steps[i].ms       = ms[i];
      req.steps[i].strength = level[i];
    }

  req.steps_count = n;
  req.use         = use;
  t->waiting      = true;
  assert_int_equal(pnut_haptics_pattern(&t->client, &req, 0, hapt_replied,
                                        t), 0);
}

static void hapt_stop(FAR struct hapt_s *t, int use)
{
  pnut_haptics_stop_request_t req;

  memset(&req, 0, sizeof(req));
  req.use    = use;
  t->waiting = true;
  assert_int_equal(pnut_haptics_stop(&t->client, &req, 0, hapt_replied,
                                     t), 0);
}

static void hapt_set_done(FAR struct pnut_settings_client_s *client,
                          int status, FAR const pnut_settings_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct hapt_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  t->waiting = false;
}

static void hapt_set(FAR struct hapt_s *t, FAR const char *key,
                     FAR const char *text, bool boolean)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_HAPTICS_SETTINGS_OWNER, sizeof(req.owner));
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
      req.value.value.boolean = boolean;
    }

  t->waiting = true;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0, hapt_set_done, t),
                   0);
}

/* The fake's calls from an index: their kinds and values, and their
 * number
 */

static void hapt_expect(FAR struct hapt_s *t, int from, int n,
                        FAR const uint8_t *kinds, FAR const uint8_t *values)
{
  int i;

  assert_true(t->ncalls - from >= n);
  for (i = 0; i < n; i++)
    {
      assert_int_equal(t->calls[from + i].kind, kinds[i]);
      assert_int_equal(t->calls[from + i].value, values[i]);
    }
}

/* The first call from an index that is not a stop: an effect started
 * while another plays stills the motor first
 */

static int hapt_first(FAR struct hapt_s *t, int from)
{
  while (from < t->ncalls && t->calls[from].kind == CALL_STOP)
    {
      from++;
    }

  assert_true(from < t->ncalls);
  return from;
}

/****************************************************************************
 * Name: hapt_effects
 *
 * Description:
 *   A click is the library's strong click at medium strength, and ends on
 *   its own; a notification is steps at medium's 80 %, then still.
 *
 ****************************************************************************/

static bool hapt_effects(FAR struct hapt_s *t)
{
  static const uint8_t kinds[] =
  {
    CALL_LEVEL, CALL_LEVEL, CALL_LEVEL, CALL_STOP
  };

  static const uint8_t values[] =
  {
    80, 0, 80, 0
  };

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_true(t->opened);
        assert_false(t->state.playing);
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_CLICK,
                    PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION);
        break;

      case 1:                     /* Played, then over: stilled */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->calls[0].kind, CALL_SEQUENCE);
        assert_int_equal(t->calls[0].value, 18);
        if (t->state.playing || t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->ncalls, 2);
        assert_int_equal(t->calls[1].kind, CALL_STOP);

        t->mark = t->ncalls;
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_NOTIFICATION,
                    PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION);
        break;

      case 2:                     /* Its three steps, then still */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->state.playing || t->ncalls < t->mark + 4)
          {
            return false;
          }

        hapt_expect(t, t->mark, 4, kinds, values);
        assert_int_equal(t->calls[t->mark].ms, 150);
        assert_int_equal(t->calls[t->mark + 1].ms, 120);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: hapt_uses
 *
 * Description:
 *   A ring repeats; a notification asked meanwhile is dropped; an alarm
 *   stops it and plays; a stop of the ring's use leaves the alarm, a stop
 *   of the alarm's stills the motor.
 *
 ****************************************************************************/

static bool hapt_uses(FAR struct hapt_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_RING,
                    PNUT_HAPTICS_USE_HAPTICS_USE_RING);
        break;

      case 1:                     /* Repeats: more than its four steps */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->ncalls < 5)
          {
            return false;
          }

        assert_true(t->state.playing);
        assert_int_equal(t->state.use, PNUT_HAPTICS_USE_HAPTICS_USE_RING);
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_NOTIFICATION,
                    PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION);
        break;

      case 2:                     /* Dropped */
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_HAPTICS_ERROR_CODE_BUSY);
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_ALARM,
                    PNUT_HAPTICS_USE_HAPTICS_USE_ALARM);
        break;

      case 3:                     /* The alarm plays */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->state.use != PNUT_HAPTICS_USE_HAPTICS_USE_ALARM)
          {
            return false;
          }

        assert_int_equal(t->state.effect,
                         PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_ALARM);
        hapt_stop(t, PNUT_HAPTICS_USE_HAPTICS_USE_RING);
        break;

      case 4:                     /* Still plays */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->ticks < 3)
          {
            return false;
          }

        assert_true(t->state.playing);
        hapt_stop(t, PNUT_HAPTICS_USE_HAPTICS_USE_ALARM);
        break;

      case 5:                     /* Still */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->state.playing)
          {
            return false;
          }

        assert_int_equal(t->calls[t->ncalls - 1].kind, CALL_STOP);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: hapt_patterns
 *
 * Description:
 *   A pattern over 5 s, or with a strength over 100, is refused; one
 *   plays its steps at medium's 80 %; an app's second within a second is
 *   refused.
 *
 ****************************************************************************/

static bool hapt_patterns(FAR struct hapt_s *t)
{
  static const uint32_t long_ms[] =
  {
    3000, 2500
  };

  static const uint32_t ms[] =
  {
    100, 50, 100
  };

  static const uint32_t levels[] =
  {
    100, 0, 50
  };

  static const uint32_t over[] =
  {
    101
  };

  static const uint8_t kinds[] =
  {
    CALL_LEVEL, CALL_LEVEL, CALL_LEVEL, CALL_STOP
  };

  static const uint8_t values[] =
  {
    80, 0, 40, 0
  };

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        hapt_pattern(t, PNUT_HAPTICS_USE_HAPTICS_USE_APP, 2, long_ms,
                     levels);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->code, PNUT_HAPTICS_ERROR_CODE_TOO_LONG);
        hapt_pattern(t, PNUT_HAPTICS_USE_HAPTICS_USE_APP, 1, ms, over);
        break;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->ncalls, 0);
        hapt_pattern(t, PNUT_HAPTICS_USE_HAPTICS_USE_APP, 3, ms, levels);
        break;

      case 3:                     /* Plays */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->state.pattern);
        hapt_pattern(t, PNUT_HAPTICS_USE_HAPTICS_USE_APP, 3, ms, levels);
        break;

      case 4:                     /* Too soon after */
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_HAPTICS_ERROR_CODE_TOO_OFTEN);
        if (t->state.playing)
          {
            return false;
          }

        hapt_expect(t, 0, 4, kinds, values);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: hapt_settings
 *
 * Description:
 *   The interface's effects are off by default; turned on, a tick plays.
 *   Weak picks the library's lighter click, and scales levels to 60 %.
 *
 ****************************************************************************/

static bool hapt_settings(FAR struct hapt_s *t)
{
  static const uint32_t ms[] =
  {
    100
  };

  static const uint32_t levels[] =
  {
    100
  };

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_TICK,
                    PNUT_HAPTICS_USE_HAPTICS_USE_INTERFACE);
        break;

      case 1:                     /* Off */
        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_HAPTICS_ERROR_CODE_OFF);
        hapt_set(t, "interface", NULL, true);
        break;

      case 2:
        if (!t->haptics.settings.values.interface)
          {
            return false;
          }

        t->mark = t->ncalls;
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_TICK,
                    PNUT_HAPTICS_USE_HAPTICS_USE_INTERFACE);
        break;

      case 3:                     /* The medium tick */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        t->mark = hapt_first(t, t->mark);
        assert_int_equal(t->calls[t->mark].kind, CALL_SEQUENCE);
        assert_int_equal(t->calls[t->mark].value, 25);
        hapt_set(t, "strength", "weak", false);
        break;

      case 4:
        if (strcmp(t->haptics.settings.values.strength, "weak") != 0)
          {
            return false;
          }

        t->mark = t->ncalls;
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_CLICK,
                    PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION);
        break;

      case 5:                     /* The weak click; then a pattern */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        t->mark = hapt_first(t, t->mark);
        assert_int_equal(t->calls[t->mark].kind, CALL_SEQUENCE);
        assert_int_equal(t->calls[t->mark].value, 19);
        t->mark = t->ncalls;
        hapt_pattern(t, PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION, 1, ms,
                     levels);
        break;

      case 6:                     /* At 60 % */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        t->mark = hapt_first(t, t->mark);
        assert_int_equal(t->calls[t->mark].kind, CALL_LEVEL);
        assert_int_equal(t->calls[t->mark].value, 60);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: hapt_no_motor
 ****************************************************************************/

static bool hapt_no_motor(FAR struct hapt_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_false(t->opened);
        hapt_effect(t, PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_CLICK,
                    PNUT_HAPTICS_USE_HAPTICS_USE_APP);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_HAPTICS_ERROR_CODE_NO_MOTOR);
        assert_int_equal(t->ncalls, 0);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: hapt_run
 ****************************************************************************/

static void hapt_tick(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct hapt_s *t = arg;

  t->ticks++;
  if (t->step > t->last || !t->steps(t))
    {
      return;
    }

  t->ticks = 0;
  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void hapt_state(FAR struct pnut_haptics_info_reader_s *reader,
                       FAR const pnut_haptics_info_t *msg, FAR void *arg)
{
  FAR struct hapt_s *t = arg;

  t->state = *msg;
}

static int hapt_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct hapt_s *t = (FAR struct hapt_s *)
    ((FAR uint8_t *)module - offsetof(struct hapt_s, module));

  assert_int_equal(pnut_haptics_info_subscribe(loop, hapt_state, t,
                                               &t->reader), 0);
  assert_int_equal(pnut_haptics_connect(loop, HAPTICS_SERVICE, NULL, NULL,
                                        &t->client), 0);
  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                                         &t->other), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, hapt_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void hapt_stop_module(FAR struct pnut_module_s *module,
                             FAR struct pnut_loop_s *loop)
{
  FAR struct hapt_s *t = (FAR struct hapt_s *)
    ((FAR uint8_t *)module - offsetof(struct hapt_s, module));

  pnut_haptics_info_unsubscribe(&t->reader);
  pnut_haptics_disconnect(&t->client);
  pnut_settings_disconnect(&t->other);
}

static void hapt_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/* Haptics' settings are the media program's */

static int hapt_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  *programp = "media";
  return OK;
}

static bool hapt_runs(FAR void *arg, FAR const char *program,
                      FAR const char *service)
{
  return strcmp(program, "media") == 0;
}

static void hapt_run(hapt_step_t steps, int last, bool open_fails)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 16,
    .texts    = 8,
    .choices  = 16,
    .secrets  = 2,
  };

  FAR struct hapt_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct haptics_config_s hconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  t->steps      = steps;
  t->last       = last;
  t->open_fails = open_fails;
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-haptics-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  assert_int_equal(mkdir(t->sdir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  sconfig.who    = hapt_who;
  sconfig.runs   = hapt_runs;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&hconfig, 0, sizeof(hconfig));
  hconfig.ops    = &g_fake;
  hconfig.arg    = t;
  hconfig.device = "/dev/fake_ff";
  hconfig.chip   = "DRV2605L";
  haptics_init(&t->haptics, &hconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->haptics.module), 0);

  t->module.name  = "test";
  t->module.start = hapt_start;
  t->module.stop  = hapt_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 10000, 0, hapt_timeout, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);
  assert_int_equal(system(cmd), 0);
  free(t);
}

static void test_haptics_effects(void **state)
{
  hapt_run(hapt_effects, 2, false);
}

static void test_haptics_uses(void **state)
{
  hapt_run(hapt_uses, 5, false);
}

static void test_haptics_patterns(void **state)
{
  hapt_run(hapt_patterns, 4, false);
}

static void test_haptics_settings(void **state)
{
  hapt_run(hapt_settings, 6, false);
}

static void test_haptics_no_motor(void **state)
{
  hapt_run(hapt_no_motor, 1, true);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_haptics_effects),
    cmocka_unit_test(test_haptics_uses),
    cmocka_unit_test(test_haptics_patterns),
    cmocka_unit_test(test_haptics_settings),
    cmocka_unit_test(test_haptics_no_motor),
  };

  return cmocka_run_group_tests_name("haptics", tests, NULL, NULL);
}
