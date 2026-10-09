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

#include <pnut/client.h>
#include <pnut/compiler.h>
#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/pool.h>
#include <pnut/service.h>
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

#ifndef CONFIG_PNUT_LIB_RUNDIR
#  define CONFIG_PNUT_LIB_RUNDIR            "/var/run"
#endif

#ifndef CONFIG_PNUT_LIB_CONNS
#  define CONFIG_PNUT_LIB_CONNS             8
#endif

#ifndef CONFIG_PNUT_LIB_INFLIGHT
#  define CONFIG_PNUT_LIB_INFLIGHT          8
#endif

#ifndef CONFIG_PNUT_LIB_CALL_TIMEOUT
#  define CONFIG_PNUT_LIB_CALL_TIMEOUT      5000
#endif

#ifndef CONFIG_PNUT_LIB_RECONNECT_MIN
#  define CONFIG_PNUT_LIB_RECONNECT_MIN     100
#endif

#ifndef CONFIG_PNUT_LIB_RECONNECT_MAX
#  define CONFIG_PNUT_LIB_RECONNECT_MAX     5000
#endif

#ifndef CONFIG_PNUT_LIB_SENDBUF
#  define CONFIG_PNUT_LIB_SENDBUF           8192
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
  PNUT_TIMER_IDLE,                /* Kept, not armed */
  PNUT_TIMER_REARMED,             /* Kept, set again by its handler */
};

/* A timer runs once, periodically, or is kept: a kept timer stays in the
 * pool from pnut_timer_keep() to pnut_timer_drop(), and is set and
 * cleared any number of times in between, so that its owner never runs
 * short of one.
 */

struct pnut_timer_s
{
  FAR struct pnut_timer_s *next;
  uint64_t deadline;              /* Milliseconds, monotonic */
  uint32_t period;                /* Milliseconds; zero for once */
  uint8_t state;
  bool kept;
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

/* A connection: a socket with a receive buffer of one message and a send
 * buffer the loop drains as the socket takes more.  Its generation changes
 * whenever it closes, so that an endpoint kept from before sees it.
 *
 * Its owner may leave a message for later, when the send buffer has no
 * room for what it would answer: the connection is then held, reading
 * nothing, and offers the message again once the buffer has drained, or
 * once the owner says room was given back (pnut_conn_recheck()).
 */

enum pnut_conn_next_e
{
  PNUT_CONN_NEXT = 0,             /* Handled: on to the next message */
  PNUT_CONN_GONE,                 /* The owner, or the connection, gone */
  PNUT_CONN_WAIT,                 /* Offer it again when there is room */
};

struct pnut_conn_s
{
  FAR struct pnut_loop_s *loop;
  int fd;                         /* -1 when closed */
  uint32_t gen;
  uint32_t events;                /* What the loop watches it for */
  bool held;                      /* A message waits for room */
  bool recheck;                   /* Held, and room was given back */
  uint8_t pending;                /* Requests not answered yet */
  size_t reserved;                /* Kept for their answers, in bytes */
  FAR uint8_t *rx;                /* PNUT_MSG_MAX bytes */
  uint16_t rxlen;
  FAR uint8_t *tx;
  size_t txlen;
  size_t txsize;
  pid_t pid;                      /* The peer, from SO_PEERCRED */
  uid_t uid;
  gid_t gid;

  /* The owner's handlers: a message has come in (a PNUT_CONN_ value);
   * the peer has gone.
   */

  CODE int (*message)(FAR struct pnut_conn_s *conn,
                      FAR const struct pnut_msghdr_s *hdr,
                      FAR const uint8_t *payload);
  CODE void (*closed)(FAR struct pnut_conn_s *conn);
  FAR void *owner;
};

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

/* timer.c: kept timers.  keep returns -EBUSY when the pool is empty; set
 * (re)arms it delay ms from now, also from its own handler; clear disarms
 * it; drop gives it back, also from its own handler.
 */

int pnut_timer_keep(FAR struct pnut_loop_s *loop,
                    pnut_timer_handler_t handler, FAR void *arg,
                    FAR struct pnut_timer_s **timerp);
void pnut_timer_set(FAR struct pnut_loop_s *loop,
                    FAR struct pnut_timer_s *timer, uint32_t delay);
void pnut_timer_clear(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer);
void pnut_timer_drop(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer);

/* worker.c */

int pnut_worker_init(FAR struct pnut_loop_s *loop);
void pnut_worker_deinit(FAR struct pnut_loop_s *loop);

/* conn.c */

int pnut_conn_init(FAR struct pnut_conn_s *conn,
                   FAR struct pnut_loop_s *loop, size_t txsize);
void pnut_conn_deinit(FAR struct pnut_conn_s *conn);
int pnut_conn_attach(FAR struct pnut_conn_s *conn, int fd);
void pnut_conn_close(FAR struct pnut_conn_s *conn);
int pnut_conn_send(FAR struct pnut_conn_s *conn,
                   FAR const struct pnut_msghdr_s *hdr,
                   FAR const void *payload);
void pnut_conn_recheck(FAR struct pnut_conn_s *conn);

/* module.c */

int pnut_module_startall(FAR struct pnut_loop_s *loop);
void pnut_module_stopall(FAR struct pnut_loop_s *loop);

#endif /* __PNUT_OS_SRC_LIB_PNUT_INTERNAL_H */
