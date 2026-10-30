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

#include <pnut/log.h>
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
  bool full;                      /* What the cache said, as it stopped */
  uint32_t backoff;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Room for the levels test's four owners, each with a log level's four
 * choices
 */

static const struct settings_limits_s g_limits =
{
  .owners   = 4,
  .settings = 32,
  .texts    = 16,
  .choices  = 32,
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

static void cache_test_clean(FAR const char *dir, FAR const char *defaults)
{
  char path[160];

  snprintf(path, sizeof(path), "%s/%s.pb", dir, PNUT_TEST_PREFS_OWNER);
  unlink(path);
  if (defaults[0] != '\0')
    {
      unlink(defaults);
    }

  snprintf(path, sizeof(path), "%s/topic.settings/last", dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.settings", dir);
  rmdir(path);
  rmdir(dir);
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

  cache_test_clean(t->dir, t->defaults);
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

  cache_test_clean(t->dir, t->defaults);
  free(t);
}

/* Settings with no room for the owner's schema: the cache keeps its
 * defaults and tries again, waiting longer each time, and says so once.
 * On the computer the log is the standard error, caught to count the
 * lines, the module's debug ones among them; NuttX's goes to syslog.
 */

#ifndef __NuttX__
static int cache_test_count(FAR const char *s, FAR const char *what)
{
  int n = 0;

  while ((s = strstr(s, what)) != NULL)
    {
      n++;
      s += strlen(what);
    }

  return n;
}
#endif

static void cache_test_full_stop(FAR struct pnut_loop_s *loop,
                                 FAR struct pnut_timer_s *timer,
                                 FAR void *arg)
{
  FAR struct cache_test_s *t = arg;

  t->full    = t->prefs.cache.full;
  t->backoff = t->prefs.cache.backoff;
  pnut_loop_stop(loop, 0);
}

static int cache_test_full_start(FAR struct pnut_module_s *module,
                                 FAR struct pnut_loop_s *loop)
{
  FAR struct cache_test_s *t = (FAR struct cache_test_s *)
    ((FAR uint8_t *)module - offsetof(struct cache_test_s, module));
  int ret;

  ret = cache_test_start(module, loop);
  if (ret < 0)
    {
      return ret;
    }

  return pnut_timer_start(loop, 1000, 0, cache_test_full_stop, t, NULL);
}

static void test_settings_cache_full(void **state)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 2,
    .texts    = 16,
    .choices  = 32,
  };

  FAR struct cache_test_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  int ret;
#ifndef __NuttX__
  static char out[16384];
  FAR FILE *file;
  size_t n;
  int saved;
#endif

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-full-%d", (int)getpid());
  assert_int_equal(mkdir(t->dir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->dir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = cache_test_full_start;
  t->module.stop  = cache_test_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);
  pnut_module_setlevel(&t->module, PNUT_LOG_DEBUG);

#ifndef __NuttX__
  fflush(stderr);
  file = tmpfile();
  assert_non_null(file);
  saved = dup(STDERR_FILENO);
  assert_true(saved >= 0);
  assert_true(dup2(fileno(file), STDERR_FILENO) >= 0);
#endif

  /* The standard error comes back before any check */

  ret = pnut_loop_run(t->loop);

#ifndef __NuttX__
  fflush(stderr);
  dup2(saved, STDERR_FILENO);
  close(saved);
  rewind(file);
  n = fread(out, 1, sizeof(out) - 1, file);
  out[n] = '\0';
  fclose(file);
  fputs(out, stderr);
#endif

  assert_int_equal(ret, 0);
  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);

  /* Still at the defaults, and tried again several times: from the first
   * wait of 100 ms, it doubled to 800 ms at least (failures near 0, 100,
   * 300 and 700 ms make it 1600 ms by the stop)
   */

  assert_true(t->full);
  assert_int_equal(t->nchanges, 0);
  assert_int_equal(t->prefs.values.level, 10);
  assert_true(t->backoff >= 800);

  /* Told once: no line for the retries, not even a debug one */

#ifndef __NuttX__
  assert_int_equal(cache_test_count(out, "no room left in Settings"), 1);
  assert_int_equal(cache_test_count(out, "Settings of test.prefs"), 1);
#endif

  cache_test_clean(t->dir, "");
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

/* Modules' log levels as settings (RFC 0026), with the loop's levels: one
 * module holds its own as the owner named as it, the others have caches
 * of the level alone, Settings' own module too; four caches over one
 * connection, where a program may hold two
 */

struct levels_test_s
{
  FAR struct pnut_loop_s *loop;
  struct settings_s settings;
  struct pnut_module_s prefs_module;     /* Named as its owner */
  struct pnut_module_s quiet;            /* No settings of its own */
  struct pnut_module_s plain;            /* No settings of its own */
  struct pnut_test_prefs_s prefs;
  struct pnut_settings_client_s other;   /* Another client of Settings */
  char dir[64];
  char defaults[96];
  int step;
  int nchanges;                   /* The prefs' handler's calls */
  int status;                     /* The last set's answer */
  int done;                       /* Set handlers called */
  bool prefs_open;
  bool late;                      /* The owner opened after the start */
};

static void levels_test_changed(FAR struct pnut_settings_cache_s *cache,
                                int index, FAR void *arg)
{
  FAR struct levels_test_s *t = arg;

  t->nchanges++;
}

static void levels_test_other_done(FAR struct pnut_settings_client_s *c,
                                   int status,
                                   FAR const pnut_settings_reply_t *out,
                                   FAR const pnut_error_t *err,
                                   FAR void *arg)
{
  FAR struct levels_test_s *t = arg;

  t->status = status;
}

static void levels_test_set_done(FAR struct pnut_settings_cache_s *cache,
                                 int status, uint32_t code, FAR void *arg)
{
  FAR struct levels_test_s *t = arg;

  t->done++;
}

static void levels_test_set(FAR struct levels_test_s *t,
                            FAR const char *owner, FAR const char *value)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, owner, sizeof(req.owner));
  strlcpy(req.key, PNUT_SETTINGS_CACHE_LEVEL, sizeof(req.key));
  req.has_value         = true;
  req.value.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  strlcpy(req.value.value.text, value, sizeof(req.value.value.text));
  t->status = -1;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0,
                                     levels_test_other_done, t), 0);
}

/* Every 20 ms: on to the next step once the one before is seen */

static void levels_test_poll(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct levels_test_s *t = arg;

  switch (t->step)
    {
      case 0:
        if (t->late && !t->prefs_open)
          {
            /* The library's cache of its level first; then the module's
             * own owner, which takes the level over
             */

            if (t->prefs_module.levels == NULL ||
                !pnut_settings_cache_ready(t->prefs_module.levels))
              {
                break;
              }

            assert_int_equal(pnut_test_prefs_open(&t->prefs_module,
                                                  levels_test_changed, t,
                                                  &t->prefs), 0);
            t->prefs_open = true;
            assert_null(t->prefs_module.levels);
            break;
          }

        /* Every cache has read its values */

        if (!pnut_settings_cache_ready(&t->prefs.cache) ||
            t->quiet.levels == NULL ||
            !pnut_settings_cache_ready(t->quiet.levels) ||
            t->plain.levels == NULL ||
            !pnut_settings_cache_ready(t->plain.levels) ||
            t->settings.module.levels == NULL ||
            !pnut_settings_cache_ready(t->settings.module.levels))
          {
            break;
          }

        /* The owner named as its module holds the level itself; the
         * target's default reaches a module without settings
         */

        assert_null(t->prefs_module.levels);
        assert_int_equal(t->prefs_module.level, PNUT_LOG_INFO);
        assert_int_equal(t->quiet.level, PNUT_LOG_WARNING);
        assert_int_equal(t->plain.level, PNUT_LOG_INFO);
        assert_int_equal(t->settings.module.level, PNUT_LOG_INFO);
        assert_int_equal(t->nchanges, 1);

        levels_test_set(t, PNUT_TEST_PREFS_OWNER, "debug");
        t->step++;
        break;

      case 1:
        if (t->prefs_module.level != PNUT_LOG_DEBUG)
          {
            break;
          }

        /* Not one of the owner's own settings: its handler is not told */

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->nchanges, 1);
        assert_int_equal(t->quiet.level, PNUT_LOG_WARNING);
        levels_test_set(t, "plain", "error");
        t->step++;
        break;

      case 2:
        if (t->plain.level != PNUT_LOG_ERROR)
          {
            break;
          }

        /* Not a level: refused, and nothing changes */

        levels_test_set(t, "plain", "loud");
        t->step++;
        break;

      case 3:
        if (t->status < 0)
          {
            break;
          }

        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->plain.level, PNUT_LOG_ERROR);

        /* A cache closed with a change in flight: its answer is dropped,
         * the other caches going on over the connection
         */

        assert_int_equal(pnut_test_prefs_set_level(&t->prefs, 14,
                                                   levels_test_set_done,
                                                   t), 0);
        pnut_test_prefs_close(&t->prefs);
        t->prefs_open = false;
        levels_test_set(t, "quiet", "debug");
        t->step++;
        break;

      case 4:
        if (t->quiet.level != PNUT_LOG_DEBUG)
          {
            break;
          }

        assert_int_equal(t->done, 0);
        t->step++;
        pnut_loop_stop(loop, 0);
        break;

      default:
        break;
    }
}

static int levels_test_start(FAR struct pnut_module_s *module,
                             FAR struct pnut_loop_s *loop)
{
  FAR struct levels_test_s *t = (FAR struct levels_test_s *)
    ((FAR uint8_t *)module - offsetof(struct levels_test_s, prefs_module));
  int ret;

  if (!t->late)
    {
      ret = pnut_test_prefs_open(module, levels_test_changed, t,
                                 &t->prefs);
      if (ret < 0)
        {
          return ret;
        }

      t->prefs_open = true;
    }

  pnut_module_ready(module);
  ret = pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                              &t->other);
  if (ret < 0)
    {
      return ret;
    }

  return pnut_timer_start(loop, 20, 20, levels_test_poll, t, NULL);
}

static void levels_test_stop(FAR struct pnut_module_s *module,
                             FAR struct pnut_loop_s *loop)
{
  FAR struct levels_test_s *t = (FAR struct levels_test_s *)
    ((FAR uint8_t *)module - offsetof(struct levels_test_s, prefs_module));

  pnut_settings_disconnect(&t->other);
  if (t->prefs_open)
    {
      pnut_test_prefs_close(&t->prefs);
    }
}

static int levels_test_ready(FAR struct pnut_module_s *module,
                             FAR struct pnut_loop_s *loop)
{
  pnut_module_ready(module);
  return OK;
}

static void levels_test_run(bool late)
{
  FAR struct levels_test_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  pnut_settings_default_t d;
  char path[160];

  assert_non_null(t);
  t->late = late;
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-levels-%d", (int)getpid());
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->defaults, sizeof(t->defaults), "%s/defaults.pb", t->dir);

  d = cache_test_default(PNUT_SETTINGS_CACHE_LEVEL,
                         PNUT_SETTING_VALUE_TEXT_TAG, 0, "warning");
  strlcpy(d.owner, "quiet", sizeof(d.owner));
  cache_test_write_defaults(t->defaults, &d, 1);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  config.levels = true;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir      = t->dir;
  sconfig.defaults = t->defaults;
  sconfig.limits   = g_limits;
  sconfig.delay    = 20;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  t->prefs_module.name  = PNUT_TEST_PREFS_OWNER;
  t->prefs_module.start = levels_test_start;
  t->prefs_module.stop  = levels_test_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->prefs_module), 0);

  t->quiet.name  = "quiet";
  t->quiet.start = levels_test_ready;
  assert_int_equal(pnut_module_add(t->loop, &t->quiet), 0);

  t->plain.name  = "plain";
  t->plain.start = levels_test_ready;
  assert_int_equal(pnut_module_add(t->loop, &t->plain), 0);

  pnut_timer_start(t->loop, 3000, 0, cache_test_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(t->loop), 0);
  assert_int_equal(t->step, 5);

  /* Stopped: the library's caches are gone */

  assert_null(t->quiet.levels);
  assert_null(t->plain.levels);
  assert_null(t->settings.module.levels);

  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);

  snprintf(path, sizeof(path), "%s/quiet.pb", t->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/plain.pb", t->dir);
  unlink(path);
  cache_test_clean(t->dir, t->defaults);
  free(t);
}

static void test_settings_cache_levels(void **state)
{
  levels_test_run(false);
}

/* The same, with the owner named as a module opened after the start: the
 * library's cache of its level gives way to it
 */

static void test_settings_cache_levels_late(void **state)
{
  levels_test_run(true);
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
    cmocka_unit_test(test_settings_cache_full),
    cmocka_unit_test(test_settings_cache_levels),
    cmocka_unit_test(test_settings_cache_levels_late),
  };

  return cmocka_run_group_tests_name("settings_cache", tests, NULL, NULL);
}
