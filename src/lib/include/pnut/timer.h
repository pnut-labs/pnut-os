/****************************************************************************
 * pnut-os/src/lib/include/pnut/timer.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_TIMER_H
#define __PNUT_OS_LIB_PNUT_TIMER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct pnut_timer_s;

/* Called on the loop when a timer expires */

typedef CODE void (*pnut_timer_handler_t)(FAR struct pnut_loop_s *loop,
                                          FAR struct pnut_timer_s *timer,
                                          FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_timer_start
 *
 * Description:
 *   Start a timer: handler is called after delay ms, then every period ms,
 *   or once if period is zero.  A handle is valid until the timer is
 *   cancelled, or until a timer that runs once has run its handler; a
 *   stale handle may name a newer timer.
 *
 *   A timer's place in the pool is given back after its handler returns,
 *   so a handler that starts its successor needs one place more than the
 *   timers alive at once; or use a periodic timer.
 *
 * Input Parameters:
 *   loop    - The loop.
 *   delay   - Milliseconds until the first expiry.
 *   period  - Milliseconds between expiries; zero for once.
 *   handler - Called on the loop.
 *   arg     - Passed to the handler.
 *   timerp  - Where to return the timer, or NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY when the loop runs as many timers as it
 *   can; another negated errno.
 *
 ****************************************************************************/

int pnut_timer_start(FAR struct pnut_loop_s *loop, uint32_t delay,
                     uint32_t period, pnut_timer_handler_t handler,
                     FAR void *arg, FAR struct pnut_timer_s **timerp);

/****************************************************************************
 * Name: pnut_timer_cancel
 *
 * Description:
 *   Cancel a timer that has not expired, or a periodic one; it may be
 *   called from the timer's own handler.
 *
 ****************************************************************************/

void pnut_timer_cancel(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer);

#endif /* __PNUT_OS_LIB_PNUT_TIMER_H */
