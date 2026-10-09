/****************************************************************************
 * pnut-os/src/lib/loop.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: loop_find
 ****************************************************************************/

static FAR struct pnut_watch_s *loop_find(FAR struct pnut_loop_s *loop,
                                          int fd)
{
  uint16_t i;

  for (i = 0; i < loop->nwatches; i++)
    {
      if (loop->watches[i].fd == fd)
        {
          return &loop->watches[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: loop_signal
 *
 * Description:
 *   SIGTERM or SIGINT has arrived: stop the loop.
 *
 ****************************************************************************/

static void loop_signal(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  struct signalfd_siginfo info;

  while (read(fd, &info, sizeof(info)) == sizeof(info))
    {
      pnut_loop_log(loop, PNUT_LOG_INFO, "Signal %d: stopping",
                    (int)info.ssi_signo);
      pnut_loop_stop(loop, 0);
    }
}

/****************************************************************************
 * Name: loop_sigset
 *
 * Description:
 *   The signals the loop takes: SIGTERM and SIGINT.
 *
 ****************************************************************************/

static void loop_sigset(FAR sigset_t *mask)
{
  sigemptyset(mask);
  sigaddset(mask, SIGTERM);
  sigaddset(mask, SIGINT);
}

/****************************************************************************
 * Name: loop_signals
 *
 * Description:
 *   Block SIGTERM and SIGINT in the calling thread, and take them on a
 *   descriptor instead.
 *
 ****************************************************************************/

static int loop_signals(FAR struct pnut_loop_s *loop)
{
  sigset_t mask;
  int ret;

  loop_sigset(&mask);

  ret = pthread_sigmask(SIG_BLOCK, &mask, &loop->sigprev);
  if (ret != 0)
    {
      return -ret;
    }

  loop->sigblocked = true;

  loop->sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (loop->sigfd < 0)
    {
      return -errno;
    }

  return pnut_loop_watch(loop, loop->sigfd, EPOLLIN, loop_signal, NULL);
}

/****************************************************************************
 * Name: loop_ownfd
 *
 * Description:
 *   Whether a descriptor is one of the loop's own, which programs may not
 *   unwatch or change.
 *
 ****************************************************************************/

static bool loop_ownfd(FAR struct pnut_loop_s *loop, int fd)
{
  return fd == loop->sigfd || fd == loop->timerfd || fd == loop->eventfd;
}

/****************************************************************************
 * Name: loop_reap
 *
 * Description:
 *   Free the slots of descriptors unwatched during a batch of events.
 *
 ****************************************************************************/

static void loop_reap(FAR struct pnut_loop_s *loop)
{
  uint16_t i;

  for (i = 0; i < loop->nwatches; i++)
    {
      loop->watches[i].dead = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pnut_loop_defaults(FAR struct pnut_loop_config_s *config)
{
  memset(config, 0, sizeof(*config));
  config->name      = "pnut";
  config->fds       = CONFIG_PNUT_LIB_FDS;
  config->timers    = CONFIG_PNUT_LIB_TIMERS;
  config->jobs      = CONFIG_PNUT_LIB_JOBS;
  config->workers   = CONFIG_PNUT_LIB_WORKERS;
  config->stacksize = CONFIG_PNUT_LIB_WORKER_STACKSIZE;
  config->budget    = CONFIG_PNUT_LIB_BUDGET;
  config->rundir    = CONFIG_PNUT_LIB_RUNDIR;
}

int pnut_loop_create(FAR const struct pnut_loop_config_s *config,
                     FAR struct pnut_loop_s **loopp)
{
  FAR struct pnut_loop_s *loop;
  uint16_t i;
  int ret;

  *loopp = NULL;

  if (config != NULL && config->fds > UINT16_MAX - PNUT_LOOP_OWN_FDS)
    {
      return -EINVAL;
    }

  loop = calloc(1, sizeof(*loop));
  if (loop == NULL)
    {
      return -ENOMEM;
    }

  if (config != NULL)
    {
      loop->config = *config;
    }
  else
    {
      pnut_loop_defaults(&loop->config);
    }

  loop->epfd    = -1;
  loop->sigfd   = -1;
  loop->timerfd = -1;
  loop->eventfd = -1;

  loop->nwatches = loop->config.fds + PNUT_LOOP_OWN_FDS;
  loop->watches  = calloc(loop->nwatches, sizeof(*loop->watches));
  loop->events   = calloc(loop->nwatches, sizeof(*loop->events));
  if (loop->watches == NULL || loop->events == NULL)
    {
      ret = -ENOMEM;
      goto errout;
    }

  for (i = 0; i < loop->nwatches; i++)
    {
      loop->watches[i].fd = -1;
    }

  /* NuttX takes the memory for every descriptor epoll watches when it is
   * created, sized here; past that it would take more at each watch.
   */

#ifdef __NuttX__
  loop->epfd = epoll_create(loop->nwatches);
  if (loop->epfd >= 0 && fcntl(loop->epfd, F_SETFD, FD_CLOEXEC) < 0)
    {
      ret = -errno;
      goto errout;
    }
#else
  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
#endif
  if (loop->epfd < 0)
    {
      ret = -errno;
      goto errout;
    }

  ret = loop_signals(loop);
  if (ret < 0)
    {
      goto errout;
    }

  ret = pnut_timer_init(loop);
  if (ret < 0)
    {
      goto errout;
    }

  ret = pnut_worker_init(loop);
  if (ret < 0)
    {
      goto errout;
    }

  *loopp = loop;
  return OK;

errout:
  pnut_loop_destroy(loop);
  return ret;
}

void pnut_loop_destroy(FAR struct pnut_loop_s *loop)
{
  sigset_t mask;

  if (loop == NULL)
    {
      return;
    }

  pnut_worker_deinit(loop);
  pnut_timer_deinit(loop);

  if (loop->sigfd >= 0)
    {
      close(loop->sigfd);
    }

  if (loop->epfd >= 0)
    {
      close(loop->epfd);
    }

  /* With nothing to read them, the signals must reach the program again:
   * unblock those that create blocked, once the descriptors are gone.
   */

  if (loop->sigblocked)
    {
      loop_sigset(&mask);
      if (sigismember(&loop->sigprev, SIGTERM))
        {
          sigdelset(&mask, SIGTERM);
        }

      if (sigismember(&loop->sigprev, SIGINT))
        {
          sigdelset(&mask, SIGINT);
        }

      pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    }

  free(loop->events);
  free(loop->watches);
  free(loop);
}

int pnut_loop_run(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_watch_s *watch;
  uint64_t start;
  int n;
  int i;
  int ret;

  loop->running = true;
  loop->status  = OK;

  ret = pnut_module_startall(loop);
  if (ret < 0)
    {
      loop->running = false;
      return ret;
    }

  while (loop->running)
    {
      n = epoll_wait(loop->epfd, loop->events, loop->nwatches, -1);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          loop->status = -errno;
          break;
        }

      loop->dispatching = true;

      for (i = 0; i < n && loop->running; i++)
        {
          watch = loop->events[i].data.ptr;
          if (watch->fd < 0 || watch->dead)
            {
              continue;
            }

          /* The loop's own handlers time the handlers they call */

          start = pnut_now();
          watch->handler(loop, watch->fd, loop->events[i].events,
                         watch->arg);

          if (!loop_ownfd(loop, watch->fd))
            {
              pnut_loop_budget(loop, start, "descriptor");
            }
        }

      loop->dispatching = false;
      loop_reap(loop);
    }

  loop->running = false;
  pnut_module_stopall(loop);
  return loop->status;
}

void pnut_loop_stop(FAR struct pnut_loop_s *loop, int status)
{
  loop->status  = status;
  loop->running = false;
}

int pnut_loop_watch(FAR struct pnut_loop_s *loop, int fd, uint32_t events,
                    pnut_fd_handler_t handler, FAR void *arg)
{
  FAR struct pnut_watch_s *watch = NULL;
  struct epoll_event ev;
  uint16_t i;

  if (fd < 0 || handler == NULL)
    {
      return -EINVAL;
    }

  if (loop_find(loop, fd) != NULL)
    {
      return -EEXIST;
    }

  for (i = 0; i < loop->nwatches; i++)
    {
      if (loop->watches[i].fd < 0 && !loop->watches[i].dead)
        {
          watch = &loop->watches[i];
          break;
        }
    }

  if (watch == NULL)
    {
      return -EBUSY;
    }

  memset(&ev, 0, sizeof(ev));
  ev.events   = events;
  ev.data.ptr = watch;

  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
      return -errno;
    }

  watch->fd      = fd;
  watch->handler = handler;
  watch->arg     = arg;
  return OK;
}

int pnut_loop_rewatch(FAR struct pnut_loop_s *loop, int fd,
                      uint32_t events)
{
  FAR struct pnut_watch_s *watch = loop_find(loop, fd);
  struct epoll_event ev;

  if (fd < 0 || watch == NULL)
    {
      return -ENOENT;
    }

  if (loop_ownfd(loop, fd))
    {
      return -EINVAL;
    }

  memset(&ev, 0, sizeof(ev));
  ev.events   = events;
  ev.data.ptr = watch;

  if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
      return -errno;
    }

  return OK;
}

int pnut_loop_unwatch(FAR struct pnut_loop_s *loop, int fd)
{
  FAR struct pnut_watch_s *watch = loop_find(loop, fd);
  int ret = OK;

  if (fd < 0 || watch == NULL)
    {
      return -ENOENT;
    }

  if (loop_ownfd(loop, fd))
    {
      return -EINVAL;
    }

  if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
    {
      ret = -errno;
    }

  watch->fd      = -1;
  watch->handler = NULL;
  watch->arg     = NULL;
  watch->dead    = loop->dispatching;
  return ret;
}

FAR const char *pnut_loop_name(FAR struct pnut_loop_s *loop)
{
  return loop->config.name;
}

/****************************************************************************
 * Name: pnut_loop_budget
 *
 * Description:
 *   Log a handler that ran longer than its budget (RFC 0007).
 *
 ****************************************************************************/

void pnut_loop_budget(FAR struct pnut_loop_s *loop, uint64_t start,
                      FAR const char *what)
{
  uint64_t elapsed = pnut_now() - start;

  if (loop->config.budget > 0 && elapsed > loop->config.budget)
    {
      pnut_loop_log(loop, PNUT_LOG_WARNING,
                    "A %s handler ran %lu ms; its budget is %lu ms",
                    what, (unsigned long)elapsed,
                    (unsigned long)loop->config.budget);
    }
}
