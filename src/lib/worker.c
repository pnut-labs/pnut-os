/****************************************************************************
 * pnut-os/src/lib/worker.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: worker_main
 *
 * Description:
 *   A worker: run the jobs queued, and hand each back to the loop.
 *
 ****************************************************************************/

static FAR void *worker_main(FAR void *arg)
{
  FAR struct pnut_loop_s *loop = arg;
  FAR struct pnut_job_s *job;

  for (; ; )
    {
      pthread_mutex_lock(&loop->lock);
      while (loop->pending == NULL && !loop->quit)
        {
          pthread_cond_wait(&loop->cond, &loop->lock);
        }

      if (loop->quit)
        {
          pthread_mutex_unlock(&loop->lock);
          break;
        }

      job = loop->pending;
      loop->pending = job->next;
      if (loop->pending == NULL)
        {
          loop->pending_last = NULL;
        }

      pthread_mutex_unlock(&loop->lock);

      job->run(job->arg);

      pthread_mutex_lock(&loop->lock);
      job->next = NULL;
      if (loop->done_last != NULL)
        {
          loop->done_last->next = job;
        }
      else
        {
          loop->done = job;
        }

      loop->done_last = job;
      pthread_mutex_unlock(&loop->lock);

      eventfd_write(loop->eventfd, 1);
    }

  return NULL;
}

/****************************************************************************
 * Name: worker_done
 *
 * Description:
 *   Jobs have run: call their done handlers on the loop.
 *
 ****************************************************************************/

static void worker_done(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct pnut_job_s *job;
  FAR struct pnut_job_s *next;
  pnut_job_done_t done;
  FAR void *jobarg;
  eventfd_t value;
  uint64_t start;

  eventfd_read(fd, &value);

  pthread_mutex_lock(&loop->lock);
  job = loop->done;
  loop->done = NULL;
  loop->done_last = NULL;
  pthread_mutex_unlock(&loop->lock);

  for (; job != NULL; job = next)
    {
      /* Free the job first, so that its done handler can queue another */

      next   = job->next;
      done   = job->done;
      jobarg = job->arg;
      pnut_pool_free(&loop->jobs, job);

      if (done != NULL)
        {
          start = pnut_now();
          done(loop, jobarg);
          pnut_loop_budget(loop, start, "job's done");
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_worker_init(FAR struct pnut_loop_s *loop)
{
  pthread_attr_t attr;
  size_t stacksize;
  uint8_t i;
  int ret;

  ret = pnut_pool_init(&loop->jobs, sizeof(struct pnut_job_s),
                       loop->config.jobs);
  if (ret < 0)
    {
      return ret;
    }

  loop->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (loop->eventfd < 0)
    {
      return -errno;
    }

  ret = pnut_loop_watch(loop, loop->eventfd, EPOLLIN, worker_done, NULL);
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_init(&loop->lock, NULL);
  pthread_cond_init(&loop->cond, NULL);
  loop->sync = true;

  if (loop->config.workers == 0)
    {
      return OK;
    }

  loop->threads = calloc(loop->config.workers, sizeof(pthread_t));
  if (loop->threads == NULL)
    {
      return -ENOMEM;
    }

  stacksize = loop->config.stacksize;
#ifdef PTHREAD_STACK_MIN
  if (stacksize < PTHREAD_STACK_MIN)
    {
      stacksize = PTHREAD_STACK_MIN;
    }
#endif

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stacksize);

  for (i = 0; i < loop->config.workers; i++)
    {
      ret = pthread_create(&loop->threads[i], &attr, worker_main, loop);
      if (ret != 0)
        {
          pthread_attr_destroy(&attr);
          return -ret;
        }

      loop->nthreads++;
    }

  pthread_attr_destroy(&attr);
  return OK;
}

void pnut_worker_deinit(FAR struct pnut_loop_s *loop)
{
  uint8_t i;

  if (loop->sync)
    {
      pthread_mutex_lock(&loop->lock);
      loop->quit = true;
      pthread_cond_broadcast(&loop->cond);
      pthread_mutex_unlock(&loop->lock);

      for (i = 0; i < loop->nthreads; i++)
        {
          pthread_join(loop->threads[i], NULL);
        }

      pthread_cond_destroy(&loop->cond);
      pthread_mutex_destroy(&loop->lock);
      loop->sync = false;
    }

  free(loop->threads);
  loop->threads = NULL;
  loop->nthreads = 0;

  if (loop->eventfd >= 0)
    {
      close(loop->eventfd);
      loop->eventfd = -1;
    }

  pnut_pool_deinit(&loop->jobs);
}

int pnut_job_submit(FAR struct pnut_loop_s *loop, pnut_job_run_t run,
                    pnut_job_done_t done, FAR void *arg)
{
  FAR struct pnut_job_s *job;

  if (run == NULL)
    {
      return -EINVAL;
    }

  if (loop->nthreads == 0)
    {
      return -ENOSYS;
    }

  job = pnut_pool_alloc(&loop->jobs);
  if (job == NULL)
    {
      return -EBUSY;
    }

  job->run  = run;
  job->done = done;
  job->arg  = arg;

  pthread_mutex_lock(&loop->lock);
  if (loop->pending_last != NULL)
    {
      loop->pending_last->next = job;
    }
  else
    {
      loop->pending = job;
    }

  loop->pending_last = job;
  pthread_cond_signal(&loop->cond);
  pthread_mutex_unlock(&loop->lock);
  return OK;
}
