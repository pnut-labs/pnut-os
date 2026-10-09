/****************************************************************************
 * pnut-os/src/lib/include/pnut/worker.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_WORKER_H
#define __PNUT_OS_LIB_PNUT_WORKER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A job runs on a worker thread, away from the loop, and its result comes
 * back to the loop (RFC 0023).  It gets copies of its input through arg,
 * never a module's state; it calls none of the library's functions, and
 * it must finish in bounded time (see pnut_loop_destroy()).
 */

typedef CODE void (*pnut_job_run_t)(FAR void *arg);

/* Called on the loop once the job has run */

typedef CODE void (*pnut_job_done_t)(FAR struct pnut_loop_s *loop,
                                     FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_job_submit
 *
 * Description:
 *   Queue a job for the workers.  Call it from the loop.
 *
 * Input Parameters:
 *   loop - The loop.
 *   run  - Runs on a worker.
 *   done - Called on the loop after run returns, or NULL.
 *   arg  - Passed to both.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY when the queue is full; -ENOSYS when the
 *   loop has no workers.
 *
 ****************************************************************************/

int pnut_job_submit(FAR struct pnut_loop_s *loop, pnut_job_run_t run,
                    pnut_job_done_t done, FAR void *arg);

#endif /* __PNUT_OS_LIB_PNUT_WORKER_H */
