/****************************************************************************
 * pnut-os/tests/unit/test_log.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The library's log lines (RFC 0026): bounded in rate, one line each, cut
 * at a whole character.  On the computer only: it reads the lines from
 * the standard error, where the library logs there; on NuttX they go to
 * syslog, so src/tests leaves it out.
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
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The standard error, where the library logs on the computer, caught in a
 * file
 */

struct catch_s
{
  FAR FILE *file;
  int saved;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char g_out[64 * 1024];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void catch_start(FAR struct catch_s *c)
{
  fflush(stderr);
  c->file = tmpfile();
  assert_non_null(c->file);
  c->saved = dup(STDERR_FILENO);
  assert_true(c->saved >= 0);
  assert_true(dup2(fileno(c->file), STDERR_FILENO) >= 0);
}

/* The lines caught, in g_out; the standard error as it was */

static FAR const char *catch_end(FAR struct catch_s *c)
{
  size_t n;

  fflush(stderr);
  dup2(c->saved, STDERR_FILENO);
  close(c->saved);
  rewind(c->file);
  n = fread(g_out, 1, sizeof(g_out) - 1, c->file);
  g_out[n] = '\0';
  fclose(c->file);
  return g_out;
}

static int count_lines(FAR const char *s)
{
  int n = 0;

  for (; *s != '\0'; s++)
    {
      n += *s == '\n';
    }

  return n;
}

static void module_init(FAR struct pnut_module_s *m, FAR const char *name)
{
  memset(m, 0, sizeof(*m));
  m->name  = name;
  m->level = PNUT_LOG_INFO;
}

/* A second, and a little, so that a module's next line is in a new one */

static void next_second(void)
{
  struct timespec ts;

  ts.tv_sec  = 1;
  ts.tv_nsec = 100 * 1000 * 1000;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    {
    }
}

static void test_log_rate(void **state)
{
  struct pnut_module_s m;
  struct catch_s c;
  char want[80];
  FAR const char *out;
  int i;

  module_init(&m, "m");

  /* A second's lines up to the rate; lines under the module's level are
   * not counted
   */

  catch_start(&c);
  for (i = 0; i < CONFIG_PNUT_LIB_LOG_RATE + 10; i++)
    {
      pnut_log(&m, PNUT_LOG_DEBUG, "quiet %d", i);
      pnut_log(&m, PNUT_LOG_INFO, "line %d", i);
    }

  out = catch_end(&c);
  assert_int_equal(count_lines(out), CONFIG_PNUT_LIB_LOG_RATE);
  assert_non_null(strstr(out, "m: line 0\n"));
  assert_null(strstr(out, "quiet"));
  assert_null(strstr(out, "dropped"));

  /* The next line, a second on, says how many went first */

  next_second();
  catch_start(&c);
  pnut_log(&m, PNUT_LOG_ERROR, "again");
  out = catch_end(&c);
  snprintf(want, sizeof(want),
           "m: Lines dropped, over %d a second: 10\nm: again\n",
           CONFIG_PNUT_LIB_LOG_RATE);
  assert_string_equal(out, want);

  /* Said once */

  next_second();
  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "once more");
  out = catch_end(&c);
  assert_string_equal(out, "m: once more\n");
}

static int start_flood_fail(FAR struct pnut_module_s *m,
                            FAR struct pnut_loop_s *loop);

static int start_flood(FAR struct pnut_module_s *m,
                       FAR struct pnut_loop_s *loop)
{
  int i;

  for (i = 0; i < CONFIG_PNUT_LIB_LOG_RATE + 3; i++)
    {
      pnut_log(m, PNUT_LOG_WARNING, "flood %d", i);
    }

  if (m->start == start_flood_fail)
    {
      return -EIO;
    }

  pnut_module_ready(m);
  pnut_loop_stop(loop, 0);
  return 0;
}

static int start_flood_fail(FAR struct pnut_module_s *m,
                            FAR struct pnut_loop_s *loop)
{
  return start_flood(m, loop);
}

static void test_log_dropped_at_stop(void **state)
{
  FAR struct pnut_loop_s *loop;
  struct pnut_module_s m;
  struct catch_s c;
  char want[80];
  FAR const char *out;
  int ret;

  module_init(&m, "flood");
  m.start = start_flood;

  /* A module that stops says how many lines it dropped */

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &m), 0);

  /* Asserted after: cmocka's own messages go to the standard error */

  catch_start(&c);
  ret = pnut_loop_run(loop);
  out = catch_end(&c);
  pnut_loop_destroy(loop);
  assert_int_equal(ret, 0);

  snprintf(want, sizeof(want), "flood: flood %d\n",
           CONFIG_PNUT_LIB_LOG_RATE - 1);
  assert_non_null(strstr(out, want));
  snprintf(want, sizeof(want), "flood: flood %d\n",
           CONFIG_PNUT_LIB_LOG_RATE);
  assert_null(strstr(out, want));
  snprintf(want, sizeof(want),
           "flood: Lines dropped, over %d a second: 3\n",
           CONFIG_PNUT_LIB_LOG_RATE);
  assert_non_null(strstr(out, want));

  /* So does one that fails to start: its line saying so is the fourth
   * over the rate
   */

  module_init(&m, "flood");
  m.start = start_flood_fail;
  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_module_add(loop, &m), 0);

  catch_start(&c);
  ret = pnut_loop_run(loop);
  out = catch_end(&c);
  pnut_loop_destroy(loop);
  assert_int_equal(ret, -EIO);
  snprintf(want, sizeof(want),
           "flood: Lines dropped, over %d a second: 4\n",
           CONFIG_PNUT_LIB_LOG_RATE);
  assert_non_null(strstr(out, want));
}

static void test_log_one_line(void **state)
{
  struct pnut_module_s m;
  struct catch_s c;
  FAR const char *out;

  module_init(&m, "m");

  /* Control characters become spaces: the message stays one line */

  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "a\nb\tc\r%s\x7f", "d\x1b");
  out = catch_end(&c);
  assert_string_equal(out, "m: a b c d  \n");
}

static void test_log_cut(void **state)
{
  char text[2 * CONFIG_PNUT_LIB_LOG_LINE];
  char want[2 * CONFIG_PNUT_LIB_LOG_LINE + 8];
  struct pnut_module_s m;
  struct catch_s c;
  FAR const char *out;
  size_t keep = CONFIG_PNUT_LIB_LOG_LINE - 1;

  module_init(&m, "m");

  /* A long message is cut to the line's bytes */

  memset(text, 'x', sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';

  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "%s", text);
  out = catch_end(&c);
  snprintf(want, sizeof(want), "m: %.*s\n", (int)keep, text);
  assert_string_equal(out, want);

  /* A character cut short goes whole: a two-byte é across the end */

  memset(text, 'x', keep - 1);
  text[keep - 1] = (char)0xc3;
  text[keep]     = (char)0xa9;
  text[keep + 1] = '\0';

  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "%s", text);
  out = catch_end(&c);
  snprintf(want, sizeof(want), "m: %.*s\n", (int)(keep - 1), text);
  assert_string_equal(out, want);

  /* A three-byte one, € (e2 82 ac), with two of its bytes in */

  memset(text, 'x', keep - 2);
  text[keep - 2] = (char)0xe2;
  text[keep - 1] = (char)0x82;
  text[keep]     = (char)0xac;
  text[keep + 1] = '\0';

  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "%s", text);
  out = catch_end(&c);
  snprintf(want, sizeof(want), "m: %.*s\n", (int)(keep - 2), text);
  assert_string_equal(out, want);

  /* One that fits whole, at the cut, stays */

  memset(text, 'x', keep - 2);
  text[keep - 2] = (char)0xc3;
  text[keep - 1] = (char)0xa9;
  text[keep]     = 'x';
  text[keep + 1] = '\0';

  catch_start(&c);
  pnut_log(&m, PNUT_LOG_INFO, "%s", text);
  out = catch_end(&c);
  snprintf(want, sizeof(want), "m: %.*s\n", (int)keep, text);
  assert_string_equal(out, want);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_log_rate),
    cmocka_unit_test(test_log_dropped_at_stop),
    cmocka_unit_test(test_log_one_line),
    cmocka_unit_test(test_log_cut),
  };

  return cmocka_run_group_tests_name("log", tests, NULL, NULL);
}
