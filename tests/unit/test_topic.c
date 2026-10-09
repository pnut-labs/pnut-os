/****************************************************************************
 * pnut-os/tests/unit/test_topic.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* On NuttX a topic outlives a test, and keeps its latest message for the
 * next: the tests look at what they publish themselves, at the end of
 * what a reader gets.
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
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

/* The test topic's code, generated from tests/proto/pnut/test */

#include "pnut/test/tick.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define READERS   2
#define GOT_MAX   32

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fixture_s;

struct reader_s
{
  struct pnut_test_tick_reader_s tick;
  FAR struct fixture_s *f;
  uint32_t got[GOT_MAX];
  int ngot;
  int stop_after;                 /* Unsubscribe after so many; 0: never */
};

struct fixture_s
{
  FAR struct pnut_loop_s *loop;
  FAR struct pnut_publisher_s *pub;
  struct reader_s readers[READERS];
  char rundir[64];
  uint32_t next;                  /* The next count to publish */
  int left;                       /* Counts left to publish */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void stop_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void on_tick(FAR struct pnut_test_tick_reader_s *tick,
                    FAR const pnut_test_tick_t *msg, FAR void *arg)
{
  FAR struct reader_s *r = arg;

  if (r->ngot < GOT_MAX)
    {
      r->got[r->ngot++] = msg->count;
    }

  if (r->stop_after != 0 && r->ngot == r->stop_after)
    {
      pnut_test_tick_unsubscribe(&r->tick);
    }
}

/* Publish a count every few milliseconds, then stop a while later */

static void publish_timer(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_tick_t msg;

  if (f->left == 0)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.count = f->next++;
  assert_int_equal(pnut_test_tick_publish(f->pub, &msg), 0);

  if (--f->left == 0)
    {
      pnut_timer_cancel(loop, timer);
      pnut_timer_start(loop, 100, 0, stop_timer, NULL, NULL);
    }
}

static void publish(FAR struct fixture_s *f, uint32_t first, int count)
{
  f->next = first;
  f->left = count;
  assert_int_equal(pnut_timer_start(f->loop, 10, 10, publish_timer, f,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
}

/* Whether a reader's last counts are first .. first + count - 1 */

static void ends_with(FAR struct reader_s *r, uint32_t first, int count)
{
  int i;

  assert_true(r->ngot >= count);
  for (i = 0; i < count; i++)
    {
      assert_int_equal(r->got[r->ngot - count + i], first + i);
    }
}

static int setup(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct fixture_s *f = calloc(1, sizeof(*f));
  int i;

  snprintf(f->rundir, sizeof(f->rundir), "/tmp/pnut-topic-%d", getpid());
  mkdir(f->rundir, 0700);

  pnut_loop_defaults(&config);
  config.rundir = f->rundir;
  assert_int_equal(pnut_loop_create(&config, &f->loop), 0);
  assert_int_equal(pnut_test_tick_advertise(f->loop, &f->pub), 0);

  for (i = 0; i < READERS; i++)
    {
      f->readers[i].f = f;
    }

  *state = f;
  return 0;
}

static int teardown(void **state)
{
  FAR struct fixture_s *f = *state;
  char path[128];
  int i;

  for (i = 0; i < READERS; i++)
    {
      pnut_test_tick_unsubscribe(&f->readers[i].tick);
    }

  pnut_topic_unadvertise(f->pub);
  pnut_loop_destroy(f->loop);

  snprintf(path, sizeof(path), "%s/topic.test_tick/last", f->rundir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.test_tick", f->rundir);
  rmdir(path);
  rmdir(f->rundir);
  free(f);
  return 0;
}

/* Two readers get every count, in order */

static void test_topic_order(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  for (i = 0; i < READERS; i++)
    {
      assert_int_equal(pnut_test_tick_subscribe(f->loop, on_tick,
                                                &f->readers[i],
                                                &f->readers[i].tick), 0);
    }

  publish(f, 100, 3);

  for (i = 0; i < READERS; i++)
    {
      ends_with(&f->readers[i], 100, 3);
    }
}

/* A reader who subscribes later gets the latest count first */

static void test_topic_latest(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR struct reader_s *r = &f->readers[0];

  publish(f, 200, 3);

  assert_int_equal(pnut_test_tick_subscribe(f->loop, on_tick, r, &r->tick),
                   0);
  publish(f, 300, 2);

  assert_int_equal(r->ngot, 3);
  assert_int_equal(r->got[0], 202);
  ends_with(r, 300, 2);
}

/* A reader may unsubscribe from its own handler, and gets no more */

static void test_topic_unsubscribe(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR struct reader_s *r = &f->readers[0];

  publish(f, 400, 1);

  r->stop_after = 2;
  assert_int_equal(pnut_test_tick_subscribe(f->loop, on_tick, r, &r->tick),
                   0);
  publish(f, 500, 3);

  /* The latest, 400, then 500, and no more */

  assert_int_equal(r->ngot, 2);
  assert_int_equal(r->got[1], 500);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_topic_order, setup, teardown),
    cmocka_unit_test_setup_teardown(test_topic_latest, setup, teardown),
    cmocka_unit_test_setup_teardown(test_topic_unsubscribe, setup,
                                    teardown),
  };

  return cmocka_run_group_tests_name("topic", tests, NULL, NULL);
}
