/****************************************************************************
 * pnut-os/tests/unit/test_ready.c
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CHECK_MS   10                   /* How often the test looks */
#define GIVEUP_MS  3000                 /* When it stops waiting */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A stand-in for NxInit's control socket, on the program's own loop: it
 * turns the first `busy` connections away, then answers "ready" with
 * `answer`
 */

struct fixture_s
{
  char rundir[64];
  char path[96];
  FAR struct pnut_loop_s *loop;
  int listener;
  int busy;
  FAR const char *answer;
  int accepted;
  int readies;                          /* "ready" lines received */
  int client;                           /* The connection, or -1 */
  char in[16];
  size_t len;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void fake_read(FAR struct pnut_loop_s *loop, int fd,
                      uint32_t events, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  char line[32];
  ssize_t n;

  n = read(fd, f->in + f->len, sizeof(f->in) - f->len);
  if (n <= 0)
    {
      pnut_loop_unwatch(loop, fd);
      close(fd);
      f->client = -1;
      return;
    }

  f->len += n;
  if (f->len == 6 && memcmp(f->in, "ready\n", 6) == 0)
    {
      f->readies++;
      snprintf(line, sizeof(line), "%s\n", f->answer);
      assert_int_equal(write(fd, line, strlen(line)), strlen(line));
      f->len = 0;
    }
}

static void fake_accept(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  static const char version[] = "nxinit 1\n";
  static const char refused[] = "error 16 too many clients\n";
  int conn;

  conn = accept(fd, NULL, NULL);
  if (conn < 0)
    {
      return;
    }

  f->accepted++;

  if (f->busy > 0)
    {
      f->busy--;
      assert_int_equal(write(conn, refused, sizeof(refused) - 1),
                       sizeof(refused) - 1);
      close(conn);
      return;
    }

  assert_int_equal(write(conn, version, sizeof(version) - 1),
                   sizeof(version) - 1);
  f->len    = 0;
  f->client = conn;
  assert_int_equal(pnut_loop_watch(loop, conn, EPOLLIN, fake_read, f), 0);
}

/* Stop once the library has stopped telling, or after GIVEUP_MS */

static void check(FAR struct pnut_loop_s *loop,
                  FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR uint32_t *elapsed = arg;

  *elapsed += CHECK_MS;
  if (!loop->nxinit.telling || *elapsed >= GIVEUP_MS)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void fixture_run(FAR struct fixture_s *f)
{
  uint32_t elapsed = 0;

  assert_int_equal(pnut_timer_start(f->loop, CHECK_MS, CHECK_MS, check,
                                    &elapsed, NULL), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
}

/* A loop told NxInit's socket is at `path`, with the stand-in listening
 * there when `listen` is set
 */

static void fixture_setup(FAR struct fixture_s *f, bool listening,
                          int busy, FAR const char *answer)
{
  struct pnut_loop_config_s config;
  struct sockaddr_un addr;

  memset(f, 0, sizeof(*f));
  f->listener = -1;
  f->client   = -1;
  f->busy     = busy;
  f->answer   = answer;

  /* On NuttX the directory is only a prefix of the socket's name */

  snprintf(f->rundir, sizeof(f->rundir), "/tmp/pnut-ready-%d", getpid());
  mkdir(f->rundir, 0700);
  snprintf(f->path, sizeof(f->path), "%s/nxinit", f->rundir);
  unlink(f->path);

  pnut_loop_defaults(&config);
  config.rundir  = f->rundir;
  config.initctl = f->path;
  assert_int_equal(pnut_loop_create(&config, &f->loop), 0);

  if (listening)
    {
      f->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
      assert_true(f->listener >= 0);

      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      strlcpy(addr.sun_path, f->path, sizeof(addr.sun_path));
      assert_int_equal(bind(f->listener, (FAR struct sockaddr *)&addr,
                            sizeof(addr)), 0);
      assert_int_equal(listen(f->listener, 4), 0);
      assert_int_equal(pnut_loop_watch(f->loop, f->listener, EPOLLIN,
                                       fake_accept, f), 0);
    }
}

static void fixture_teardown(FAR struct fixture_s *f)
{
  if (f->client >= 0)
    {
      close(f->client);
    }

  if (f->listener >= 0)
    {
      close(f->listener);
    }

  pnut_loop_destroy(f->loop);
  unlink(f->path);
  rmdir(f->rundir);
}

/* Handlers that must not run */

static void never_fd(FAR struct pnut_loop_s *loop, int fd, uint32_t events,
                     FAR void *arg)
{
  fail();
}

static void never_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  fail();
}

/****************************************************************************
 * Name: test_ready_told
 *
 * Description:
 *   A loop whose modules are all ready sends "ready" once, and stops
 *   telling at NxInit's "ok".
 ****************************************************************************/

static void test_ready_told(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, true, 0, "ok");
  fixture_run(&f);

  assert_int_equal(f.accepted, 1);
  assert_int_equal(f.readies, 1);
  assert_false(f.loop->nxinit.telling);
  assert_int_equal(f.loop->nxinit.fd, -1);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_ready_turned_away
 *
 * Description:
 *   A connection NxInit turns away is tried again, after a delay.
 ****************************************************************************/

static void test_ready_turned_away(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, true, 2, "ok");
  fixture_run(&f);

  assert_int_equal(f.accepted, 3);
  assert_int_equal(f.readies, 1);
  assert_false(f.loop->nxinit.telling);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_ready_refused
 *
 * Description:
 *   An error answer is final: a program NxInit did not start, or one
 *   whose service lacks "notify", is not asked again.
 ****************************************************************************/

static void test_ready_refused(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, true, 0, "error 22 not a notify service");
  fixture_run(&f);

  assert_int_equal(f.accepted, 1);
  assert_int_equal(f.readies, 1);
  assert_false(f.loop->nxinit.telling);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_ready_no_nxinit
 *
 * Description:
 *   Without the socket, the loop goes on trying, with nothing open in
 *   between; stopping the loop stops it.
 ****************************************************************************/

static void test_ready_no_nxinit(void **state)
{
  struct fixture_s f;
  uint32_t elapsed = GIVEUP_MS - 300;

  fixture_setup(&f, false, 0, "ok");

  assert_int_equal(pnut_timer_start(f.loop, CHECK_MS, CHECK_MS, check,
                                    &elapsed, NULL), 0);
  assert_int_equal(pnut_loop_run(f.loop), 0);

  assert_true(f.loop->nxinit.warned);
  assert_true(f.loop->nxinit.backoff > CONFIG_PNUT_LIB_RECONNECT_MIN);
  assert_false(f.loop->nxinit.telling);
  assert_int_equal(f.loop->nxinit.fd, -1);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_ready_slots
 *
 * Description:
 *   Telling NxInit takes a descriptor and a timer of its own, not the
 *   program's.
 ****************************************************************************/

static void test_ready_slots(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  int fds[2][2];
  int i;

  pnut_loop_defaults(&config);
  config.fds     = 2;
  config.timers  = 1;
  config.initctl = "/tmp/pnut-ready-none";
  assert_int_equal(pnut_loop_create(&config, &loop), 0);

  for (i = 0; i < 2; i++)
    {
      assert_int_equal(pipe(fds[i]), 0);
      assert_int_equal(pnut_loop_watch(loop, fds[i][0], EPOLLIN, never_fd,
                                       NULL), 0);
    }

  assert_int_equal(pnut_loop_watch(loop, fds[0][1], EPOLLOUT, never_fd,
                                   NULL), -EBUSY);
  assert_int_equal(pnut_loop_watch_ready(loop, fds[0][1], EPOLLOUT,
                                         never_fd, NULL), 0);

  assert_int_equal(pnut_timer_start(loop, 100, 0, never_timer, NULL, NULL),
                   0);
  assert_int_equal(pnut_timer_start(loop, 100, 0, never_timer, NULL, NULL),
                   -EBUSY);

  pnut_loop_destroy(loop);

  for (i = 0; i < 2; i++)
    {
      close(fds[i][0]);
      close(fds[i][1]);
    }

  /* A path longer than a socket's is refused when the loop is made */

  config.initctl = "/tmp/a-path-much-too-long-for-a-socket-name-on-any-"
                   "system-so-the-loop-refuses-it-at-once-0123456789-"
                   "0123456789-0123456789";
  assert_int_equal(pnut_loop_create(&config, &loop), -ENAMETOOLONG);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_ready_told),
    cmocka_unit_test(test_ready_turned_away),
    cmocka_unit_test(test_ready_refused),
    cmocka_unit_test(test_ready_no_nxinit),
    cmocka_unit_test(test_ready_slots),
  };

  return cmocka_run_group_tests_name("ready", tests, NULL, NULL);
}
