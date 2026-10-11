/****************************************************************************
 * pnut-os/tests/unit/test_power.c
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
#include <string.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "power.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_INTERVAL  5      /* ms between readings */
#define TEST_CUTOFF    3300   /* mV */
#define TEST_READS     3

/* A reading that fails rather than gives a voltage */

#define TEST_FAIL      -1

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The readings the battery gives, in turn; the last one repeats */

struct battery_s
{
  FAR const int *mv;
  FAR const bool *external;
  int count;
  int calls;
  int error;                      /* What a TEST_FAIL reading returns */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int measure(FAR void *arg, FAR int *mv, FAR bool *external)
{
  FAR struct battery_s *battery = arg;
  int i = battery->calls < battery->count ? battery->calls :
                                            battery->count - 1;

  battery->calls++;
  if (battery->mv[i] == TEST_FAIL)
    {
      return battery->error;
    }

  *mv       = battery->mv[i];
  *external = battery->external != NULL && battery->external[i];
  return 0;
}

static void stop_handler(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

/* Run the module until it stops the loop, or for ms at most */

static bool run(FAR struct battery_s *battery, int cutoff, uint32_t ms)
{
  struct power_config_s config;
  struct power_s power;
  FAR struct pnut_loop_s *loop;
  bool off;

  memset(&config, 0, sizeof(config));
  config.interval = TEST_INTERVAL;
  config.cutoff   = cutoff;
  config.reads    = TEST_READS;
  config.measure  = measure;
  config.arg      = battery;
  power_init(&power, &config);

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &power.module), 0);
  assert_int_equal(pnut_timer_start(loop, ms, 0, stop_handler, NULL, NULL),
                   0);
  assert_int_equal(pnut_loop_run(loop), 0);
  off = power_wants_off(&power);
  pnut_loop_destroy(loop);
  return off;
}

/* Three readings in a row at or under the cutoff, on battery: off.  The
 * first reading, at the start, only checks for a battery.
 */

static void test_power_cutoff(void **state)
{
  static const int mv[] =
  {
    3700, 3300, 3290, 3280, 3270
  };

  struct battery_s battery =
  {
    mv, NULL, 5, 0, 0
  };

  assert_true(run(&battery, TEST_CUTOFF, 1000));
  assert_int_equal(battery.calls, 1 + TEST_READS);
}

/* A reading over the cutoff starts the count again */

static void test_power_dip(void **state)
{
  static const int mv[] =
  {
    3700, 3200, 3200, 3400, 3200, 3200, 3200
  };

  struct battery_s battery =
  {
    mv, NULL, 7, 0, 0
  };

  assert_true(run(&battery, TEST_CUTOFF, 1000));
  assert_int_equal(battery.calls, 7);
}

/* On external power a low battery is charging: never off */

static void test_power_external(void **state)
{
  static const int mv[] =
  {
    3100
  };

  static const bool external[] =
  {
    true
  };

  struct battery_s battery =
  {
    mv, external, 1, 0, 0
  };

  assert_false(run(&battery, TEST_CUTOFF, 100));
  assert_true(battery.calls > 1 + TEST_READS);
}

/* External power arriving while the count runs starts it again */

static void test_power_external_arrives(void **state)
{
  static const int mv[] =
  {
    3700, 3200, 3200, 3200, 3200, 3200, 3200
  };

  static const bool external[] =
  {
    false, false, false, true, false, false, false
  };

  struct battery_s battery =
  {
    mv, external, 7, 0, 0
  };

  assert_true(run(&battery, TEST_CUTOFF, 1000));
  assert_int_equal(battery.calls, 7);
}

/* A failed reading neither counts nor starts the count again */

static void test_power_failed_reading(void **state)
{
  static const int mv[] =
  {
    3700, 3200, TEST_FAIL, 3200, 3200
  };

  struct battery_s battery =
  {
    mv, NULL, 5, 0, -EIO
  };

  assert_true(run(&battery, TEST_CUTOFF, 1000));
  assert_int_equal(battery.calls, 5);
}

/* No battery at the start: no readings after */

static void test_power_no_battery(void **state)
{
  static const int mv[] =
  {
    TEST_FAIL
  };

  struct battery_s battery =
  {
    mv, NULL, 1, 0, -ENODEV
  };

  assert_false(run(&battery, TEST_CUTOFF, 50));
  assert_int_equal(battery.calls, 1);
}

/* A battery gone later ends the readings */

static void test_power_battery_gone(void **state)
{
  static const int mv[] =
  {
    3700, 3200, TEST_FAIL
  };

  struct battery_s battery =
  {
    mv, NULL, 3, 0, -ENODEV
  };

  assert_false(run(&battery, TEST_CUTOFF, 100));
  assert_int_equal(battery.calls, 3);
}

/* A cutoff of zero reads nothing */

static void test_power_no_cutoff(void **state)
{
  static const int mv[] =
  {
    3000
  };

  struct battery_s battery =
  {
    mv, NULL, 1, 0, 0
  };

  assert_false(run(&battery, 0, 50));
  assert_int_equal(battery.calls, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_power_cutoff),
    cmocka_unit_test(test_power_dip),
    cmocka_unit_test(test_power_external),
    cmocka_unit_test(test_power_external_arrives),
    cmocka_unit_test(test_power_failed_reading),
    cmocka_unit_test(test_power_no_battery),
    cmocka_unit_test(test_power_battery_gone),
    cmocka_unit_test(test_power_no_cutoff),
  };

  return cmocka_run_group_tests_name("power", tests, NULL, NULL);
}
