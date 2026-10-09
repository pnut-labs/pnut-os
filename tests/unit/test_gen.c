/****************************************************************************
 * pnut-os/tests/unit/test_gen.c
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
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/client.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/service.h>
#include <pnut/timer.h>

/* The test interface's code, generated from tests/proto/pnut/test */

#include "pnut/test/echo.pnut.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fixture_s
{
  FAR struct pnut_loop_s *loop;
  struct pnut_test_echo_server_s server;
  struct pnut_test_echo_client_s client;
  char rundir[64];
  uint32_t said;                  /* The server's count */
  struct pnut_request_s kept;     /* A request answered later */
  char text[64];
  bool later;                     /* Answer later */
  bool silent;                    /* Never answer */
  int expect;                     /* Answers to wait for */
  int answers;
  int status[PNUT_CLIENT_INFLIGHT + 2];
  pnut_test_say_reply_t out[PNUT_CLIENT_INFLIGHT + 2];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void stop_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

/* An answer came: the test is over once all it waits for have */

static void answered(FAR struct fixture_s *f, int status)
{
  f->status[f->answers++] = status;
  if (f->answers == f->expect)
    {
      pnut_loop_stop(f->loop, 0);
    }
}

static void later_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_say_reply_t out = PNUT_TEST_SAY_REPLY_INIT_ZERO;

  strcpy(out.text, f->text);
  out.count = ++f->said;
  assert_int_equal(pnut_test_echo_say_reply(&f->server, &f->kept,
                                            PNUT_STATUS_OK, &out), 0);
}

/* The server's Say: answers at once, or later from a copy */

static void on_say(FAR struct pnut_test_echo_server_s *server,
                   FAR const struct pnut_request_s *req,
                   FAR const pnut_test_say_request_t *in, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_say_reply_t out = PNUT_TEST_SAY_REPLY_INIT_ZERO;

  if (f->silent)
    {
      return;
    }

  if (f->later)
    {
      f->kept = *req;
      strcpy(f->text, in->text);
      pnut_timer_start(f->loop, 20, 0, later_timer, f, NULL);
      return;
    }

  strcpy(out.text, in->text);
  out.count = ++f->said;
  assert_int_equal(pnut_test_echo_say_reply(server, req, PNUT_STATUS_OK,
                                            &out), 0);
}

static void on_count(FAR struct pnut_test_echo_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_test_count_request_t *in, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_count_reply_t out = PNUT_TEST_COUNT_REPLY_INIT_ZERO;

  out.count = f->said;
  assert_int_equal(pnut_test_echo_count_reply(server, req, PNUT_STATUS_OK,
                                              &out), 0);
}

/* Count is left unserved, except by g_all */

static const struct pnut_test_echo_handlers_s g_handlers =
{
  .say = on_say,
};

static const struct pnut_test_echo_handlers_s g_all =
{
  .say   = on_say,
  .count = on_count,
};

static void on_said(FAR struct pnut_test_echo_client_s *client,
                    int status, FAR const pnut_test_say_reply_t *out,
                    FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (out != NULL)
    {
      f->out[f->answers] = *out;
    }
  else
    {
      assert_int_not_equal(status, PNUT_STATUS_OK);
    }

  answered(f, status);
}

static void on_counted(FAR struct pnut_test_echo_client_s *client,
                       int status, FAR const pnut_test_count_reply_t *out,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (status == PNUT_STATUS_OK)
    {
      assert_non_null(out);
      assert_int_equal(out->count, f->said);
    }
  else
    {
      assert_null(out);
    }

  answered(f, status);
}

static void on_raw(FAR struct pnut_client_s *client, int status,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  answered(f, status);
}

static int setup(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct fixture_s *f = calloc(1, sizeof(*f));

  snprintf(f->rundir, sizeof(f->rundir), "/tmp/pnut-gen-%d", getpid());
  mkdir(f->rundir, 0700);

  pnut_loop_defaults(&config);
  config.rundir = f->rundir;
  assert_int_equal(pnut_loop_create(&config, &f->loop), 0);
  assert_int_equal(pnut_test_echo_serve(f->loop, "echo", 0, &g_handlers, f,
                                        &f->server), 0);
  *state = f;
  return 0;
}

static int teardown(void **state)
{
  FAR struct fixture_s *f = *state;

  pnut_test_echo_disconnect(&f->client);
  pnut_test_echo_close(&f->server);
  pnut_loop_destroy(f->loop);
  rmdir(f->rundir);
  free(f);
  return 0;
}

/* Connect, and run until the answers expected have come; a test that
 * expects none, or never gets them, stops on its own after a while
 */

static void run(FAR struct fixture_s *f, pnut_test_echo_state_t state,
                int expect)
{
  f->expect = expect;
  assert_int_equal(pnut_test_echo_connect(f->loop, "echo", state, f,
                                          &f->client), 0);
  pnut_timer_start(f->loop, expect > 0 ? 2000 : 100, 0, stop_timer, NULL,
                   NULL);
  assert_int_equal(pnut_loop_run(f->loop), 0);
}

/* Typed calls, typed answers */

static void say_state(FAR struct pnut_test_echo_client_s *client, bool up,
                      FAR void *arg)
{
  pnut_test_say_request_t in = PNUT_TEST_SAY_REQUEST_INIT_ZERO;

  if (up)
    {
      strcpy(in.text, "hi");
      assert_int_equal(pnut_test_echo_say(client, &in, 0, on_said, arg), 0);
      strcpy(in.text, "ho");
      assert_int_equal(pnut_test_echo_say(client, &in, 0, on_said, arg), 0);
    }
}

static void test_gen_say(void **state)
{
  FAR struct fixture_s *f = *state;

  run(f, say_state, 2);
  assert_int_equal(f->answers, 2);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_string_equal(f->out[0].text, "hi");
  assert_int_equal(f->out[0].count, 1);
  assert_int_equal(f->status[1], PNUT_STATUS_OK);
  assert_string_equal(f->out[1].text, "ho");
  assert_int_equal(f->out[1].count, 2);
}

/* A method its server leaves unserved is not found */

static void count_state(FAR struct pnut_test_echo_client_s *client, bool up,
                        FAR void *arg)
{
  pnut_test_count_request_t in = PNUT_TEST_COUNT_REQUEST_INIT_ZERO;

  if (up)
    {
      assert_int_equal(pnut_test_echo_count(client, &in, 0, on_counted,
                                            arg), 0);
    }
}

static void test_gen_unserved(void **state)
{
  FAR struct fixture_s *f = *state;

  run(f, count_state, 1);
  assert_int_equal(f->answers, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_NOTFOUND);
}

/* A served method whose request is empty, end to end */

static void test_gen_count(void **state)
{
  FAR struct fixture_s *f = *state;

  pnut_test_echo_close(&f->server);
  assert_int_equal(pnut_test_echo_serve(f->loop, "echo", 0, &g_all, f,
                                        &f->server), 0);
  f->said = 3;
  run(f, count_state, 1);
  assert_int_equal(f->answers, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
}

/* Calls never answered time out, with no body, and give their places
 * back: one more than fit is refused at once, and after the timeouts one
 * more goes, which the service answers busy, since the requests it never
 * answered still count
 */

static void silent_state(FAR struct pnut_test_echo_client_s *client,
                         bool up, FAR void *arg)
{
  pnut_test_say_request_t in = PNUT_TEST_SAY_REQUEST_INIT_ZERO;
  int i;

  if (up)
    {
      for (i = 0; i < PNUT_CLIENT_INFLIGHT; i++)
        {
          assert_int_equal(pnut_test_echo_say(client, &in, 50, on_said,
                                              arg), 0);
        }

      assert_int_equal(pnut_test_echo_say(client, &in, 50, on_said, arg),
                       -EBUSY);
    }
}

static void again_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_say_request_t in = PNUT_TEST_SAY_REQUEST_INIT_ZERO;

  assert_int_equal(pnut_test_echo_say(&f->client, &in, 0, on_said, f), 0);
}

static void test_gen_timeout(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  f->silent = true;
  pnut_timer_start(f->loop, 200, 0, again_timer, f, NULL);
  run(f, silent_state, PNUT_CLIENT_INFLIGHT + 1);
  assert_int_equal(f->answers, PNUT_CLIENT_INFLIGHT + 1);
  for (i = 0; i < PNUT_CLIENT_INFLIGHT; i++)
    {
      assert_int_equal(f->status[i], PNUT_STATUS_TIMEOUT);
    }

  assert_int_equal(f->status[PNUT_CLIENT_INFLIGHT], PNUT_STATUS_BUSY);
}

/* A body that does not decode, and another version of the method, are
 * invalid
 */

static void raw_state(FAR struct pnut_test_echo_client_s *client, bool up,
                      FAR void *arg)
{
  static const uint8_t junk[] =
  {
    0x0a, 0x7f, 'x'               /* Field 1, 127 bytes long: cut short */
  };

  if (up)
    {
      assert_int_equal(pnut_call(client->client, PNUT_TEST_ECHO_IFACE,
                                 PNUT_TEST_ECHO_SAY, 1, junk, sizeof(junk),
                                 0, on_raw, arg), 0);
      assert_int_equal(pnut_call(client->client, PNUT_TEST_ECHO_IFACE,
                                 PNUT_TEST_ECHO_SAY, 2, NULL, 0, 0, on_raw,
                                 arg), 0);
    }
}

static void test_gen_invalid(void **state)
{
  FAR struct fixture_s *f = *state;

  run(f, raw_state, 2);
  assert_int_equal(f->answers, 2);
  assert_int_equal(f->status[0], PNUT_STATUS_INVALID);
  assert_int_equal(f->status[1], PNUT_STATUS_INVALID);
}

/* An answer given later, from a copy of the request */

static void later_state(FAR struct pnut_test_echo_client_s *client,
                        bool up, FAR void *arg)
{
  pnut_test_say_request_t in = PNUT_TEST_SAY_REQUEST_INIT_ZERO;

  if (up)
    {
      strcpy(in.text, "later");
      assert_int_equal(pnut_test_echo_say(client, &in, 0, on_said, arg), 0);
    }
}

static void test_gen_later(void **state)
{
  FAR struct fixture_s *f = *state;

  f->later = true;
  run(f, later_state, 1);
  assert_int_equal(f->answers, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_string_equal(f->out[0].text, "later");
  assert_int_equal(f->out[0].count, 1);
}

/* A request that does not encode is refused, and nothing is sent */

static void unterminated_state(FAR struct pnut_test_echo_client_s *client,
                               bool up, FAR void *arg)
{
  pnut_test_say_request_t in;

  if (up)
    {
      memset(in.text, 'x', sizeof(in.text));
      assert_int_equal(pnut_test_echo_say(client, &in, 0, on_said, arg),
                       -EINVAL);
    }
}

static void test_gen_unencodable(void **state)
{
  FAR struct fixture_s *f = *state;

  run(f, unterminated_state, 0);
  assert_int_equal(f->answers, 0);
  assert_int_equal(f->said, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_gen_say, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_unserved, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_count, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_timeout, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_invalid, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_later, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_unencodable, setup, teardown),
  };

  return cmocka_run_group_tests_name("gen", tests, NULL, NULL);
}
