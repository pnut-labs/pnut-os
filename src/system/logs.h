/****************************************************************************
 * pnut-os/src/system/logs.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_LOGS_H
#define __PNUT_OS_SYSTEM_LOGS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pnut/module.h>
#include <pnut/timer.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A line of the RAM log, its time, level and program included; one longer
 * is cut
 */

#define LOGS_LINE_MAX     256

/* A crash report's name: crash-YYYYMMDD-HHMMSS, and a number when two
 * come in a second
 */

#define LOGS_CRASH_NAME   32

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Log (RFC 0026), so far the part that keeps the log across restarts: a
 * module of the system program that follows NuttX's RAM log and copies
 * its warnings and errors into system.log in its directory, rotated by
 * size (system.log.1, .2, ...), on a worker, every few seconds or once
 * enough has gathered, and as it stops.  As it starts, it copies the
 * lines the RAM log kept that the files have not, those a reset cut off
 * included, and marks the start in system.log.  A crash's dump in the RAM
 * log, of this start or one before a reset, is saved as a crash report,
 * crash-<time>, the last few kept.  Each service's own level, the
 * Developer options, and Log's interface come later.
 */

struct logs_config_s
{
  FAR const char *source;         /* The RAM log, or NULL */

  /* The RAM log, read again whole for a crash's report: source when NULL
   * (the tests follow a FIFO, and give a file for this)
   */

  FAR const char *reread;
  FAR const char *dir;            /* The files' directory */
  uint32_t file_size;             /* A file's bytes, at most */
  uint8_t files;                  /* system.log and the rotated ones */
  uint8_t crashes;                /* Crash reports kept */
  uint32_t crash_size;            /* A crash report's bytes, at most */
  uint32_t delay;                 /* From a line to its write, in ms */

  /* Bytes of lines waiting that start a write at once; the lines waiting
   * while one is written are dropped, and counted, past twice this
   */

  uint32_t batch;

  /* The wall clock, for the start's mark and the crash reports' names:
   * the kernel's when NULL (the tests give their own)
   */

  CODE int (*gettime)(FAR void *arg, FAR struct timespec *ts);
  FAR void *clock;
};

/* A write, on a worker: the module fills it in, and does not touch it
 * again until finished
 */

struct logs_write_s
{
  FAR const char *dir;
  uint32_t file_size;
  uint8_t files;
  uint8_t crashes;

  FAR char *data;                 /* Lines for system.log, or NULL */
  size_t len;
  FAR char *crash;                /* A crash report, or NULL */
  size_t crash_len;
  char crash_name[LOGS_CRASH_NAME];

  /* The worker's */

  int ret;                        /* system.log's: a negated errno value */
  int crash_ret;                  /* The report's */
  bool finished;
};

struct logs_s
{
  struct pnut_module_s module;    /* First: the module is the log */
  struct logs_config_s config;

  int fd;                         /* The RAM log; -1 none */
  bool watching;

  /* The lines waiting, and the buffer being written */

  FAR char *pending;
  size_t len;
  FAR char *spare;
  struct logs_write_s write;
  bool writing;
  FAR struct pnut_timer_s *timer; /* Until the next write */

  /* A crash report waiting for the write, or NULL */

  FAR char *crash;
  size_t crash_len;
  char crash_name[LOGS_CRASH_NAME];

  /* A crash's dump coming in: saved once it has been quiet a moment */

  FAR struct pnut_timer_s *quiet;

  /* The line being read */

  char line[LOGS_LINE_MAX];
  size_t linelen;

  uint32_t dropped;               /* Lines that found no room */
  bool failing;                   /* A write failed, logged */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: logs_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop.
 *
 ****************************************************************************/

void logs_init(FAR struct logs_s *logs,
               FAR const struct logs_config_s *config);

/****************************************************************************
 * Name: logs_deinit
 *
 * Description:
 *   Free its buffers, once the loop is destroyed: a write may run on a
 *   worker until then.
 *
 ****************************************************************************/

void logs_deinit(FAR struct logs_s *logs);

#endif /* __PNUT_OS_SYSTEM_LOGS_H */
