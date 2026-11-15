/****************************************************************************
 * pnut-os/tests/unit/test_ui_setting.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Other programs' settings, as the system UI reads and changes them
 * (setting.c): through Settings' own calls, not an owner's cache, and read
 * again when the settings topic tells of a change.  Settings runs in the
 * test's loop; Wi-Fi's and Telephony's schemas are registered by caches of
 * their own, standing in for their programs.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/telephony.settings.h"
#include "pnut/wifi.settings.h"

#include "settings.h"
#include "setting.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_ENABLED    0
#define TEL_ENABLED     1
#define TEL_DATA        2
#define TEL_APN         3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct step_s
{
  uint32_t at;
  CODE void (*run)(void);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Wi-Fi's 49 settings (16 known networks, three each) and Telephony's */

static const struct settings_limits_s g_limits =
{
  .owners   = 4,
  .settings = 128,
  .texts    = 96,
  .choices  = 64,
};

static struct settings_s g_settings;
static struct pnut_module_s g_owners;
static struct pnut_wifi_settings_s g_wifi;
static struct pnut_telephony_settings_s g_tel;
static struct setting_watch_s g_watch;
static struct setting_s g_list[4];
static int g_changes[4];
static int g_done;
static int g_status;
static bool g_unknown;            /* Settings cannot tell the caller now */
static bool g_watch_later;        /* The watch opened by a step */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void changed(FAR void *arg, int index)
{
  g_changes[index]++;
}

static void done(FAR void *arg, int status, uint32_t code)
{
  g_done++;
  g_status = status;
}

static void owner_changed(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
}

/* Who calls: the UI, owner of all (one process: every caller is the
 * same); or, while g_unknown, not to be told now
 */

static int fake_who(FAR void *arg, pid_t pid, FAR const char **program)
{
  *program = "ui";
  return g_unknown ? -EAGAIN : 0;
}

static bool fake_runs(FAR void *arg, FAR const char *program,
                      FAR const char *service)
{
  return true;
}

/* The owners' caches, and the UI's watch */

static int owners_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  memset(g_list, 0, sizeof(g_list));
  g_list[WIFI_ENABLED].owner = "wifi";
  g_list[WIFI_ENABLED].key   = "enabled";
  g_list[TEL_ENABLED].owner  = "telephony";
  g_list[TEL_ENABLED].key    = "enabled";
  g_list[TEL_DATA].owner     = "telephony";
  g_list[TEL_DATA].key       = "data";
  g_list[TEL_APN].owner      = "telephony";
  g_list[TEL_APN].key        = "apn";

  pnut_module_ready(module);
  assert_int_equal(pnut_wifi_settings_open(module, owner_changed, NULL,
                                           &g_wifi), 0);
  assert_int_equal(pnut_telephony_settings_open(module, owner_changed,
                                                NULL, &g_tel), 0);
  return g_watch_later ? 0 : setting_open(&g_watch, loop, g_list, 4,
                                          changed, NULL);
}

static void owners_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  setting_close(&g_watch);
  pnut_telephony_settings_close(&g_tel);
  pnut_wifi_settings_close(&g_wifi);
}

static void step_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR const struct step_s *step = arg;

  if (step->run != NULL)
    {
      step->run();
    }
  else
    {
      pnut_loop_stop(loop, 0);
    }
}

static void run(FAR const struct step_s *steps, int nsteps)
{
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-setting-XXXXXX";
  char cmd[64];
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&config);
  config.rundir = dir;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = dir;
  sconfig.limits = g_limits;
  sconfig.delay  = 20;
  sconfig.who    = fake_who;
  sconfig.runs   = fake_runs;
  settings_init(&g_settings, &sconfig);
  assert_int_equal(pnut_module_add(loop, &g_settings.module), 0);

  memset(&g_owners, 0, sizeof(g_owners));
  g_owners.name  = "owners";
  g_owners.start = owners_start;
  g_owners.stop  = owners_stop;
  assert_int_equal(pnut_module_add(loop, &g_owners), 0);

  memset(g_changes, 0, sizeof(g_changes));
  g_done = 0;
  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_loop_destroy(loop);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  assert_int_equal(system(cmd), 0);
}

/* test_setting_read_and_set */

static void read_known(void)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      assert_true(g_list[i].known);
      assert_true(g_changes[i] >= 1);
    }

  assert_false(g_list[WIFI_ENABLED].value.value.boolean);
  assert_true(g_list[TEL_ENABLED].value.value.boolean);
  assert_false(g_list[TEL_DATA].value.value.boolean);
  assert_string_equal(g_list[TEL_APN].value.value.text, "");

  memset(g_changes, 0, sizeof(g_changes));
  assert_int_equal(setting_set_bool(&g_watch, WIFI_ENABLED, true, done,
                                    NULL), 0);
  assert_int_equal(setting_set_text(&g_watch, TEL_APN, "internet.example",
                                    done, NULL), 0);
}

static void read_set(void)
{
  assert_int_equal(g_done, 2);
  assert_int_equal(g_status, 0);
  assert_true(g_list[WIFI_ENABLED].value.value.boolean);
  assert_int_equal(g_changes[WIFI_ENABLED], 1);
  assert_string_equal(g_list[TEL_APN].value.value.text,
                      "internet.example");
  assert_int_equal(g_changes[TEL_APN], 1);

  /* The others did not change: not told */

  assert_int_equal(g_changes[TEL_ENABLED], 0);
  assert_int_equal(g_changes[TEL_DATA], 0);

  /* Changed by another program (the owner): read again */

  assert_int_equal(pnut_wifi_settings_set_enabled(&g_wifi, false, NULL,
                                                  NULL), 0);
}

static void read_other(void)
{
  assert_false(g_list[WIFI_ENABLED].value.value.boolean);
  assert_int_equal(g_changes[WIFI_ENABLED], 2);
}

static void test_setting_read_and_set(void **state)
{
  static const struct step_s steps[] =
  {
    { 600, read_known },
    { 1200, read_set },
    { 1800, read_other },
    { 1900, NULL },
  };

  run(steps, 4);
}

/* test_setting_refused: a value Settings refuses is told, not kept */

static void refused_set(void)
{
  assert_int_equal(setting_set_text(&g_watch, WIFI_ENABLED, "maybe", done,
                                    NULL), 0);
}

static void refused_told(void)
{
  assert_int_equal(g_done, 1);
  assert_true(g_status != 0);
  assert_false(g_list[WIFI_ENABLED].value.value.boolean);
}

static void test_setting_refused(void **state)
{
  static const struct step_s steps[] =
  {
    { 600, refused_set },
    { 1200, refused_told },
    { 1300, NULL },
  };

  run(steps, 3);
}

/* test_setting_retried: a read Settings cannot answer now (the caller not
 * told yet) is tried again
 */

static void retried_open(void)
{
  g_unknown = true;
  assert_int_equal(setting_open(&g_watch, g_owners.loop,
                                g_list, 4, changed, NULL), 0);
}

static void retried_unknown(void)
{
  assert_false(g_list[WIFI_ENABLED].known);
  g_unknown = false;
}

static void retried_known(void)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      assert_true(g_list[i].known);
    }
}

static void test_setting_retried(void **state)
{
  static const struct step_s steps[] =
  {
    { 400, retried_open },
    { 800, retried_unknown },
    { 2400, retried_known },
    { 2500, NULL },
  };

  g_watch_later = true;
  run(steps, 4);
  g_watch_later = false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_setting_read_and_set),
    cmocka_unit_test(test_setting_refused),
    cmocka_unit_test(test_setting_retried),
  };

  return cmocka_run_group_tests_name("ui_setting", tests, NULL, NULL);
}
