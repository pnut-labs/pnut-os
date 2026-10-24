/****************************************************************************
 * pnut-os/tests/unit/test_logs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Log's module (RFC 0026) on a RAM log the test writes: a FIFO it follows,
 * and a file standing for the RAM log read again whole.  The tests look
 * at the files Log writes.  On the computer only: they need a FIFO and
 * FIONREAD on a file, which the simulator's configuration lacks.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "logs.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 24 October 2026, 12:00:00 UTC */

#define T0          1792843200
#define T0_NAME     "crash-20261024-120000"
#define T0_START    "---- start 2026-10-24T12:00:00Z"
#define T0_STOP     "---- stop 2026-10-24T12:00:00Z ----"

/* A crash's dump, as NuttX's assertion writes it */

#define DUMP \
  "[   10.000000] [ ALERT] sh: _assert: Current Version: NuttX 13.1.0\r\n" \
  "[   10.000100] [ ALERT] sh: _assert: Assertion failed panic: at file: " \
  "x.c:1 task: sh\r\n" \
  "[   10.000200] [ ALERT] sh: dump_stack: sp: 0x3fc80000\r\n"

#define LINES_MAX   64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct logs_test_s;
typedef CODE bool (*logs_until_t)(FAR struct logs_test_s *t);
typedef CODE void (*logs_step_t)(FAR struct logs_test_s *t);

struct logs_test_s
{
  FAR struct pnut_loop_s *loop;
  struct logs_s logs;
  struct logs_config_s config;
  char dir[64];                   /* Log's files */
  char fifo[96];                  /* The RAM log, followed */
  char ring[96];                  /* The RAM log, read again whole */
  int writer;                     /* The FIFO's writing end */
  logs_until_t until;             /* Stops the loop once true */
  logs_step_t step;               /* Runs once, 50 ms in */
  int waited;                     /* ms */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int test_gettime(FAR void *arg, FAR struct timespec *ts)
{
  ts->tv_sec  = T0;
  ts->tv_nsec = 0;
  return 0;
}

static void test_path(FAR struct logs_test_s *t, FAR const char *name,
                      FAR char *path, size_t size)
{
  snprintf(path, size, "%s/%s", t->dir, name);
}

/* A file's whole content, NUL-terminated, malloc()ed; NULL if none */

static FAR char *test_read(FAR struct logs_test_s *t, FAR const char *name)
{
  char path[160];
  FAR char *data;
  struct stat st;
  FAR FILE *f;

  test_path(t, name, path, sizeof(path));
  if (stat(path, &st) < 0)
    {
      return NULL;
    }

  data = malloc(st.st_size + 1);
  assert_non_null(data);
  f = fopen(path, "r");
  assert_non_null(f);
  assert_int_equal(fread(data, 1, st.st_size, f), st.st_size);
  fclose(f);
  data[st.st_size] = '\0';
  return data;
}

/* A file's lines, split in place */

static int test_lines(FAR char *data, FAR char **lines)
{
  FAR char *nl;
  int n = 0;

  while (*data != '\0' && n < LINES_MAX)
    {
      lines[n++] = data;
      nl = strchr(data, '\n');
      assert_non_null(nl);
      *nl = '\0';
      data = nl + 1;
    }

  return n;
}

static void test_write_file(FAR const char *path, FAR const char *text)
{
  FAR FILE *f = fopen(path, "w");

  assert_non_null(f);
  fputs(text, f);
  fclose(f);
}

/* Lines into the RAM log: the FIFO Log follows, and the whole RAM log's
 * file
 */

static void test_ring(FAR struct logs_test_s *t, FAR const char *text)
{
  FAR FILE *f;

  assert_int_equal(write(t->writer, text, strlen(text)),
                   (ssize_t)strlen(text));
  f = fopen(t->ring, "a");
  assert_non_null(f);
  fputs(text, f);
  fclose(f);
}

static void test_setup(FAR struct logs_test_s *t)
{
  char cmd[96];

  memset(t, 0, sizeof(*t));
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-logs-%d", (int)getpid());
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->fifo, sizeof(t->fifo), "%s.fifo", t->dir);
  snprintf(t->ring, sizeof(t->ring), "%s.ring", t->dir);
  unlink(t->fifo);
  unlink(t->ring);
  assert_int_equal(mkfifo(t->fifo, 0600), 0);
  test_write_file(t->ring, "");

  /* Always a writer: Log never sees the FIFO end */

  t->writer = open(t->fifo, O_RDWR | O_NONBLOCK);
  assert_true(t->writer >= 0);

  t->config.source     = t->fifo;
  t->config.reread     = t->ring;
  t->config.dir        = t->dir;
  t->config.file_size  = 4096;
  t->config.files      = 3;
  t->config.crashes    = 2;
  t->config.crash_size = 4096;
  t->config.delay      = 50;
  t->config.batch      = 1024;
  t->config.gettime    = test_gettime;
}

static void test_teardown(FAR struct logs_test_s *t)
{
  char cmd[96];

  close(t->writer);
  unlink(t->fifo);
  unlink(t->ring);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
}

static void test_tick(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct logs_test_s *t = arg;

  t->waited += 10;
  if (t->waited == 50 && t->step != NULL)
    {
      t->step(t);
    }

  if ((t->waited > 50 && t->until != NULL && t->until(t)) ||
      t->waited >= 5000)
    {
      pnut_loop_stop(loop, 0);
    }
}

/* Run Log until the test's condition holds (5 s at most), then stop it */

static void test_run(FAR struct logs_test_s *t, logs_until_t until,
                     logs_step_t step)
{
  logs_init(&t->logs, &t->config);
  t->until  = until;
  t->step   = step;
  t->waited = 0;
  assert_int_equal(pnut_loop_create(NULL, &t->loop), 0);
  assert_int_equal(pnut_module_add(t->loop, &t->logs.module), 0);
  assert_int_equal(pnut_timer_start(t->loop, 10, 10, test_tick, t, NULL),
                   0);
  assert_int_equal(pnut_loop_run(t->loop), 0);
  pnut_loop_destroy(t->loop);
  logs_deinit(&t->logs);
  assert_true(t->waited < 5000);
}

static bool test_has_log(FAR struct logs_test_s *t)
{
  char path[160];
  struct stat st;

  test_path(t, "system.log", path, sizeof(path));
  return stat(path, &st) == 0 && st.st_size > 0;
}

static bool test_has_crash(FAR struct logs_test_s *t)
{
  char path[160];
  struct stat st;

  test_path(t, T0_NAME, path, sizeof(path));
  return stat(path, &st) == 0;
}

static int test_count(FAR struct logs_test_s *t, FAR const char *prefix)
{
  FAR struct dirent *e;
  FAR DIR *d;
  int n = 0;

  d = opendir(t->dir);
  assert_non_null(d);
  while ((e = readdir(d)) != NULL)
    {
      n += strncmp(e->d_name, prefix, strlen(prefix)) == 0;
    }

  closedir(d);
  return n;
}

static bool test_starts(FAR const char *line, FAR const char *prefix)
{
  return strncmp(line, prefix, strlen(prefix)) == 0;
}

/* Warnings and errors kept, the rest not, with no carriage return; the
 * start's mark first, the stop's last
 */

static void test_logs_keeps(void **state)
{
  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  FAR char *log;

  test_setup(&t);
  test_ring(&t, "[    1.000000] [  INFO] sh: hello\r\n"
                "[    1.100000] [  WARN] pnut_system: time: odd\r\n"
                "[    1.200000] [ ERROR] pnut_system: x: broke\r\n"
                "[    1.300000] [ DEBUG] pnut_system: x: detail\r\n"
                "[    1.400000] [NOTICE] sh: note\r\n"
                "[    1.500000] [  CRIT] pnut_system: x: worse\r\n"
                "[    1.600000] [ ALERT] sh: _assert: Current Version\r\n"
                "no header at all\r\n");
  test_run(&t, test_has_log, NULL);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_int_equal(test_lines(log, lines), 5);
  assert_true(test_starts(lines[0], T0_START));
  assert_string_equal(lines[1],
                      "[    1.100000] [  WARN] pnut_system: time: odd");
  assert_string_equal(lines[2],
                      "[    1.200000] [ ERROR] pnut_system: x: broke");
  assert_string_equal(lines[3],
                      "[    1.500000] [  CRIT] pnut_system: x: worse");
  assert_string_equal(lines[4], T0_STOP);
  free(log);

  /* The alert's line is a dump: a report's, not system.log's */

  assert_int_equal(test_count(&t, "crash-"), 1);
  test_teardown(&t);
}

/* Lines that come while Log runs: written after the delay */

static void test_step_more(FAR struct logs_test_s *t)
{
  test_ring(t, "[    2.000000] [  WARN] sh: later\r\n"
               "[    2.100000] [  INFO] sh: not kept\r\n");
}

static bool test_has_later(FAR struct logs_test_s *t)
{
  FAR char *log = test_read(t, "system.log");
  bool found = log != NULL && strstr(log, "sh: later\n") != NULL;

  free(log);
  return found;
}

static void test_logs_follows(void **state)
{
  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  FAR char *log;

  test_setup(&t);
  test_run(&t, test_has_later, test_step_more);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_int_equal(test_lines(log, lines), 3);
  assert_true(test_starts(lines[0], T0_START));
  assert_string_equal(lines[1], "[    2.000000] [  WARN] sh: later");
  assert_string_equal(lines[2], T0_STOP);
  free(log);
  test_teardown(&t);
}

/* A start after a reset: the lines after the last one copied, those the
 * reset cut off first, then the start's mark before the new start's
 */

static void test_logs_catch_up(void **state)
{
  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  char path[160];
  FAR char *log;

  test_setup(&t);
  test_path(&t, "system.log", path, sizeof(path));
  test_write_file(path, "---- start 2026-10-23T08:00:00Z ----\n"
                        "[    5.000000] [  WARN] a: saved\n");
  test_ring(&t, "[    4.000000] [  WARN] a: old\r\n"
                "[    5.000000] [  WARN] a: saved\r\n"
                "[    6.000000] [ ERROR] a: lost at the reset\r\n"
                "[    0.002000] [  WARN] b: new start\r\n"
                "[    0.003000] [  INFO] b: info\r\n");
  test_run(&t, test_has_later, test_step_more);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_int_equal(test_lines(log, lines), 7);
  assert_string_equal(lines[1], "[    5.000000] [  WARN] a: saved");
  assert_string_equal(lines[2],
                      "[    6.000000] [ ERROR] a: lost at the reset");
  assert_true(test_starts(lines[3], T0_START));
  assert_string_equal(lines[4], "[    0.002000] [  WARN] b: new start");
  assert_string_equal(lines[5], "[    2.000000] [  WARN] sh: later");
  assert_string_equal(lines[6], T0_STOP);
  free(log);
  test_teardown(&t);
}

/* A start before that logged nothing after its first moments: the drop
 * small, still a new start; lines a preemption put out of order, not one
 */

static void test_logs_short_start(void **state)
{
  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  FAR char *log;

  test_setup(&t);
  test_ring(&t, "[    0.035000] [  WARN] a: previous start\r\n"
                "[    0.371000] [  INFO] a: ready\r\n"
                "[    0.002000] [  INFO] a: started\r\n"
                "[    0.034000] [  WARN] a: this start\r\n"
                "[    0.371500] [  INFO] b: out of order\r\n"
                "[    0.371400] [  INFO] c: by a preemption\r\n");
  test_run(&t, test_has_log, NULL);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_int_equal(test_lines(log, lines), 4);
  assert_string_equal(lines[0], "[    0.035000] [  WARN] a: previous start");
  assert_true(test_starts(lines[1], T0_START));
  assert_string_equal(lines[2], "[    0.034000] [  WARN] a: this start");
  free(log);
  test_teardown(&t);
}

/* A start of the program alone, the device not reset: the lines after
 * the last copied, with no drop in their times, after the mark
 */

static void test_logs_restart(void **state)
{
  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  char path[160];
  FAR char *log;

  test_setup(&t);
  test_path(&t, "system.log", path, sizeof(path));
  test_write_file(path, "[    5.000000] [  WARN] a: saved\n"
                        T0_STOP "\n");
  test_ring(&t, "[    5.000000] [  WARN] a: saved\r\n"
                "[    5.000001] [  WARN] a: after\r\n");
  test_run(&t, test_has_later, test_step_more);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_int_equal(test_lines(log, lines), 6);
  assert_true(test_starts(lines[2], T0_START));
  assert_string_equal(lines[3], "[    5.000001] [  WARN] a: after");
  free(log);
  test_teardown(&t);
}

/* Rotation by size: whole lines, the files at most their size, the oldest
 * gone
 */

static void test_logs_rotates(void **state)
{
  FAR const char *names[] =
  {
    "system.log.2", "system.log.1", "system.log"
  };

  FAR char *lines[LINES_MAX];
  struct logs_test_s t;
  char all[1600];
  char line[64];
  FAR char *log;
  int next = -1;
  int i;
  int n;

  test_setup(&t);
  t.config.file_size = 512;

  for (i = 0; i < 40; i++)
    {
      snprintf(line, sizeof(line),
               "[%5d.000000] [  WARN] sh: line %02d of forty\r\n", i, i);
      test_ring(&t, line);
    }

  test_run(&t, test_has_log, NULL);

  /* Three files, none past its size, the oldest gone: what is left, in
   * order, is whole lines, the forty's last ones in turn, then the stop
   */

  assert_int_equal(test_count(&t, "system.log"), 3);
  all[0] = '\0';
  for (i = 0; i < 3; i++)
    {
      log = test_read(&t, names[i]);
      assert_non_null(log);
      assert_true(strlen(log) <= 512);
      assert_true(strlen(log) > 0 && log[strlen(log) - 1] == '\n');
      assert_true(strlen(all) + strlen(log) < sizeof(all));
      strcat(all, log);
      free(log);
    }

  n = test_lines(all, lines);
  assert_true(n > 20);
  for (i = 0; i < n - 1; i++)
    {
      int a;
      int b;

      if (test_starts(lines[i], "---- "))
        {
          continue;
        }

      assert_int_equal(sscanf(lines[i],
                              "[%d.000000] [  WARN] sh: line %d of forty",
                              &a, &b), 2);
      assert_int_equal(a, b);
      assert_true(next < 0 || b == next + 1);
      next = b;
    }

  assert_int_equal(next, 39);
  assert_string_equal(lines[n - 1], T0_STOP);
  test_teardown(&t);
}

/* A crash's dump left by a reset: the report, up to the dump's end; in
 * system.log only the assertion's line
 */

static void test_logs_crash_at_start(void **state)
{
  struct logs_test_s t;
  FAR char *report;
  FAR char *log;

  test_setup(&t);
  test_ring(&t, "[    9.000000] [  INFO] sh: before\r\n" DUMP
                "[    0.001000] [  INFO] AppBringUp: Started\r\n");
  test_run(&t, test_has_crash, NULL);

  report = test_read(&t, T0_NAME);
  assert_non_null(report);
  assert_string_equal(report, "[    9.000000] [  INFO] sh: before\r\n" DUMP);
  free(report);

  log = test_read(&t, "system.log");
  assert_non_null(log);
  assert_non_null(strstr(log, "Assertion failed panic"));
  assert_null(strstr(log, "Current Version"));
  assert_null(strstr(log, "dump_stack"));
  free(log);
  test_teardown(&t);
}

/* A dump whose start a small RAM log lost, the assertion's line with it:
 * still a report
 */

static void test_logs_crash_cut(void **state)
{
  struct logs_test_s t;
  FAR char *report;

  test_setup(&t);
  test_ring(&t, "[   10.000300] [ ALERT] sh: dump_task: 8 sh\r\n"
                "[   10.000400] [ EMERG] sh: backtrace| 8: 0x42069606\r\n"
                "[   10.000500] [  INFO] sh: reboot status=1\r\n");
  test_run(&t, test_has_crash, NULL);

  report = test_read(&t, T0_NAME);
  assert_non_null(report);
  assert_string_equal(report,
                      "[   10.000300] [ ALERT] sh: dump_task: 8 sh\r\n"
                      "[   10.000400] [ EMERG] sh: backtrace| 8: "
                      "0x42069606\r\n");
  free(report);
  test_teardown(&t);
}

/* A report is the dump and the lines before it, up to its size, from a
 * whole line
 */

static void test_logs_crash_size(void **state)
{
  struct logs_test_s t;
  FAR char *report;
  char line[80];
  int i;

  test_setup(&t);
  for (i = 0; i < 100; i++)
    {
      snprintf(line, sizeof(line),
               "[%5d.000000] [  INFO] sh: line %02d before the crash\r\n",
               i, i);
      test_ring(&t, line);
    }

  test_ring(&t, DUMP);
  test_run(&t, test_has_crash, NULL);

  report = test_read(&t, T0_NAME);
  assert_non_null(report);
  assert_true(strlen(report) <= 4096);
  assert_true(strlen(report) > 4096 - 80);
  assert_true(test_starts(report, "[   "));
  assert_non_null(strstr(report, "line 99 before the crash\r\n" DUMP));
  assert_null(strstr(report, "line 00 "));
  free(report);
  test_teardown(&t);
}

/* A dump that a line after it says is saved: no report again */

static void test_logs_crash_saved(void **state)
{
  struct logs_test_s t;

  test_setup(&t);
  test_ring(&t, DUMP "[   11.000000] [  WARN] pnut_system: log: Crash "
                "report saved: /var/lib/log/" T0_NAME "\r\n");
  test_run(&t, test_has_log, NULL);
  assert_int_equal(test_count(&t, "crash-"), 0);
  test_teardown(&t);
}

/* A task's assertion while Log runs: the report once the dump is over */

static void test_step_crash(FAR struct logs_test_s *t)
{
  test_ring(t, "[   20.000000] [  WARN] sh: just before\r\n" DUMP);
}

static void test_logs_crash_live(void **state)
{
  struct logs_test_s t;
  FAR char *report;

  test_setup(&t);
  test_run(&t, test_has_crash, test_step_crash);

  report = test_read(&t, T0_NAME);
  assert_non_null(report);
  assert_string_equal(report,
                      "[   20.000000] [  WARN] sh: just before\r\n" DUMP);
  free(report);
  test_teardown(&t);
}

/* A dump saved already, though the line saying so is missing from the
 * RAM log: no report again
 */

static void test_logs_crash_known(void **state)
{
  struct logs_test_s t;
  char path[160];

  test_setup(&t);
  test_path(&t, "crash-20261023-080000", path, sizeof(path));
  test_write_file(path, "[    9.000000] [  INFO] sh: before\r\n" DUMP);
  test_ring(&t, "[    9.000000] [  INFO] sh: before\r\n" DUMP
                "[    0.001000] [  WARN] AppBringUp: Started\r\n");
  test_run(&t, test_has_log, NULL);
  assert_int_equal(test_count(&t, "crash-"), 1);
  test_teardown(&t);
}

/* The reports kept: the oldest go */

static void test_logs_crash_trim(void **state)
{
  struct logs_test_s t;
  char path[160];
  FAR char *old;

  test_setup(&t);
  test_path(&t, "crash-20200101-000000", path, sizeof(path));
  test_write_file(path, "oldest\n");
  test_path(&t, "crash-20210101-000000", path, sizeof(path));
  test_write_file(path, "older\n");
  test_ring(&t, DUMP);
  test_run(&t, test_has_crash, NULL);

  assert_int_equal(test_count(&t, "crash-"), 2);
  old = test_read(&t, "crash-20200101-000000");
  assert_null(old);
  old = test_read(&t, "crash-20210101-000000");
  assert_non_null(old);
  free(old);
  test_teardown(&t);
}

/* No RAM log: Log keeps nothing, and the program runs on */

static bool test_after_100ms(FAR struct logs_test_s *t)
{
  return t->waited >= 100;
}

static void test_logs_no_source(void **state)
{
  struct logs_test_s t;

  test_setup(&t);
  t.config.source = "/nonexistent/kmsg";
  test_run(&t, test_after_100ms, NULL);
  assert_int_equal(test_count(&t, "system.log"), 0);
  test_teardown(&t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_logs_keeps),
    cmocka_unit_test(test_logs_follows),
    cmocka_unit_test(test_logs_catch_up),
    cmocka_unit_test(test_logs_short_start),
    cmocka_unit_test(test_logs_restart),
    cmocka_unit_test(test_logs_rotates),
    cmocka_unit_test(test_logs_crash_at_start),
    cmocka_unit_test(test_logs_crash_cut),
    cmocka_unit_test(test_logs_crash_size),
    cmocka_unit_test(test_logs_crash_saved),
    cmocka_unit_test(test_logs_crash_live),
    cmocka_unit_test(test_logs_crash_known),
    cmocka_unit_test(test_logs_crash_trim),
    cmocka_unit_test(test_logs_no_source),
  };

  return cmocka_run_group_tests_name("logs", tests, NULL, NULL);
}
