/****************************************************************************
 * pnut-os/src/lib/timer.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: timer_arm
 *
 * Description:
 *   Set the timer descriptor for the soonest timer, or disarm it.
 *
 ****************************************************************************/

static void timer_arm(FAR struct pnut_loop_s *loop)
{
  struct itimerspec its;
  uint64_t now;
  uint64_t wait;

  memset(&its, 0, sizeof(its));

  if (loop->armed != NULL)
    {
      /* A zero value disarms, so a timer already due waits one tick */

      now  = pnut_now();
      wait = loop->armed->deadline > now ?
             loop->armed->deadline - now : 0;

      its.it_value.tv_sec  = wait / 1000;
      its.it_value.tv_nsec = (wait % 1000) * 1000000;
      if (wait == 0)
        {
          its.it_value.tv_nsec = 1;
        }
    }

  timerfd_settime(loop->timerfd, 0, &its, NULL);
}

/****************************************************************************
 * Name: timer_insert
 *
 * Description:
 *   Put a timer in the list, after those due at the same time.
 *
 ****************************************************************************/

static void timer_insert(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer)
{
  FAR struct pnut_timer_s **link = &loop->armed;

  while (*link != NULL && (*link)->deadline <= timer->deadline)
    {
      link = &(*link)->next;
    }

  timer->next = *link;
  *link = timer;
  timer->state = PNUT_TIMER_ARMED;
}

/****************************************************************************
 * Name: timer_unlink
 *
 * Description:
 *   Take an armed timer out of the list, and set the timer descriptor for
 *   the next if it was the soonest.
 *
 ****************************************************************************/

static void timer_unlink(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer)
{
  FAR struct pnut_timer_s **link;
  bool first = loop->armed == timer;

  for (link = &loop->armed; *link != NULL; link = &(*link)->next)
    {
      if (*link == timer)
        {
          *link = timer->next;
          timer->next = NULL;
          break;
        }
    }

  if (first)
    {
      timer_arm(loop);
    }
}

/****************************************************************************
 * Name: timer_expired
 *
 * Description:
 *   Run the handlers of the timers that are due.
 *
 ****************************************************************************/

static void timer_expired(FAR struct pnut_loop_s *loop, int fd,
                          uint32_t events, FAR void *arg)
{
  FAR struct pnut_timer_s *timer;
  uint64_t expirations;
  uint64_t start;
  uint64_t after;
  uint64_t now;

  while (read(fd, &expirations, sizeof(expirations)) > 0)
    {
    }

  now = pnut_now();

  while (loop->running && loop->armed != NULL &&
         loop->armed->deadline <= now)
    {
      timer = loop->armed;
      loop->armed = timer->next;
      timer->next = NULL;
      timer->state = PNUT_TIMER_FIRING;

      start = pnut_now();
      timer->handler(loop, timer, timer->arg);
      pnut_loop_budget(loop, start, "timer");

      if (timer->state == PNUT_TIMER_CANCELLED ||
          (!timer->kept && timer->period == 0))
        {
          pnut_pool_free(&loop->timers, timer);
        }
      else if (timer->kept)
        {
          /* Set again by its handler, or idle until it is */

          if (timer->state == PNUT_TIMER_REARMED)
            {
              timer_insert(loop, timer);
            }
          else
            {
              timer->state = PNUT_TIMER_IDLE;
            }
        }
      else
        {
          /* Expiries missed while the loop or the handler was busy are
           * skipped: a slow handler still leaves a period before the next.
           */

          after = pnut_now();
          timer->deadline += timer->period;
          if (timer->deadline <= after)
            {
              timer->deadline = after + timer->period;
            }

          timer_insert(loop, timer);
        }
    }

  timer_arm(loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_timer_init(FAR struct pnut_loop_s *loop)
{
  int ret;

  ret = pnut_pool_init(&loop->timers, sizeof(struct pnut_timer_s),
                       loop->config.timers + PNUT_LOOP_READY(loop));
  if (ret < 0)
    {
      return ret;
    }

  loop->timerfd = timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (loop->timerfd < 0)
    {
      return -errno;
    }

  return pnut_loop_watch(loop, loop->timerfd, EPOLLIN, timer_expired,
                         NULL);
}

void pnut_timer_deinit(FAR struct pnut_loop_s *loop)
{
  if (loop->timerfd >= 0)
    {
      close(loop->timerfd);
      loop->timerfd = -1;
    }

  loop->armed = NULL;
  pnut_pool_deinit(&loop->timers);
}

int pnut_timer_start(FAR struct pnut_loop_s *loop, uint32_t delay,
                     uint32_t period, pnut_timer_handler_t handler,
                     FAR void *arg, FAR struct pnut_timer_s **timerp)
{
  FAR struct pnut_timer_s *timer;

  if (handler == NULL)
    {
      return -EINVAL;
    }

  timer = pnut_pool_alloc(&loop->timers);
  if (timer == NULL)
    {
      return -EBUSY;
    }

  timer->deadline = pnut_now() + delay;
  timer->period   = period;
  timer->kept     = false;
  timer->handler  = handler;
  timer->arg      = arg;

  timer_insert(loop, timer);
  if (loop->armed == timer)
    {
      timer_arm(loop);
    }

  if (timerp != NULL)
    {
      *timerp = timer;
    }

  return OK;
}

void pnut_timer_cancel(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer)
{
  if (timer == NULL)
    {
      return;
    }

  /* Its own handler runs: it is freed when the handler returns */

  if (timer->state != PNUT_TIMER_ARMED)
    {
      timer->state = PNUT_TIMER_CANCELLED;
      return;
    }

  timer_unlink(loop, timer);
  pnut_pool_free(&loop->timers, timer);
}

int pnut_timer_keep(FAR struct pnut_loop_s *loop,
                    pnut_timer_handler_t handler, FAR void *arg,
                    FAR struct pnut_timer_s **timerp)
{
  FAR struct pnut_timer_s *timer;

  timer = pnut_pool_alloc(&loop->timers);
  if (timer == NULL)
    {
      return -EBUSY;
    }

  timer->next     = NULL;
  timer->deadline = 0;
  timer->period   = 0;
  timer->state    = PNUT_TIMER_IDLE;
  timer->kept     = true;
  timer->handler  = handler;
  timer->arg      = arg;

  *timerp = timer;
  return OK;
}

void pnut_timer_set(FAR struct pnut_loop_s *loop,
                    FAR struct pnut_timer_s *timer, uint32_t delay)
{
  timer->deadline = pnut_now() + delay;

  switch (timer->state)
    {
      case PNUT_TIMER_FIRING:
      case PNUT_TIMER_REARMED:

        /* From its own handler: put back in the list once it returns */

        timer->state = PNUT_TIMER_REARMED;
        return;

      case PNUT_TIMER_ARMED:
        timer_unlink(loop, timer);
        break;

      default:
        break;
    }

  timer_insert(loop, timer);
  if (loop->armed == timer)
    {
      timer_arm(loop);
    }
}

void pnut_timer_clear(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer)
{
  switch (timer->state)
    {
      case PNUT_TIMER_ARMED:
        timer_unlink(loop, timer);
        timer->state = PNUT_TIMER_IDLE;
        break;

      case PNUT_TIMER_REARMED:

        /* From its own handler: idle once it returns */

        timer->state = PNUT_TIMER_FIRING;
        break;

      default:
        break;
    }
}

void pnut_timer_drop(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer)
{
  if (timer == NULL)
    {
      return;
    }

  switch (timer->state)
    {
      case PNUT_TIMER_FIRING:
      case PNUT_TIMER_REARMED:

        /* From its own handler: freed once it returns */

        timer->state = PNUT_TIMER_CANCELLED;
        return;

      case PNUT_TIMER_ARMED:
        timer_unlink(loop, timer);
        break;

      default:
        break;
    }

  pnut_pool_free(&loop->timers, timer);
}
