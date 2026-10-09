/****************************************************************************
 * pnut-os/tests/unit/test_loop.c
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

struct pipes_s
{
  int a[2];
  int b[2];
  int calls;
};

struct trace_s
{
  char log[32];
  int len;
  FAR struct pnut_module_s *late;
  int fail;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void read_and_stop(FAR struct pnut_loop_s *loop, int fd,
                          uint32_t events, FAR void *arg)
{
  char c;

  assert_true(events & EPOLLIN);
  assert_int_equal(read(fd, &c, 1), 1);
  pnut_loop_stop(loop, c);
}

static void test_loop_watch(void **state)
{
  FAR struct pnut_loop_s *loop;
  int fds[2];

  assert_int_equal(pipe(fds), 0);
  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_loop_watch(loop, fds[0], EPOLLIN, read_and_stop,
                                   NULL), 0);
  assert_int_equal(pnut_loop_watch(loop, fds[0], EPOLLIN, read_and_stop,
                                   NULL), -EEXIST);
  assert_int_equal(write(fds[1], "\x07", 1), 1);
  assert_int_equal(pnut_loop_run(loop), 7);
  assert_int_equal(pnut_loop_unwatch(loop, fds[0]), 0);
  assert_int_equal(pnut_loop_unwatch(loop, fds[0]), -ENOENT);
  pnut_loop_destroy(loop);
  close(fds[0]);
  close(fds[1]);
}

static void test_loop_own_fds(void **state)
{
  FAR struct pnut_loop_s *loop;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_loop_unwatch(loop, loop->timerfd), -EINVAL);
  assert_int_equal(pnut_loop_unwatch(loop, loop->eventfd), -EINVAL);
  assert_int_equal(pnut_loop_unwatch(loop, loop->sigfd), -EINVAL);
  assert_int_equal(pnut_loop_rewatch(loop, loop->timerfd, EPOLLIN),
                   -EINVAL);
  pnut_loop_destroy(loop);
}

static void test_loop_limits_and_signals(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  sigset_t mask;

  pnut_loop_defaults(&config);
  config.fds = UINT16_MAX;
  assert_int_equal(pnut_loop_create(&config, &loop), -EINVAL);
  assert_null(loop);

  /* A loop blocks SIGTERM while it lives, and unblocks it when it goes */

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  pthread_sigmask(SIG_BLOCK, NULL, &mask);
  assert_true(sigismember(&mask, SIGTERM));
  pnut_loop_destroy(loop);
  pthread_sigmask(SIG_BLOCK, NULL, &mask);
  assert_false(sigismember(&mask, SIGTERM));

  /* A signal blocked before the loop stays blocked after it */

  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);
  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  pnut_loop_destroy(loop);
  pthread_sigmask(SIG_BLOCK, NULL, &mask);
  assert_true(sigismember(&mask, SIGINT));
  assert_false(sigismember(&mask, SIGTERM));

  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
}

static void test_loop_watch_full(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  int a[2];
  int b[2];

  assert_int_equal(pipe(a), 0);
  assert_int_equal(pipe(b), 0);
  pnut_loop_defaults(&config);
  config.fds = 1;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);
  assert_int_equal(pnut_loop_watch(loop, a[0], EPOLLIN, read_and_stop,
                                   NULL), 0);
  assert_int_equal(pnut_loop_watch(loop, b[0], EPOLLIN, read_and_stop,
                                   NULL), -EBUSY);
  pnut_loop_destroy(loop);
  close(a[0]);
  close(a[1]);
  close(b[0]);
  close(b[1]);
}

/* Both pipes are ready in one batch; whichever handler runs first unwatches
 * the other, which must then not run.
 */

static void unwatch_other(FAR struct pnut_loop_s *loop, int fd,
                          uint32_t events, FAR void *arg)
{
  FAR struct pipes_s *p = arg;
  char c;

  p->calls++;
  assert_int_equal(read(fd, &c, 1), 1);
  assert_int_equal(pnut_loop_unwatch(loop, fd == p->a[0] ? p->b[0] :
                                                          p->a[0]), 0);
}

static void stop_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void test_loop_unwatch_in_batch(void **state)
{
  FAR struct pnut_loop_s *loop;
  struct pipes_s p;

  memset(&p, 0, sizeof(p));
  assert_int_equal(pipe(p.a), 0);
  assert_int_equal(pipe(p.b), 0);
  assert_int_equal(write(p.a[1], "a", 1), 1);
  assert_int_equal(write(p.b[1], "b", 1), 1);

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_loop_watch(loop, p.a[0], EPOLLIN, unwatch_other,
                                   &p), 0);
  assert_int_equal(pnut_loop_watch(loop, p.b[0], EPOLLIN, unwatch_other,
                                   &p), 0);
  assert_int_equal(pnut_timer_start(loop, 50, 0, stop_timer, NULL, NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(p.calls, 1);

  pnut_loop_destroy(loop);
  close(p.a[0]);
  close(p.a[1]);
  close(p.b[0]);
  close(p.b[1]);
}

static void sigterm_timer(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  kill(getpid(), SIGTERM);
}

static void trace(FAR struct trace_s *t, char c)
{
  t->log[t->len++] = c;
}

static FAR struct trace_s *g_trace;

static int start_a(FAR struct pnut_module_s *m, FAR struct pnut_loop_s *l)
{
  trace(g_trace, 'A');
  pnut_module_ready(m);
  return 0;
}

static int start_b(FAR struct pnut_module_s *m, FAR struct pnut_loop_s *l)
{
  trace(g_trace, 'B');
  g_trace->late = m;
  return g_trace->fail;
}

static void stop_a(FAR struct pnut_module_s *m, FAR struct pnut_loop_s *l)
{
  trace(g_trace, 'a');
}

static void stop_b(FAR struct pnut_module_s *m, FAR struct pnut_loop_s *l)
{
  trace(g_trace, 'b');
}

static void late_ready(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  assert_false(pnut_loop_ready(loop));
  pnut_module_ready(g_trace->late);
  assert_true(pnut_loop_ready(loop));
  pnut_timer_start(loop, 5, 0, sigterm_timer, NULL, NULL);
}

static void test_loop_modules_and_sigterm(void **state)
{
  FAR struct pnut_loop_s *loop;
  struct pnut_module_s a;
  struct pnut_module_s b;
  struct trace_s t;

  memset(&t, 0, sizeof(t));
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  g_trace = &t;

  a.name = "a";
  a.start = start_a;
  a.stop = stop_a;
  b.name = "b";
  b.start = start_b;
  b.stop = stop_b;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &a), 0);
  assert_int_equal(pnut_module_add(loop, &b), 0);
  assert_int_equal(pnut_timer_start(loop, 10, 0, late_ready, NULL, NULL),
                   0);

  /* SIGTERM stops the loop with status 0; modules stop in reverse */

  assert_int_equal(pnut_loop_run(loop), 0);
  assert_string_equal(t.log, "ABba");
  assert_false(pnut_loop_ready(loop));
  pnut_loop_destroy(loop);
}

static void test_loop_module_fails(void **state)
{
  FAR struct pnut_loop_s *loop;
  struct pnut_module_s a;
  struct pnut_module_s b;
  struct trace_s t;

  memset(&t, 0, sizeof(t));
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  t.fail = -EIO;
  g_trace = &t;

  a.name = "a";
  a.start = start_a;
  a.stop = stop_a;
  b.name = "b";
  b.start = start_b;
  b.stop = stop_b;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &a), 0);
  assert_int_equal(pnut_module_add(loop, &b), 0);

  /* b fails: a, which started, is stopped; b is not */

  assert_int_equal(pnut_loop_run(loop), -EIO);
  assert_string_equal(t.log, "ABa");
  pnut_loop_destroy(loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_loop_watch),
    cmocka_unit_test(test_loop_watch_full),
    cmocka_unit_test(test_loop_own_fds),
    cmocka_unit_test(test_loop_limits_and_signals),
    cmocka_unit_test(test_loop_unwatch_in_batch),
    cmocka_unit_test(test_loop_modules_and_sigterm),
    cmocka_unit_test(test_loop_module_fails),
  };

  return cmocka_run_group_tests_name("loop", tests, NULL, NULL);
}
