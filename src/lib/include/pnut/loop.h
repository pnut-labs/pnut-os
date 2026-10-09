/****************************************************************************
 * pnut-os/src/lib/include/pnut/loop.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_LOOP_H
#define __PNUT_OS_LIB_PNUT_LOOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A program's event loop (RFC 0023).  A program has one, made in its main
 * thread; every call of the library takes it, since in NuttX's flat build
 * all programs share one address space and the library keeps no state of
 * its own.  The library's functions are called from the loop's thread
 * (its handlers), except where a header says otherwise.
 */

struct pnut_loop_s;

/* How a loop is sized: everything it needs is taken when it is made */

struct pnut_loop_config_s
{
  FAR const char *name;           /* The program's name, for the log */
  uint16_t fds;                   /* Descriptors watched at once */
  uint16_t timers;                /* Timers running at once */
  uint16_t jobs;                  /* Jobs queued or running at once */
  uint8_t workers;                /* Worker threads */
  size_t stacksize;               /* A worker's stack, in bytes */
  uint32_t budget;                /* A handler's budget, in ms */
};

/* Called on the loop when a watched descriptor is ready.  events are
 * epoll's (EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLHUP).
 */

typedef CODE void (*pnut_fd_handler_t)(FAR struct pnut_loop_s *loop,
                                       int fd, uint32_t events,
                                       FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_loop_defaults
 *
 * Description:
 *   Fill in a configuration with the defaults (CONFIG_PNUT_LIB_*).
 *
 ****************************************************************************/

void pnut_loop_defaults(FAR struct pnut_loop_config_s *config);

/****************************************************************************
 * Name: pnut_loop_create
 *
 * Description:
 *   Make a loop, with its pools and workers.  SIGTERM and SIGINT are
 *   blocked in the calling thread, and arrive on the loop instead, where
 *   they stop it; call this from the program's main thread, before making
 *   any other thread.
 *
 * Input Parameters:
 *   config - The sizes; NULL for the defaults.
 *   loopp  - Where to return the loop.
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL if fds is beyond what a loop can watch;
 *   another negated errno value on failure.
 *
 ****************************************************************************/

int pnut_loop_create(FAR const struct pnut_loop_config_s *config,
                     FAR struct pnut_loop_s **loopp);

/****************************************************************************
 * Name: pnut_loop_destroy
 *
 * Description:
 *   Stop the workers and free the loop.  It waits for the jobs that are
 *   running, so a job must finish in bounded time (no read without a
 *   timeout); jobs still queued are dropped, and their done handlers are
 *   never called.  SIGTERM and SIGINT are unblocked again, unless they
 *   were blocked before pnut_loop_create().
 *
 ****************************************************************************/

void pnut_loop_destroy(FAR struct pnut_loop_s *loop);

/****************************************************************************
 * Name: pnut_loop_run
 *
 * Description:
 *   Start the modules, in the order they were added, and run the loop until
 *   pnut_loop_stop() or a SIGTERM; then stop the modules, in reverse order.
 *
 * Returned Value:
 *   The status given to pnut_loop_stop(); zero after a signal; a negated
 *   errno value if a module failed to start or the loop failed.
 *
 ****************************************************************************/

int pnut_loop_run(FAR struct pnut_loop_s *loop);

/****************************************************************************
 * Name: pnut_loop_stop
 *
 * Description:
 *   Make pnut_loop_run() return status, once the current handler returns.
 *
 ****************************************************************************/

void pnut_loop_stop(FAR struct pnut_loop_s *loop, int status);

/****************************************************************************
 * Name: pnut_loop_watch
 *
 * Description:
 *   Watch a descriptor: handler is called on the loop when it is ready for
 *   events.
 *
 *   Unwatch a descriptor before closing it.  A descriptor unwatched by a
 *   handler frees its place only once the handlers of the current events
 *   have run.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY when the loop watches as many descriptors
 *   as it can; -EEXIST if fd is watched already; another negated errno.
 *
 ****************************************************************************/

int pnut_loop_watch(FAR struct pnut_loop_s *loop, int fd, uint32_t events,
                    pnut_fd_handler_t handler, FAR void *arg);

/****************************************************************************
 * Name: pnut_loop_rewatch
 *
 * Description:
 *   Change the events a watched descriptor is watched for.  -EINVAL for
 *   the loop's own descriptors.
 *
 ****************************************************************************/

int pnut_loop_rewatch(FAR struct pnut_loop_s *loop, int fd,
                      uint32_t events);

/****************************************************************************
 * Name: pnut_loop_unwatch
 *
 * Description:
 *   Stop watching a descriptor.  Its handler is not called again, even for
 *   events already waiting; the descriptor itself stays open.  -EINVAL for
 *   the loop's own descriptors.
 *
 ****************************************************************************/

int pnut_loop_unwatch(FAR struct pnut_loop_s *loop, int fd);

/****************************************************************************
 * Name: pnut_loop_name
 ****************************************************************************/

FAR const char *pnut_loop_name(FAR struct pnut_loop_s *loop);

#endif /* __PNUT_OS_LIB_PNUT_LOOP_H */
