/****************************************************************************
 * pnut-os/tests/unit/test_settings_cache.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

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
#include <pnut/msg.h>
#include <pnut/settings_cache.h>
#include <pnut/timer.h>

#include <pb_encode.h>

#include "pnut/test/prefs.settings.h"
#include "settings.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A Settings module and a program's module with the test owner's typed
 * settings, in one loop; each step runs when the one before it is seen
 */

struct cache_test_s
{
  FAR struct pnut_loop_s *loop;
  struct settings_s settings;
  struct pnut_module_s module;
  struct pnut_test_prefs_s prefs;
  struct pnut_settings_client_s other;   /* Another client of Settings */
  char dir[64];
  char defaults[96];              /* The target's defaults, or empty */
  bool restarted;                 /* Only read the values, then stop */
  int step;
  int changes[16];                /* The indexes the handler was given */
  int nchanges;
  int status;                     /* The last set's answer */
  uint32_t code;
  bool defaults_seen;             /* The struct held them at open */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct settings_limits_s g_limits =
{
  .owners   = 4,
  .settings = 32,
  .texts    = 8,
  .choices  = 16,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cache_test_stop(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void cache_test_next(FAR struct cache_test_s *t);

static void cache_test_done(FAR struct pnut_settings_cache_s *cache,
                            int status, uint32_t code, FAR void *arg)
{
  FAR struct cache_test_s *t = arg;

  t->status = status;
  t->code   = code;

  /* A refused change changes nothing: go on at once */

  if (status != PNUT_STATUS_OK)
    {
      t->step++;
      cache_test_next(t);
    }
}

static void cache_test_changed(FAR struct pnut_settings_cache_s *cache,
                               int index, FAR void *arg)
{
  FAR struct cache_test_s *t = arg;

  if (t->nchanges < 16)
    {
      t->changes[t->nchanges++] = index;
    }

  t->step++;
  cache_test_next(t);
}

static void cache_test_other_done(FAR struct pnut_settings_client_s *client,
                                  int status,
                                  FAR const pnut_settings_reply_t *out,
                                  FAR const pnut_error_t *err,
                                  FAR void *arg)
{
  FAR struct cache_test_s *t = arg;

  t->status = status;
}

/* The steps, each when the last one's change has come back */

static void cache_test_next(FAR struct cache_test_s *t)
{
  pnut_settings_set_request_t req;

  switch (t->step)
    {
      case 1:

        /* Read: the defaults on the first run, the changes after */

        assert_true(pnut_settings_cache_ready(&t->prefs.cache));
        if (t->restarted)
          {
            pnut_loop_stop(t->loop, 0);
            break;
          }

        assert_int_equal(pnut_test_prefs_set_level(&t->prefs, 15,
                                                   cache_test_done, t), 0);
        break;

      case 2:
        assert_int_equal(t->prefs.values.level, 15);

        /* Out of its range: refused */

        assert_int_equal(pnut_test_prefs_set_level(&t->prefs, 99,
                                                   cache_test_done, t), 0);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->code, PNUT_SETTINGS_ERROR_CODE_OUT_OF_RANGE);
        assert_int_equal(t->prefs.values.level, 15);
        assert_int_equal(pnut_test_prefs_set_colour(&t->prefs, "blue",
                                                    NULL, NULL), 0);
        break;

      case 4:
        assert_string_equal(t->prefs.values.colour, "blue");

        /* Changed by another program: seen through the topic */

        memset(&req, 0, sizeof(req));
        strlcpy(req.owner, PNUT_TEST_PREFS_OWNER, sizeof(req.owner));
        strlcpy(req.key, "big.number", sizeof(req.key));
        req.has_value           = true;
        req.value.which_value   = PNUT_SETTING_VALUE_INTEGER_TAG;
        req.value.value.integer = 7000000000;
        assert_int_equal(pnut_settings_set(&t->other, &req, 0,
                                           cache_test_other_done, t), 0);
        break;

      case 5:
        assert_true(t->prefs.values.big == 7000000000);

        /* Settings writes a moment after the last change */

        pnut_timer_start(t->loop, 200, 0, cache_test_stop, NULL, NULL);
        break;

      default:
        break;
    }
}

static int cache_test_start(FAR struct pnut_module_s *module,
                            FAR struct pnut_loop_s *loop)
{
  FAR struct cache_test_s *t = (FAR struct cache_test_s *)
    ((FAR uint8_t *)module - offsetof(struct cache_test_s, module));
  int ret;

  ret = pnut_test_prefs_open(module, cache_test_changed, t, &t->prefs);
  if (ret < 0)
    {
      return ret;
    }

  /* The defaults are there before Settings answers */

  t->defaults_seen = t->prefs.values.loud &&
                     t->prefs.values.level == 10 &&
                     t->prefs.values.count == 0 &&
                     t->prefs.values.big == 5000000000 &&
                     strcmp(t->prefs.values.name, "pnut") == 0 &&
                     strcmp(t->prefs.values.colour, "green") == 0;

  pnut_module_ready(module);
  return pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                               &t->other);
}

static void cache_test_stop_module(FAR struct pnut_module_s *module,
                                   FAR struct pnut_loop_s *loop)
{
  FAR struct cache_test_s *t = (FAR struct cache_test_s *)
    ((FAR uint8_t *)module - offsetof(struct cache_test_s, module));

  pnut_settings_disconnect(&t->other);
  pnut_test_prefs_close(&t->prefs);
}

static void cache_test_run(FAR struct cache_test_s *t)
{
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir      = t->dir;
  sconfig.defaults = t->defaults[0] != '\0' ? t->defaults : NULL;
  sconfig.limits   = g_limits;
  sconfig.delay    = 20;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = cache_test_start;
  t->module.stop  = cache_test_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  t->step     = 0;
  t->nchanges = 0;
  pnut_timer_start(t->loop, 3000, 0, cache_test_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(t->loop), 0);

  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);
}

/* Write a defaults file: pnut.SettingsDefaults, as tools/settings-defaults
 * writes it
 */

static FAR const pnut_settings_default_t *g_defaults;
static int g_ndefaults;

static bool cache_test_encode(FAR pb_ostream_t *stream,
                              FAR const pb_field_t *field,
                              FAR void * const *arg)
{
  int i;

  for (i = 0; i < g_ndefaults; i++)
    {
      if (!pb_encode_tag_for_field(stream, field) ||
          !pb_encode_submessage(stream, PNUT_SETTINGS_DEFAULT_FIELDS,
                                &g_defaults[i]))
        {
          return false;
        }
    }

  return true;
}

static void cache_test_write_defaults(FAR const char *path,
                                      FAR const pnut_settings_default_t *d,
                                      int n)
{
  pnut_settings_defaults_t msg;
  uint8_t buffer[1024];
  pb_ostream_t stream;
  FILE *f;

  g_defaults = d;
  g_ndefaults = n;
  memset(&msg, 0, sizeof(msg));
  msg.defaults.funcs.encode = cache_test_encode;
  stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
  assert_true(pb_encode(&stream, PNUT_SETTINGS_DEFAULTS_FIELDS, &msg));

  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buffer, 1, stream.bytes_written, f),
                   stream.bytes_written);
  fclose(f);
}

static pnut_settings_default_t cache_test_default(FAR const char *key,
                                                  pb_size_t which,
                                                  int64_t integer,
                                                  FAR const char *text)
{
  pnut_settings_default_t d;

  memset(&d, 0, sizeof(d));
  strlcpy(d.owner, PNUT_TEST_PREFS_OWNER, sizeof(d.owner));
  strlcpy(d.key, key, sizeof(d.key));
  d.has_value         = true;
  d.value.which_value = which;
  if (which == PNUT_SETTING_VALUE_TEXT_TAG)
    {
      strlcpy(d.value.value.text, text, sizeof(d.value.value.text));
    }
  else
    {
      d.value.value.integer = integer;
    }

  return d;
}

/* Remove a test's directory, its settings' file and the topic's */

static void cache_test_clean(FAR struct cache_test_s *t)
{
  char path[160];

  snprintf(path, sizeof(path), "%s/%s", t->dir, PNUT_TEST_PREFS_OWNER);
  unlink(path);
  if (t->defaults[0] != '\0')
    {
      unlink(t->defaults);
    }

  snprintf(path, sizeof(path), "%s/topic.settings/last", t->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.settings", t->dir);
  rmdir(path);
  rmdir(t->dir);
}

/* Defaults at once; then Settings' values, changes by the program and by
 * others; and the values Settings kept, after a restart
 */

static void test_settings_cache(void **state)
{
  FAR struct cache_test_s *t = calloc(1, sizeof(*t));

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-cache-%d", (int)getpid());
  assert_int_equal(mkdir(t->dir, 0700), 0);

  cache_test_run(t);
  assert_true(t->defaults_seen);
  assert_int_equal(t->step, 5);
  assert_int_equal(t->nchanges, 4);
  assert_int_equal(t->changes[0], PNUT_SETTINGS_CACHE_ALL);
  assert_int_equal(t->changes[1], PNUT_TEST_PREFS_LEVEL);
  assert_int_equal(t->changes[2], PNUT_TEST_PREFS_COLOUR);
  assert_int_equal(t->changes[3], PNUT_TEST_PREFS_BIG);

  /* Again, on the files Settings wrote */

  t->restarted = true;
  cache_test_run(t);
  assert_int_equal(t->step, 1);
  assert_int_equal(t->changes[0], PNUT_SETTINGS_CACHE_ALL);
  assert_int_equal(t->prefs.values.level, 15);
  assert_string_equal(t->prefs.values.colour, "blue");
  assert_true(t->prefs.values.big == 7000000000);
  assert_true(t->prefs.values.loud);
  assert_string_equal(t->prefs.values.name, "pnut");

  cache_test_clean(t);
  free(t);
}

/* The target's defaults win over the schema's where they fit; one that
 * does not fit, or names another owner, is left aside
 */

static void test_settings_cache_defaults(void **state)
{
  FAR struct cache_test_s *t = calloc(1, sizeof(*t));
  pnut_settings_default_t d[5];

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-defaults-%d", (int)getpid());
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->defaults, sizeof(t->defaults), "%s/defaults.pb", t->dir);

  d[0] = cache_test_default("level", PNUT_SETTING_VALUE_INTEGER_TAG, 12,
                            NULL);
  d[1] = cache_test_default("colour", PNUT_SETTING_VALUE_TEXT_TAG, 0,
                            "blue");
  d[2] = cache_test_default("big.number", PNUT_SETTING_VALUE_INTEGER_TAG,
                            -1, NULL);
  d[3] = cache_test_default("loud", PNUT_SETTING_VALUE_TEXT_TAG, 0, "yes");
  d[4] = cache_test_default("name", PNUT_SETTING_VALUE_TEXT_TAG, 0,
                            "other");
  strlcpy(d[4].owner, "test.other", sizeof(d[4].owner));
  cache_test_write_defaults(t->defaults, d, 5);

  t->restarted = true;
  cache_test_run(t);
  assert_int_equal(t->step, 1);
  assert_int_equal(t->prefs.values.level, 12);
  assert_string_equal(t->prefs.values.colour, "blue");
  assert_true(t->prefs.values.big == 5000000000);
  assert_true(t->prefs.values.loud);
  assert_string_equal(t->prefs.values.name, "pnut");

  cache_test_clean(t);
  free(t);
}

/* An index out of range is refused before anything is sent */

static void test_settings_cache_bad_index(void **state)
{
  struct pnut_settings_cache_s cache;
  pnut_setting_value_t value;

  memset(&cache, 0, sizeof(cache));
  memset(&value, 0, sizeof(value));
  cache.desc = &g_pnut_test_prefs_desc;
  assert_int_equal(pnut_settings_cache_set(&cache, -1, &value, NULL, NULL),
                   -EINVAL);
  assert_int_equal(pnut_settings_cache_set(&cache,
                                           PNUT_TEST_PREFS_NSETTINGS,
                                           &value, NULL, NULL),
                   -EINVAL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_settings_cache),
    cmocka_unit_test(test_settings_cache_defaults),
    cmocka_unit_test(test_settings_cache_bad_index),
  };

  return cmocka_run_group_tests_name("settings_cache", tests, NULL, NULL);
}
