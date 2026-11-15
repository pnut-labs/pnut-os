/****************************************************************************
 * pnut-os/tests/unit/test_ui_status.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The device's state for the header (status.c): from the telephony, wifi
 * and time topics, unknown until a message comes, told only when it
 * changes, and the clock's minutes.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

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
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/power.topics.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/wifi.topics.h"

#include "status.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 2026-09-22 12:06:59 UTC: 14:06:59 in Warsaw (summer time) */

#define STATUS_NOW   1790078819
#define STATUS_RULE  "CET-1CEST,M3.5.0,M10.5.0/3"

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

static struct ui_status_s g_status;
static FAR struct pnut_publisher_s *g_tel;
static FAR struct pnut_publisher_s *g_wifi;
static FAR struct pnut_publisher_s *g_time;
static FAR struct pnut_publisher_s *g_battery;
static int g_changes[16];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t fixed_now(void)
{
  return STATUS_NOW;
}

static void changed(FAR void *arg, uint8_t what)
{
  int i;

  for (i = 0; i < 8; i++)
    {
      if ((what & (1 << i)) != 0)
        {
          g_changes[i]++;
        }
    }
}

static int count(uint8_t what)
{
  int i = 0;

  while ((1 << i) != what)
    {
      i++;
    }

  return g_changes[i];
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
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-status-XXXXXX";
  char cmd[64];
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&config);
  config.rundir = dir;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);

  assert_int_equal(pnut_telephony_info_advertise(loop, &g_tel), 0);
  assert_int_equal(pnut_wifi_info_advertise(loop, &g_wifi), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &g_time), 0);
  assert_int_equal(pnut_battery_info_advertise(loop, &g_battery), 0);

  memset(g_changes, 0, sizeof(g_changes));
  assert_int_equal(status_start(&g_status, loop, fixed_now, changed, NULL),
                   0);

  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  status_stop(&g_status);
  pnut_topic_unadvertise(g_tel);
  pnut_topic_unadvertise(g_wifi);
  pnut_topic_unadvertise(g_time);
  pnut_topic_unadvertise(g_battery);
  pnut_loop_destroy(loop);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  assert_int_equal(system(cmd), 0);
}

/* test_status_topics */

static void topics_none(void)
{
  assert_false(g_status.has_tel);
  assert_false(g_status.has_wifi);
  assert_false(g_status.has_time);
  assert_false(g_status.has_battery);
}

static void topics_publish(void)
{
  pnut_battery_info_t battery;
  pnut_telephony_info_t tel;
  pnut_wifi_info_t wifi;

  memset(&battery, 0, sizeof(battery));
  battery.level    = 84;
  battery.charging = true;
  assert_int_equal(pnut_battery_info_publish(g_battery, &battery), 0);

  memset(&tel, 0, sizeof(tel));
  tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.bars  = 3;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);

  memset(&wifi, 0, sizeof(wifi));
  wifi.state = PNUT_WIFI_INFO_STATE_JOINED;
  strcpy(wifi.name, "Home");
  assert_int_equal(pnut_wifi_info_publish(g_wifi, &wifi), 0);
}

static void topics_known(void)
{
  assert_true(g_status.has_tel);
  assert_int_equal(g_status.tel.bars, 3);
  assert_int_equal(count(STATUS_TEL), 1);
  assert_true(g_status.has_wifi);
  assert_string_equal(g_status.wifi.name, "Home");
  assert_int_equal(count(STATUS_WIFI), 1);
  assert_true(g_status.has_battery);
  assert_int_equal(g_status.battery, 84);
  assert_true(g_status.charging);
  assert_int_equal(count(STATUS_BATTERY), 1);

  /* The same again: nothing changed, no one told */

  topics_publish();
}

static void topics_same(void)
{
  pnut_telephony_info_t tel;

  assert_int_equal(count(STATUS_TEL), 1);
  assert_int_equal(count(STATUS_WIFI), 1);
  assert_int_equal(count(STATUS_BATTERY), 1);

  memset(&tel, 0, sizeof(tel));
  tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.bars  = 4;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);
}

static void topics_changed(void)
{
  assert_int_equal(count(STATUS_TEL), 2);
  assert_int_equal(g_status.tel.bars, 4);
}

static void test_status_topics(void **state)
{
  static const struct step_s steps[] =
  {
    { 50, topics_none },
    { 100, topics_publish },
    { 300, topics_known },
    { 500, topics_same },
    { 700, topics_changed },
    { 800, NULL },
  };

  run(steps, 6);
}

/* test_status_time: local time from the zone's rule; a minute begins */

static void time_utc(void)
{
  struct tm tm;
  pnut_time_change_t msg;

  /* No zone yet: UTC, and the time is not known */

  assert_false(g_status.has_time);
  status_local(&g_status, status_now(&g_status), &tm);
  assert_int_equal(tm.tm_hour, 12);

  memset(&msg, 0, sizeof(msg));
  strcpy(msg.zone, "Europe/Warsaw");
  strcpy(msg.rule, STATUS_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &msg), 0);
}

static void time_local(void)
{
  struct tm tm;

  assert_true(g_status.has_time);
  assert_true(count(STATUS_TIME) >= 1);
  status_local(&g_status, status_now(&g_status), &tm);
  assert_int_equal(tm.tm_hour, 14);
  assert_int_equal(tm.tm_min, 6);
  assert_int_equal(tm.tm_wday, 2);
}

static void time_minute(void)
{
  /* The clock is fixed at hh:mm:59: a minute began a second later, and
   * again each minute (not within this test)
   */

  assert_int_equal(count(STATUS_TIME), 2);
}

static void test_status_time(void **state)
{
  static const struct step_s steps[] =
  {
    { 50, time_utc },
    { 300, time_local },
    { 1500, time_minute },
    { 1600, NULL },
  };

  run(steps, 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_status_topics),
    cmocka_unit_test(test_status_time),
  };

  return cmocka_run_group_tests_name("ui_status", tests, NULL, NULL);
}
