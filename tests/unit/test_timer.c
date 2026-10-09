/****************************************************************************
 * pnut-os/tests/unit/test_timer.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/pool.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct record_s
{
  FAR struct pnut_timer_s *other;
  int order[8];
  int count;
  int fired;
};

struct order_arg_s
{
  FAR struct record_s *rec;
  int id;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct pnut_loop_s *loop_new(void)
{
  FAR struct pnut_loop_s *loop;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  return loop;
}

static void stop_handler(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void once_handler(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct record_s *rec = arg;

  rec->fired++;
  pnut_loop_stop(loop, 0);
}

static void test_timer_once(void **state)
{
  FAR struct pnut_loop_s *loop = loop_new();
  struct record_s rec;
  uint64_t start;

  memset(&rec, 0, sizeof(rec));
  start = pnut_now();
  assert_int_equal(pnut_timer_start(loop, 30, 0, once_handler, &rec, NULL),
                   0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(rec.fired, 1);
  assert_true(pnut_now() - start >= 30);
  assert_int_equal(loop->timers.avail, loop->config.timers);
  pnut_loop_destroy(loop);
}

static void order_handler(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct order_arg_s *order = arg;
  FAR struct record_s *rec = order->rec;

  rec->order[rec->count++] = order->id;
  if (rec->count == 3)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void test_timer_order(void **state)
{
  FAR struct pnut_loop_s *loop = loop_new();
  struct record_s rec;
  struct order_arg_s args[3];
  int delays[3] =
  {
    30, 10, 20
  };

  int i;

  memset(&rec, 0, sizeof(rec));
  for (i = 0; i < 3; i++)
    {
      args[i].rec = &rec;
      args[i].id  = delays[i];
    }

  for (i = 0; i < 3; i++)
    {
      assert_int_equal(pnut_timer_start(loop, delays[i], 0, order_handler,
                                        &args[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(rec.order[0], 10);
  assert_int_equal(rec.order[1], 20);
  assert_int_equal(rec.order[2], 30);
  pnut_loop_destroy(loop);
}

static void periodic_handler(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct record_s *rec = arg;

  if (++rec->count == 3)
    {
      pnut_timer_cancel(loop, timer);
      pnut_timer_start(loop, 30, 0, stop_handler, NULL, NULL);
    }
}

static void test_timer_periodic_cancels_itself(void **state)
{
  FAR struct pnut_loop_s *loop = loop_new();
  struct record_s rec;

  memset(&rec, 0, sizeof(rec));
  assert_int_equal(pnut_timer_start(loop, 5, 5, periodic_handler, &rec,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(rec.count, 3);
  assert_int_equal(loop->timers.avail, loop->config.timers);
  pnut_loop_destroy(loop);
}

static void cancel_other_handler(FAR struct pnut_loop_s *loop,
                                 FAR struct pnut_timer_s *timer,
                                 FAR void *arg)
{
  FAR struct record_s *rec = arg;

  pnut_timer_cancel(loop, rec->other);
}

static void never_handler(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct record_s *rec = arg;

  rec->fired++;
}

static void test_timer_cancel(void **state)
{
  FAR struct pnut_loop_s *loop = loop_new();
  struct record_s rec;

  memset(&rec, 0, sizeof(rec));
  assert_int_equal(pnut_timer_start(loop, 5, 0, cancel_other_handler, &rec,
                                    NULL), 0);
  assert_int_equal(pnut_timer_start(loop, 200, 0, never_handler, &rec,
                                    &rec.other), 0);
  assert_int_equal(pnut_timer_start(loop, 250, 0, stop_handler, NULL, NULL),
                   0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(rec.fired, 0);
  assert_int_equal(loop->timers.avail, loop->config.timers);
  pnut_loop_destroy(loop);
}

static void test_timer_pool_full(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;

  pnut_loop_defaults(&config);
  config.timers = 2;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);
  assert_int_equal(pnut_timer_start(loop, 100, 0, stop_handler, NULL, NULL),
                   0);
  assert_int_equal(pnut_timer_start(loop, 100, 0, stop_handler, NULL, NULL),
                   0);
  assert_int_equal(pnut_timer_start(loop, 100, 0, stop_handler, NULL, NULL),
                   -EBUSY);
  pnut_loop_destroy(loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_timer_once),
    cmocka_unit_test(test_timer_order),
    cmocka_unit_test(test_timer_periodic_cancels_itself),
    cmocka_unit_test(test_timer_cancel),
    cmocka_unit_test(test_timer_pool_full),
  };

  return cmocka_run_group_tests_name("timer", tests, NULL, NULL);
}
