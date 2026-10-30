/****************************************************************************
 * pnut-os/tests/unit/test_at.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony's AT engine (RFC 0030): commands one at a time, their answers
 * sorted from what the modem sends unasked, timeouts on the loop.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "at.h"
#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct at_test_s
{
  FAR struct pnut_loop_s *loop;
  struct pnut_module_s module;
  struct at_s at;
  char sent[1024];                /* What went to the modem */
  size_t nsent;
  bool full;                      /* The channel takes nothing now */
  char urcs[1024];                /* Lines sent unasked, each ended */
  int results[16];
  char replies[16][128];
  int ndone;
  bool requeue;                   /* A handler queues another */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int at_test_send(FAR void *arg, FAR const char *buf, size_t len)
{
  FAR struct at_test_s *t = arg;

  if (t->full)
    {
      return -EAGAIN;
    }

  assert_true(t->nsent + len < sizeof(t->sent));
  memcpy(t->sent + t->nsent, buf, len);
  t->nsent += len;
  t->sent[t->nsent] = '\0';
  return 0;
}

static void at_test_urc(FAR void *arg, FAR const char *line)
{
  FAR struct at_test_s *t = arg;

  strlcat(t->urcs, line, sizeof(t->urcs));
  strlcat(t->urcs, "|", sizeof(t->urcs));
}

static const struct at_ops_s g_at_test_ops =
{
  .send = at_test_send,
  .urc  = at_test_urc,
};

static void at_test_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct at_test_s *t = arg;

  assert_true(t->ndone < 16);
  t->results[t->ndone] = result;
  strlcpy(t->replies[t->ndone], reply, sizeof(t->replies[0]));
  t->ndone++;

  if (t->requeue)
    {
      t->requeue = false;
      assert_int_equal(at_command(&t->at, "AT+NEW", NULL, 0, 0,
                                  at_test_done, t), 0);
    }
}

static void at_test_init(FAR struct at_test_s *t)
{
  memset(t, 0, sizeof(*t));
  assert_int_equal(pnut_loop_create(NULL, &t->loop), 0);
  t->module.name = "at";
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);
  at_init(&t->at, &t->module, &g_at_test_ops, t);
}

static void at_test_feed(FAR struct at_test_s *t, FAR const char *text)
{
  at_input(&t->at, text, strlen(text));
}

static void at_test_stop(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

/****************************************************************************
 * Name: test_at_answers
 *
 * Description:
 *   Commands go one at a time; their lines are kept, an echo left out, and
 *   what the modem sends unasked goes apart, between answers too.
 *
 ****************************************************************************/

static void test_at_answers(void **state)
{
  struct at_test_s t;

  at_test_init(&t);

  assert_int_equal(at_command(&t.at, "AT+CSQ", "+CSQ:", 0, 0, at_test_done,
                              &t), 0);
  assert_int_equal(at_command(&t.at, "AT+CPIN?", "+CPIN:", 0, 0,
                              at_test_done, &t), 0);

  /* Only the first is out */

  assert_string_equal(t.sent, "AT+CSQ\r");

  /* Its echo, a registration report in the middle, its answer */

  at_test_feed(&t, "AT+CSQ\r\r\n+CEREG: 1\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
  assert_int_equal(t.ndone, 1);
  assert_int_equal(t.results[0], 0);
  assert_string_equal(t.replies[0], "+CSQ: 20,99\n");
  assert_string_equal(t.urcs, "+CEREG: 1|");

  /* The next went out; +CPIN: is its answer here, unasked elsewhere */

  assert_string_equal(t.sent, "AT+CSQ\rAT+CPIN?\r");
  at_test_feed(&t, "\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n");
  assert_int_equal(t.ndone, 2);
  assert_string_equal(t.replies[1], "+CPIN: SIM PIN\n");

  /* With nothing out, it is unasked */

  at_test_feed(&t, "\r\n+CPIN: READY\r\n");
  assert_string_equal(t.urcs, "+CEREG: 1|+CPIN: READY|");

  /* An error ends a command, its line kept */

  assert_int_equal(at_command(&t.at, "AT+CPIN?", "+CPIN:", 0, 0,
                              at_test_done, &t), 0);
  at_test_feed(&t, "\r\n+CME ERROR: SIM not inserted\r\n");
  assert_int_equal(t.ndone, 3);
  assert_int_equal(t.results[2], -EIO);
  assert_string_equal(t.replies[2], "+CME ERROR: SIM not inserted\n");
  assert_true(at_idle(&t.at));

  pnut_loop_destroy(t.loop);
}

/****************************************************************************
 * Name: test_at_timeout
 *
 * Description:
 *   A command unanswered times out on the loop, and the next goes.
 *
 ****************************************************************************/

static void test_at_timeout(void **state)
{
  struct at_test_s t;

  at_test_init(&t);
  assert_int_equal(at_command(&t.at, "AT", NULL, 30, 0, at_test_done, &t),
                   0);
  assert_int_equal(at_command(&t.at, "ATI", NULL, 30, 0, at_test_done, &t),
                   0);
  assert_int_equal(pnut_timer_start(t.loop, 200, 0, at_test_stop, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(t.loop), 0);

  assert_int_equal(t.ndone, 2);
  assert_int_equal(t.results[0], -ETIMEDOUT);
  assert_int_equal(t.results[1], -ETIMEDOUT);
  assert_string_equal(t.sent, "AT\rATI\r");
  assert_int_equal(t.loop->timers.avail, t.loop->config.timers);
  pnut_loop_destroy(t.loop);
}

/****************************************************************************
 * Name: test_at_queue
 *
 * Description:
 *   The queue's bound; a channel with no room; a cancel, whose handlers
 *   may queue more; a secret wiped once done; a line too long cut.
 *
 ****************************************************************************/

static void test_at_queue(void **state)
{
  struct at_test_s t;
  int i;

  at_test_init(&t);

  for (i = 0; i < AT_QUEUE; i++)
    {
      assert_int_equal(at_command(&t.at, "AT", NULL, 0, 0, at_test_done,
                                  &t), 0);
    }

  assert_int_equal(at_command(&t.at, "AT", NULL, 0, 0, at_test_done, &t),
                   -EBUSY);

  /* A cancel ends them all; one handler's new command goes after */

  t.requeue = true;
  at_cancel(&t.at, -ECANCELED);
  assert_int_equal(t.ndone, AT_QUEUE);
  for (i = 0; i < AT_QUEUE; i++)
    {
      assert_int_equal(t.results[i], -ECANCELED);
    }

  assert_string_equal(t.sent, "AT\rAT+NEW\r");
  at_test_feed(&t, "OK\r\n");
  assert_true(at_idle(&t.at));

  /* No room: sent once there is */

  t.full  = true;
  t.ndone = 0;
  t.nsent = 0;
  t.sent[0] = '\0';
  assert_int_equal(at_command(&t.at, "AT+CPIN=\"1234\"", NULL, 0, AT_SECRET,
                              at_test_done, &t), 0);
  assert_string_equal(t.sent, "");
  t.full = false;
  at_room(&t.at);
  assert_string_equal(t.sent, "AT+CPIN=\"1234\"\r");
  at_test_feed(&t, "OK\r\n");
  assert_int_equal(t.ndone, 1);
  assert_int_equal(t.results[0], 0);

  /* The secret is gone from the queue */

  for (i = 0; i < AT_QUEUE; i++)
    {
      assert_null(strstr(t.at.queue[i].command, "1234"));
    }

  /* A line past AT_LINE_MAX is cut, and the next read whole */

  for (i = 0; i < AT_LINE_MAX + 50; i++)
    {
      at_input(&t.at, "x", 1);
    }

  at_test_feed(&t, "\r\nRING\r\n");
  assert_int_equal(strlen(t.urcs), AT_LINE_MAX - 1 + 1 + 5);
  assert_non_null(strstr(t.urcs, "|RING|"));

  pnut_loop_destroy(t.loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_at_answers),
    cmocka_unit_test(test_at_timeout),
    cmocka_unit_test(test_at_queue),
  };

  return cmocka_run_group_tests_name("at", tests, NULL, NULL);
}
