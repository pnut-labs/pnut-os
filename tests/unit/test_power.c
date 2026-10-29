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

#include "board.h"
#include "power.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_INTERVAL  5      /* ms between readings */
#define TEST_CUTOFF    3300   /* mV */
#define TEST_READS     3

/* A reading that fails rather than gives a voltage */

#define TEST_FAIL      -1

/* A file every computer has, opened as the gauge or the charger, and one
 * none has
 */

#define TEST_DEVICE    "/dev/null"
#define TEST_MISSING   "/dev/pnut-none"

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
  int chargers;                   /* Readings given a charger */
};

/* What the description has besides the battery's power-off voltage */

enum parts_e
{
  PARTS_BOTH,                     /* The gauge's and the charger's files */
  PARTS_NO_CHARGER,               /* No charger */
  PARTS_MISSING_CHARGER,          /* The charger's file is not there */
  PARTS_SPARE_BATTERY,            /* A battery part without its detail
                                   * first */
  PARTS_NO_GAUGE,                 /* A battery without a device file */
  PARTS_MISSING_GAUGE             /* The gauge's file is not there */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The module run with Board: it reads the battery only once it has let go
 * of Board
 */

static FAR struct power_s *g_power;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int measure(FAR void *arg, int gauge, int charger, FAR int *mv,
                   FAR bool *external)
{
  FAR struct battery_s *battery = arg;
  int i = battery->calls < battery->count ? battery->calls :
                                            battery->count - 1;

  if (g_power != NULL)
    {
      assert_false(g_power->board_open);
    }

  assert_true(gauge >= 0);
  if (charger >= 0)
    {
      battery->chargers++;
    }

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
  FAR struct power_s *power = arg;

  /* With Board, the answer came and the module let go of it; and the
   * module holds the gauge and the charger only while it reads
   */

  if (power != NULL)
    {
      assert_false(power->board_open);
      if (power->timer == NULL)
        {
          assert_int_equal(power->gauge, -1);
          assert_int_equal(power->charger, -1);
        }
    }

  pnut_loop_stop(loop, 0);
}

/* Run the module until it stops the loop, or for ms at most */

static bool run(FAR struct battery_s *battery, int cutoff, uint32_t ms)
{
  FAR struct power_s *power = calloc(1, sizeof(*power));
  struct power_config_s config;
  FAR struct pnut_loop_s *loop;
  bool off;

  assert_non_null(power);
  memset(&config, 0, sizeof(config));
  config.interval = TEST_INTERVAL;
  config.cutoff   = cutoff;
  config.reads    = TEST_READS;
  config.gauge    = TEST_DEVICE;
  config.charger  = TEST_DEVICE;
  config.measure  = measure;
  config.arg      = battery;
  power_init(power, &config);

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &power->module), 0);
  assert_int_equal(pnut_timer_start(loop, ms, 0, stop_handler, power,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);

  /* Stopped, the module let go of the gauge */

  assert_int_equal(power->gauge, -1);
  assert_int_equal(power->charger, -1);
  off = power_wants_off(power);
  pnut_loop_destroy(loop);
  free(power);
  return off;
}

/* Run the module with Board, whose description has a battery that powers
 * off at empty_mv, or no battery (-1), or is missing (-2), and the parts
 * besides; the cutoff given is never used, nor the gauge
 */

static bool run_board(FAR struct battery_s *battery, int empty_mv,
                      enum parts_e parts, uint32_t ms)
{
  FAR pnut_board_description_t *d = calloc(1, sizeof(*d));
  FAR uint8_t *buffer = malloc(PNUT_BOARD_DESCRIPTION_SIZE);
  FAR struct board_s *board = calloc(1, sizeof(*board));
  FAR struct power_s *power = calloc(1, sizeof(*power));
  FAR pnut_board_part_t *p;
  struct pnut_loop_config_s lconfig;
  struct power_config_s config;
  struct board_config_s bconfig;
  FAR struct pnut_loop_s *loop;
  pb_ostream_t stream;
  char dir[48];
  char path[96];
  bool off;
  FILE *f;

  snprintf(dir, sizeof(dir), "/tmp/pnut-power-%d", (int)getpid());
  assert_int_equal(mkdir(dir, 0700), 0);
  snprintf(path, sizeof(path), "%s/board.pb", dir);

  assert_non_null(d);
  assert_non_null(buffer);
  assert_non_null(board);
  assert_non_null(power);
  if (parts == PARTS_SPARE_BATTERY)
    {
      d->parts[0].kind = PNUT_BOARD_PART_KIND_BATTERY;
      strlcpy(d->parts[0].name, "backup", sizeof(d->parts[0].name));
      d->parts_count = 1;
    }

  if (empty_mv != -1)
    {
      p = &d->parts[d->parts_count++];
      p->kind         = PNUT_BOARD_PART_KIND_BATTERY;
      p->which_detail = PNUT_BOARD_PART_BATTERY_TAG;
      p->detail.battery.empty_mv = empty_mv;
      if (parts == PARTS_MISSING_GAUGE)
        {
          strlcpy(p->device, TEST_MISSING, sizeof(p->device));
        }
      else if (parts != PARTS_NO_GAUGE)
        {
          strlcpy(p->device, TEST_DEVICE, sizeof(p->device));
        }
    }

  if (parts != PARTS_NO_CHARGER)
    {
      p = &d->parts[d->parts_count++];
      p->kind = PNUT_BOARD_PART_KIND_CHARGER;
      strlcpy(p->device, parts == PARTS_MISSING_CHARGER ? TEST_MISSING :
              TEST_DEVICE, sizeof(p->device));
    }

  if (empty_mv != -2)
    {
      stream = pb_ostream_from_buffer(buffer, PNUT_BOARD_DESCRIPTION_SIZE);
      assert_true(pb_encode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, d));
      f = fopen(path, "wb");
      assert_non_null(f);
      assert_int_equal(fwrite(buffer, 1, stream.bytes_written, f),
                       stream.bytes_written);
      fclose(f);
    }

  free(d);
  free(buffer);

  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);

  bconfig.path = path;
  board_init(board, &bconfig);
  assert_int_equal(pnut_module_add(loop, &board->module), 0);

  memset(&config, 0, sizeof(config));
  config.interval = TEST_INTERVAL;
  config.cutoff   = 5000;
  config.reads    = TEST_READS;
  config.board    = BOARD_SERVICE;
  config.gauge    = TEST_MISSING;
  config.measure  = measure;
  config.arg      = battery;
  power_init(power, &config);
  assert_int_equal(pnut_module_add(loop, &power->module), 0);

  g_power = power;
  assert_int_equal(pnut_timer_start(loop, ms, 0, stop_handler, power,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  g_power = NULL;
  assert_int_equal(power->gauge, -1);
  assert_int_equal(power->charger, -1);
  off = power_wants_off(power);
  pnut_loop_destroy(loop);
  free(power);
  free(board);

  unlink(path);
  assert_int_equal(rmdir(dir), 0);
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
  assert_int_equal(battery.chargers, battery.calls);
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

/* With Board, the cutoff is the battery's power-off voltage: 3300 mV,
 * not the 5000 mV given; the gauge and the charger are the battery's and
 * the charger's files
 */

static void test_power_board(void **state)
{
  static const int mv[] =
  {
    3700, 3400, 3300, 3290, 3280
  };

  struct battery_s battery =
  {
    mv, NULL, 5, 0, 0
  };

  assert_true(run_board(&battery, TEST_CUTOFF, PARTS_BOTH, 1000));
  assert_int_equal(battery.calls, 2 + TEST_READS);
  assert_int_equal(battery.chargers, battery.calls);
}

/* Without a charger in the description, or with one whose file is not
 * there, the readings go on without one
 */

static void test_power_board_no_charger(void **state)
{
  static const int mv[] =
  {
    3700, 3300, 3290, 3280
  };

  struct battery_s battery =
  {
    mv, NULL, 4, 0, 0
  };

  assert_true(run_board(&battery, TEST_CUTOFF, PARTS_NO_CHARGER, 1000));
  assert_int_equal(battery.calls, 1 + TEST_READS);
  assert_int_equal(battery.chargers, 0);

  battery.calls = 0;
  assert_true(run_board(&battery, TEST_CUTOFF, PARTS_MISSING_CHARGER,
                        1000));
  assert_int_equal(battery.calls, 1 + TEST_READS);
  assert_int_equal(battery.chargers, 0);
}

/* A battery part without its detail, before the battery: the battery's
 * voltage and gauge all the same
 */

static void test_power_board_spare(void **state)
{
  static const int mv[] =
  {
    3700, 3300, 3290, 3280
  };

  struct battery_s battery =
  {
    mv, NULL, 4, 0, 0
  };

  assert_true(run_board(&battery, TEST_CUTOFF, PARTS_SPARE_BATTERY, 1000));
  assert_int_equal(battery.calls, 1 + TEST_READS);
  assert_int_equal(battery.chargers, battery.calls);
}

/* A battery without its device file, or with one that is not there: no
 * readings, never off
 */

static void test_power_board_no_gauge(void **state)
{
  static const int mv[] =
  {
    3000
  };

  struct battery_s battery =
  {
    mv, NULL, 1, 0, 0
  };

  assert_false(run_board(&battery, TEST_CUTOFF, PARTS_NO_GAUGE, 50));
  assert_false(run_board(&battery, TEST_CUTOFF, PARTS_MISSING_GAUGE, 50));
  assert_int_equal(battery.calls, 0);
}

/* A description without the battery's power-off voltage, without a
 * battery, or none at all: no readings, never off
 */

static void test_power_board_none(void **state)
{
  static const int mv[] =
  {
    3000
  };

  struct battery_s battery =
  {
    mv, NULL, 1, 0, 0
  };

  assert_false(run_board(&battery, 0, PARTS_BOTH, 50));
  assert_false(run_board(&battery, -1, PARTS_BOTH, 50));
  assert_false(run_board(&battery, -2, PARTS_BOTH, 50));
  assert_int_equal(battery.calls, 0);
}

/* The kernel's side of the sleep clock's rate */

struct rate_s
{
  int32_t ppb;
  int calls;
  int error;                      /* What the kernel answers */
};

static int set_rate(FAR void *arg, int32_t ppb)
{
  FAR struct rate_s *rate = arg;

  rate->ppb = ppb;
  rate->calls++;
  return rate->error;
}

/* The sleep clock's rate Time announces goes to the kernel, also when it
 * was announced before Power started; one the kernel refuses changes
 * nothing else
 */

static void rate_run(FAR struct rate_s *rate, int32_t ppb)
{
  FAR struct power_s *power = calloc(1, sizeof(*power));
  struct pnut_loop_config_s lconfig;
  struct power_config_s config;
  FAR struct pnut_publisher_s *pub;
  FAR struct pnut_loop_s *loop;
  pnut_sleep_rate_t msg;
  char dir[48];
  char path[96];

  assert_non_null(power);
  snprintf(dir, sizeof(dir), "/tmp/pnut-power-%d", (int)getpid());
  assert_int_equal(mkdir(dir, 0700), 0);

  memset(&config, 0, sizeof(config));
  config.sleep_rate = set_rate;
  config.arg        = rate;
  power_init(power, &config);

  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);
  assert_int_equal(pnut_sleep_rate_advertise(loop, &pub), 0);
  memset(&msg, 0, sizeof(msg));
  msg.ppb = ppb;
  assert_int_equal(pnut_sleep_rate_publish(pub, &msg), 0);

  assert_int_equal(pnut_module_add(loop, &power->module), 0);
  assert_int_equal(pnut_timer_start(loop, 200, 0, stop_handler, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_false(power_wants_off(power));

  pnut_topic_unadvertise(pub);
  pnut_loop_destroy(loop);
  free(power);

  snprintf(path, sizeof(path), "%s/topic.sleep_rate/last", dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.sleep_rate", dir);
  rmdir(path);
  assert_int_equal(rmdir(dir), 0);
}

static void test_power_sleep_rate(void **state)
{
  struct rate_s rate =
  {
    0, 0, 0
  };

  rate_run(&rate, 8500000);
  assert_int_equal(rate.calls, 1);
  assert_int_equal(rate.ppb, 8500000);

  rate.error = -EINVAL;
  rate_run(&rate, 200000000);
  assert_int_equal(rate.calls, 2);
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
    cmocka_unit_test(test_power_board),
    cmocka_unit_test(test_power_board_no_charger),
    cmocka_unit_test(test_power_board_no_gauge),
    cmocka_unit_test(test_power_board_spare),
    cmocka_unit_test(test_power_board_none),
    cmocka_unit_test(test_power_sleep_rate),
  };

  return cmocka_run_group_tests_name("power", tests, NULL, NULL);
}
