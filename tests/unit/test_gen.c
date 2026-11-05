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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

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
  int failwith;                   /* Answer with this error status */
  bool bare;                      /* The error without its detail */
  FAR const char *why;            /* The detail's text */
  int expect;                     /* Answers to wait for */
  int answers;
  int status[PNUT_CLIENT_INFLIGHT + 2];
  pnut_test_say_reply_t out[PNUT_CLIENT_INFLIGHT + 2];
  bool detail[PNUT_CLIENT_INFLIGHT + 2];
  pnut_error_t err[PNUT_CLIENT_INFLIGHT + 2];

  /* Ticks: the server's stream, and what the client saw of it */

  struct pnut_stream_s stream;
  uint32_t total;                 /* Ticks asked for */
  uint32_t sent;
  bool closed;                    /* The server was told it is over */
  int closed_status;
  uint16_t credit;                /* The client's first */
  uint16_t give;                  /* It gives one more after each tick */
  uint32_t cancel_after;          /* Ticks, then it cancels; 0 never */
  uint32_t drop_after;            /* Ticks, then it disconnects; 0 never */
  uint32_t end_after;             /* Ticks, then the server fails it */
  uint16_t sno;                   /* The stream's number */
  int opened;                     /* Its opening answer; -1 none yet */
  bool opened_detail;
  uint32_t got;
  uint32_t numbers[16];
  int ended;                      /* Its end's status; -1 none yet */
  bool cancel_gone;               /* It cancels after the service went */
  bool cancel_early;              /* It cancels before the opening answer,
                                   * then says something */
  int cancel_ret;
  int raw;                        /* A caller of the test's own */
  bool raw_request;               /* It asks, rather than gives credit */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void on_said(FAR struct pnut_test_echo_client_s *client,
                    int status, FAR const pnut_test_say_reply_t *out,
                    FAR const pnut_error_t *err, FAR void *arg);

static void h_junk(FAR struct pnut_service_s *service,
                   FAR const struct pnut_request_s *req,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Say, served by hand: answers with a detail that does not decode */

static const struct pnut_method_s g_junk[] =
{
  { PNUT_TEST_ECHO_IFACE, PNUT_TEST_ECHO_SAY, 1, h_junk, 16 },
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

static void answered(FAR struct fixture_s *f, int status,
                     FAR const pnut_error_t *err)
{
  if (err != NULL)
    {
      assert_int_not_equal(status, PNUT_STATUS_OK);
      f->detail[f->answers] = true;
      f->err[f->answers] = *err;
    }

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

  if (f->failwith != PNUT_STATUS_OK && f->bare)
    {
      assert_int_equal(pnut_test_echo_say_reply(server, req, f->failwith,
                                                NULL), 0);
      return;
    }

  if (f->failwith != PNUT_STATUS_OK)
    {
      assert_int_equal(pnut_test_echo_fail(server, req, PNUT_STATUS_OK, 1,
                                           "ok is no error"), -EINVAL);
      assert_int_equal(pnut_test_echo_fail(server, req, f->failwith, 7,
                                           f->why), 0);
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

  /* Its answer is a few bytes, and its room an error's detail */

  if (f->failwith != PNUT_STATUS_OK)
    {
      assert_int_equal(pnut_test_echo_fail(server, req, f->failwith, 9,
                                           f->why), 0);
      return;
    }

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

/* The server's Ticks: as many as asked for, as credit allows, then the
 * end
 */

static void ticks_push(FAR struct fixture_s *f)
{
  pnut_test_ticked_t out = PNUT_TEST_TICKED_INIT_ZERO;
  int ret;

  while (f->stream.open && f->sent < f->total)
    {
      if (f->end_after > 0 && f->sent == f->end_after)
        {
          pnut_stream_end(&f->stream, PNUT_STATUS_INTERNAL);
          return;
        }

      out.number = f->sent + 1;
      ret = pnut_test_echo_ticks_push(&f->server, &f->stream, &out);
      if (ret == -EAGAIN)
        {
          return;                 /* Until credit comes */
        }

      assert_int_equal(ret, 0);
      f->sent++;
    }

  if (f->stream.open)
    {
      pnut_stream_end(&f->stream, PNUT_STATUS_OK);
    }
}

static void ticks_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  ticks_push(arg);
}

static void ticks_closed(FAR struct pnut_stream_s *stream, int status,
                         FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  assert_false(stream->open);
  f->closed        = true;
  f->closed_status = status;
  pnut_loop_stop(f->loop, 0);
}

static void on_ticks(FAR struct pnut_test_echo_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_test_ticks_request_t *in, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (f->failwith != PNUT_STATUS_OK)
    {
      assert_int_equal(pnut_test_echo_fail(server, req, f->failwith, 4,
                                           "no ticks"), 0);
      return;
    }

  f->total = in->count;
  assert_int_equal(pnut_test_echo_ticks_accept(server, req, &f->stream,
                                               ticks_ready, ticks_closed, f),
                   0);
  ticks_push(f);
}

static const struct pnut_test_echo_handlers_s g_ticks =
{
  .say   = on_say,
  .ticks = on_ticks,
};

/* The client's side of Ticks */

static void ticks_opened(FAR struct pnut_test_echo_client_s *client,
                         int status, FAR const pnut_error_t *err,
                         FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->opened        = status;
  f->opened_detail = err != NULL;
  if (status != PNUT_STATUS_OK)
    {
      pnut_loop_stop(f->loop, 0);
    }
}

static void ticks_item(FAR struct pnut_test_echo_client_s *client,
                       int status, FAR const pnut_test_ticked_t *item,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (status != PNUT_STATUS_OK)
    {
      assert_null(item);
      f->ended = status;
      pnut_loop_stop(f->loop, 0);
      return;
    }

  assert_non_null(item);
  assert_int_equal(f->ended, -1);
  if (f->got < 16)
    {
      f->numbers[f->got] = item->number;
    }

  f->got++;
  if (f->cancel_gone)
    {
      pnut_test_echo_close(&f->server);
      f->cancel_ret = pnut_test_echo_ticks_cancel(client, f->sno);
      pnut_loop_stop(f->loop, 0);
      return;
    }

  if (f->cancel_after > 0 && f->got == f->cancel_after)
    {
      assert_int_equal(pnut_test_echo_ticks_cancel(client, f->sno), 0);
      return;
    }

  if (f->drop_after > 0 && f->got == f->drop_after)
    {
      pnut_test_echo_disconnect(client);
      return;
    }

  if (f->give > 0)
    {
      assert_int_equal(pnut_test_echo_ticks_credit(client, f->sno, f->give),
                       0);
    }
}

static void ticks_state(FAR struct pnut_test_echo_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_test_ticks_request_t in = PNUT_TEST_TICKS_REQUEST_INIT_ZERO;

  if (up && f->opened == -1)
    {
      in.count = f->total;
      assert_int_equal(pnut_test_echo_ticks(client, &in, f->credit, 0,
                                            ticks_opened, ticks_item, f,
                                            &f->sno), 0);
      if (f->cancel_early)
        {
          pnut_test_say_request_t say = PNUT_TEST_SAY_REQUEST_INIT_ZERO;

          /* Its call's place taken by another call at once: the late
           * opening answer must not reach it
           */

          assert_int_equal(pnut_test_echo_ticks_cancel(client, f->sno), 0);
          strcpy(say.text, "after");
          assert_int_equal(pnut_test_echo_say(client, &say, 0, on_said, f),
                           0);
        }
    }
}

/* Serve Ticks, and set the client's part */

static void ticks_setup(FAR struct fixture_s *f, uint32_t total,
                        uint16_t credit, uint16_t give)
{
  pnut_test_echo_close(&f->server);
  assert_int_equal(pnut_test_echo_serve(f->loop, "echo", 0, &g_ticks, f,
                                        &f->server), 0);
  f->total  = total;
  f->credit = credit;
  f->give   = give;
  f->opened = -1;
  f->ended  = -1;
}

static void on_said(FAR struct pnut_test_echo_client_s *client,
                    int status, FAR const pnut_test_say_reply_t *out,
                    FAR const pnut_error_t *err, FAR void *arg)
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

  answered(f, status, err);
}

static void on_counted(FAR struct pnut_test_echo_client_s *client,
                       int status, FAR const pnut_test_count_reply_t *out,
                       FAR const pnut_error_t *err, FAR void *arg)
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

  answered(f, status, err);
}

static void on_raw(FAR struct pnut_client_s *client, int status,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  answered(f, status, NULL);
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

/* An error answer with its detail: the interface's code and a text */

static void test_gen_fail(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  f->failwith = PNUT_STATUS_INVALID;
  f->why      = "read-only";
  run(f, say_state, 2);
  assert_int_equal(f->answers, 2);
  for (i = 0; i < 2; i++)
    {
      assert_int_equal(f->status[i], PNUT_STATUS_INVALID);
      assert_true(f->detail[i]);
      assert_int_equal(f->err[i].code, 7);
      assert_string_equal(f->err[i].text, "read-only");
    }
}

/* An error answer without one */

static void test_gen_fail_bare(void **state)
{
  FAR struct fixture_s *f = *state;

  f->failwith = PNUT_STATUS_UNAVAILABLE;
  f->bare     = true;
  run(f, say_state, 2);
  assert_int_equal(f->answers, 2);
  assert_int_equal(f->status[0], PNUT_STATUS_UNAVAILABLE);
  assert_false(f->detail[0]);
  assert_int_equal(f->status[1], PNUT_STATUS_UNAVAILABLE);
  assert_false(f->detail[1]);
}

/* A method whose answer is a few bytes has room for an error's whole
 * detail; a longer text is cut, and not inside a character
 */

static void test_gen_fail_long(void **state)
{
  FAR struct fixture_s *f = *state;
  char why[100];
  char cut[64];

  /* 62 letters, then a two-byte character whose first byte is the 63rd,
   * so that it straddles the cut
   */

  memset(why, 'a', sizeof(why) - 1);
  why[sizeof(why) - 1] = '\0';
  why[62] = (char)0xc5;
  why[63] = (char)0x82;
  memset(cut, 'a', 62);
  cut[62] = '\0';

  pnut_test_echo_close(&f->server);
  assert_int_equal(pnut_test_echo_serve(f->loop, "echo", 0, &g_all, f,
                                        &f->server), 0);
  f->failwith = PNUT_STATUS_DENIED;
  f->why      = why;
  run(f, count_state, 1);
  assert_int_equal(f->answers, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_DENIED);
  assert_true(f->detail[0]);
  assert_int_equal(f->err[0].code, 9);
  assert_string_equal(f->err[0].text, cut);
}

/* A detail that does not decode is dropped, and the status stands */

static void h_junk(FAR struct pnut_service_s *service,
                   FAR const struct pnut_request_s *req,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  static const uint8_t junk[] =
  {
    0x12, 0x7f, 'x'               /* Field 2, 127 bytes long: cut short */
  };

  assert_int_equal(pnut_reply(req, PNUT_STATUS_DENIED, junk,
                              sizeof(junk)), 0);
}

static void test_gen_fail_junk(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR struct pnut_service_s *junk;

  pnut_test_echo_close(&f->server);
  assert_int_equal(pnut_service_open(f->loop, "echo", g_junk, 1, 0, f,
                                     &junk), 0);
  run(f, say_state, 2);
  assert_int_equal(f->answers, 2);
  assert_int_equal(f->status[0], PNUT_STATUS_DENIED);
  assert_false(f->detail[0]);
  assert_int_equal(f->status[1], PNUT_STATUS_DENIED);
  assert_false(f->detail[1]);
  pnut_service_close(junk);
}

/* Streams: five ticks, with credit for two at first and one more after
 * each: all come, in order, then the end
 */

static void test_gen_stream(void **state)
{
  FAR struct fixture_s *f = *state;
  uint32_t i;

  ticks_setup(f, 5, 2, 1);
  run(f, ticks_state, 1);
  assert_int_equal(f->opened, PNUT_STATUS_OK);
  assert_int_equal(f->got, 5);
  for (i = 0; i < 5; i++)
    {
      assert_int_equal(f->numbers[i], i + 1);
    }

  assert_int_equal(f->ended, PNUT_STATUS_ENDED);
  assert_false(f->stream.open);
  assert_false(f->closed);
}

/* No credit, no ticks: a stream opened with none stays open and silent;
 * opened with three, three come and no more
 */

static void test_gen_stream_credit(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 5, 0, 0);
  run(f, ticks_state, 0);
  assert_int_equal(f->opened, PNUT_STATUS_OK);
  assert_int_equal(f->got, 0);
  assert_int_equal(f->sent, 0);
  assert_true(f->stream.open);
  assert_int_equal(f->ended, -1);
}

static void test_gen_stream_some(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 5, 3, 0);
  run(f, ticks_state, 0);
  assert_int_equal(f->opened, PNUT_STATUS_OK);
  assert_int_equal(f->got, 3);
  assert_int_equal(f->sent, 3);
  assert_true(f->stream.open);
  assert_int_equal(f->ended, -1);
}

/* The client cancels after two ticks: the server is told, and nothing
 * more comes to the client
 */

static void test_gen_stream_cancel(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 10, 2, 1);
  f->cancel_after = 2;
  run(f, ticks_state, 1);
  assert_true(f->closed);
  assert_int_equal(f->closed_status, PNUT_STATUS_ENDED);
  assert_int_equal(f->got, 2);
  assert_int_equal(f->ended, -1);
  assert_false(f->stream.open);
}

/* The client goes after one tick: the server's stream is over,
 * unavailable
 */

static void test_gen_stream_gone(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 10, 2, 1);
  f->drop_after = 1;
  run(f, ticks_state, 1);
  assert_true(f->closed);
  assert_int_equal(f->closed_status, PNUT_STATUS_UNAVAILABLE);
  assert_false(f->stream.open);
}

/* The server fails a stream after two ticks: the client is told */

static void test_gen_stream_failed(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 10, 5, 0);
  f->end_after = 2;
  run(f, ticks_state, 1);
  assert_int_equal(f->got, 2);
  assert_int_equal(f->ended, PNUT_STATUS_INTERNAL);
  assert_false(f->closed);
  assert_false(f->stream.open);
}

/* A stream refused, with a detail: its opening answer says so, no item
 * comes, and its call is free again
 */

static void test_gen_stream_refused(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  ticks_setup(f, 3, 1, 1);
  f->failwith = PNUT_STATUS_DENIED;
  run(f, ticks_state, 1);
  assert_int_equal(f->opened, PNUT_STATUS_DENIED);
  assert_true(f->opened_detail);
  assert_int_equal(f->got, 0);
  assert_int_equal(f->ended, -1);
  for (i = 0; i < PNUT_CLIENT_INFLIGHT; i++)
    {
      assert_false(f->client.calls[i].used);
    }
}

/* A cancel that cannot be sent, the service gone: the stream is over
 * here all the same, its call free
 */

static void test_gen_stream_cancel_gone(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  ticks_setup(f, 10, 1, 0);
  f->cancel_gone = true;
  run(f, ticks_state, 1);
  assert_int_equal(f->got, 1);
  for (i = 0; i < PNUT_CLIENT_INFLIGHT; i++)
    {
      assert_false(f->client.calls[i].used);
    }
}

/* A stream cancelled before its opening answer, its call's place taken
 * by another call: the late answer is dropped, the other call answered
 * as its own, and the service's stream ended when it pushes
 */

static void test_gen_stream_cancel_early(void **state)
{
  FAR struct fixture_s *f = *state;

  ticks_setup(f, 10, 2, 0);
  f->cancel_early = true;
  run(f, ticks_state, 1);
  assert_int_equal(f->answers, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_string_equal(f->out[0].text, "after");
  assert_int_equal(f->opened, -1);
  assert_int_equal(f->got, 0);
}

/* A caller that goes while the service answers it, a stream's closed
 * handler closing the service from inside that answer: nothing of the
 * service is used after (the reviewer's reproductions, ASan's to judge)
 */

static void raw_closed(FAR struct pnut_stream_s *stream, int status,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->closed        = true;
  f->closed_status = status;
  pnut_test_echo_close(&f->server);
}

static void raw_ticks(FAR struct pnut_test_echo_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_test_ticks_request_t *in, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  assert_int_equal(pnut_test_echo_ticks_accept(server, req, &f->stream,
                                               NULL, raw_closed, f), 0);
}

static const struct pnut_test_echo_handlers_s g_raw =
{
  .ticks = raw_ticks,
};

static void raw_send(int fd, uint8_t kind, uint16_t method, uint16_t seq,
                     uint16_t credit, FAR const uint8_t *body, uint16_t len)
{
  struct pnut_msghdr_s hdr;
  uint8_t buf[PNUT_MSG_HEADER + 8];

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = kind;
  hdr.iface    = PNUT_TEST_ECHO_IFACE;
  hdr.method   = method;
  hdr.mversion = 1;
  hdr.seq      = seq;
  hdr.len      = len;
  hdr.credit   = credit;
  pnut_msg_encode(&hdr, buf);
  if (len > 0)
    {
      memcpy(buf + PNUT_MSG_HEADER, body, len);
    }

  assert_int_equal(write(fd, buf, PNUT_MSG_HEADER + len),
                   PNUT_MSG_HEADER + len);
}

static void raw_open(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  static const uint8_t count[] =
  {
    0x08, 0x05                    /* count = 5 */
  };

  struct sockaddr_un addr;

  f->raw = socket(AF_UNIX, SOCK_STREAM, 0);
  assert_true(f->raw >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/echo", f->rundir);
  assert_int_equal(connect(f->raw, (FAR struct sockaddr *)&addr,
                           sizeof(addr)), 0);
  raw_send(f->raw, PNUT_KIND_REQUEST, PNUT_TEST_ECHO_TICKS, 1, 0, count,
           sizeof(count));
}

static void raw_leave(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  struct pnut_msghdr_s hdr;
  uint8_t buf[PNUT_MSG_HEADER];

  assert_int_equal(read(f->raw, buf, sizeof(buf)), PNUT_MSG_HEADER);
  assert_int_equal(pnut_msg_decode(buf, &hdr), 0);
  assert_int_equal(hdr.kind, PNUT_KIND_REPLY);
  assert_int_equal(hdr.status, PNUT_STATUS_OK);

  /* Something the service answers, then gone before the answer */

  if (f->raw_request)
    {
      raw_send(f->raw, PNUT_KIND_REQUEST, 77, 2, 0, NULL, 0);
    }
  else
    {
      raw_send(f->raw, PNUT_KIND_STREAM, PNUT_TEST_ECHO_TICKS, 999, 1,
               NULL, 0);
    }

  close(f->raw);
  f->raw = -1;
}

static void raw_run(FAR struct fixture_s *f, bool request)
{
  pnut_test_echo_close(&f->server);
  assert_int_equal(pnut_test_echo_serve(f->loop, "echo", 0, &g_raw, f,
                                        &f->server), 0);
  f->raw_request = request;
  assert_int_equal(pnut_timer_start(f->loop, 0, 0, raw_open, f, NULL), 0);
  assert_int_equal(pnut_timer_start(f->loop, 150, 0, raw_leave, f, NULL),
                   0);
  assert_int_equal(pnut_timer_start(f->loop, 400, 0, stop_timer, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_true(f->closed);
  assert_int_equal(f->closed_status, PNUT_STATUS_UNAVAILABLE);
  assert_false(f->stream.open);
}

static void test_gen_stream_closing(void **state)
{
  raw_run(*state, false);
}

static void test_gen_stream_closing_answer(void **state)
{
  raw_run(*state, true);
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
    cmocka_unit_test_setup_teardown(test_gen_fail, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_fail_bare, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_fail_long, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_fail_junk, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_credit, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_some, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_cancel, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_gone, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_failed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_refused, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_cancel_gone, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_cancel_early, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_closing, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_gen_stream_closing_answer, setup,
                                    teardown),
  };

  return cmocka_run_group_tests_name("gen", tests, NULL, NULL);
}
