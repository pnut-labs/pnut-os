/****************************************************************************
 * pnut-os/src/lib/pnut_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SRC_LIB_PNUT_INTERNAL_H
#define __PNUT_OS_SRC_LIB_PNUT_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>

#include <pnut/compiler.h>
#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/pool.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The defaults, for the build on the computer, where Kconfig sets none */

#ifndef CONFIG_PNUT_LIB_FDS
#  define CONFIG_PNUT_LIB_FDS               16
#endif

#ifndef CONFIG_PNUT_LIB_TIMERS
#  define CONFIG_PNUT_LIB_TIMERS            16
#endif

#ifndef CONFIG_PNUT_LIB_JOBS
#  define CONFIG_PNUT_LIB_JOBS              8
#endif

#ifndef CONFIG_PNUT_LIB_WORKERS
#  define CONFIG_PNUT_LIB_WORKERS           1
#endif

#ifndef CONFIG_PNUT_LIB_WORKER_STACKSIZE
#  define CONFIG_PNUT_LIB_WORKER_STACKSIZE  4096
#endif

#ifndef CONFIG_PNUT_LIB_BUDGET
#  define CONFIG_PNUT_LIB_BUDGET            10
#endif

#ifndef CONFIG_PNUT_LIB_LOG_LINE
#  define CONFIG_PNUT_LIB_LOG_LINE          160
#endif

/* The loop watches three descriptors of its own: signals, timers and the
 * workers' results.
 */

#define PNUT_LOOP_OWN_FDS  3

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A descriptor the loop watches.  One that is unwatched while the loop
 * handles a batch of events stays dead until the batch is over, so that a
 * later event of the same batch does not reach it.
 */

struct pnut_watch_s
{
  int fd;                         /* -1 when the slot is free */
  bool dead;
  pnut_fd_handler_t handler;
  FAR void *arg;
};

enum pnut_timer_state_e
{
  PNUT_TIMER_ARMED = 0,
  PNUT_TIMER_FIRING,              /* Its handler runs */
  PNUT_TIMER_CANCELLED,           /* Cancelled by its handler */
};

struct pnut_timer_s
{
  FAR struct pnut_timer_s *next;
  uint64_t deadline;              /* Milliseconds, monotonic */
  uint32_t period;                /* Milliseconds; zero for once */
  uint8_t state;
  pnut_timer_handler_t handler;
  FAR void *arg;
};

struct pnut_job_s
{
  FAR struct pnut_job_s *next;
  pnut_job_run_t run;
  pnut_job_done_t done;
  FAR void *arg;
};

struct pnut_loop_s
{
  struct pnut_loop_config_s config;
  bool running;
  int status;
  bool sigblocked;                /* create blocked SIGTERM and SIGINT */
  sigset_t sigprev;               /* The mask before, to restore them */

  /* Descriptors */

  int epfd;
  int sigfd;
  int timerfd;
  int eventfd;
  FAR struct pnut_watch_s *watches;
  FAR struct epoll_event *events;
  uint16_t nwatches;
  bool dispatching;               /* Handling a batch of events */

  /* Timers, soonest first */

  struct pnut_pool_s timers;
  FAR struct pnut_timer_s *armed;

  /* Modules, in the order they were added */

  FAR struct pnut_module_s *first;
  FAR struct pnut_module_s *last;
  bool ready;

  /* Workers.  The jobs' pool is used on the loop only; the queues are
   * shared with the workers, under lock.
   */

  struct pnut_pool_s jobs;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  FAR struct pnut_job_s *pending;
  FAR struct pnut_job_s *pending_last;
  FAR struct pnut_job_s *done;
  FAR struct pnut_job_s *done_last;
  FAR pthread_t *threads;
  uint8_t nthreads;
  bool quit;
  bool sync;                      /* lock and cond are initialised */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* clock.c */

uint64_t pnut_now(void);

/* log.c: a line for the loop itself, marked with the program's name */

void pnut_loop_log(FAR struct pnut_loop_s *loop, int level,
                   FAR const char *fmt, ...) printf_like(3, 4);

/* loop.c: a handler that ran longer than its budget is logged */

void pnut_loop_budget(FAR struct pnut_loop_s *loop, uint64_t start,
                      FAR const char *what);

/* timer.c */

int pnut_timer_init(FAR struct pnut_loop_s *loop);
void pnut_timer_deinit(FAR struct pnut_loop_s *loop);

/* worker.c */

int pnut_worker_init(FAR struct pnut_loop_s *loop);
void pnut_worker_deinit(FAR struct pnut_loop_s *loop);

/* module.c */

int pnut_module_startall(FAR struct pnut_loop_s *loop);
void pnut_module_stopall(FAR struct pnut_loop_s *loop);

#endif /* __PNUT_OS_SRC_LIB_PNUT_INTERNAL_H */
