/****************************************************************************
 * pnut-os/tests/unit/test_worker.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/pool.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct job_s
{
  pthread_t loop_thread;
  pthread_t ran_on;
  pthread_t done_on;
  sem_t release;
  int runs;
  int dones;
  int expect;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void job_run(FAR void *arg)
{
  FAR struct job_s *job = arg;

  job->ran_on = pthread_self();
  job->runs++;
}

static void job_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct job_s *job = arg;

  job->done_on = pthread_self();
  if (++job->dones == job->expect)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void test_worker_runs_away_and_comes_back(void **state)
{
  FAR struct pnut_loop_s *loop;
  struct job_s job;

  memset(&job, 0, sizeof(job));
  job.loop_thread = pthread_self();
  job.expect = 1;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(pnut_job_submit(loop, job_run, job_done, &job), 0);
  assert_int_equal(pnut_loop_run(loop), 0);

  assert_int_equal(job.runs, 1);
  assert_int_equal(job.dones, 1);
  assert_false(pthread_equal(job.ran_on, job.loop_thread));
  assert_true(pthread_equal(job.done_on, job.loop_thread));
  assert_int_equal(loop->jobs.avail, loop->config.jobs);
  pnut_loop_destroy(loop);
}

static void job_blocked(FAR void *arg)
{
  FAR struct job_s *job = arg;

  sem_wait(&job->release);
  job->runs++;
}

static void test_worker_queue_full(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  struct job_s job;

  memset(&job, 0, sizeof(job));
  sem_init(&job.release, 0, 0);
  job.expect = 2;

  pnut_loop_defaults(&config);
  config.jobs = 2;
  config.workers = 1;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);

  assert_int_equal(pnut_job_submit(loop, job_blocked, job_done, &job), 0);
  assert_int_equal(pnut_job_submit(loop, job_blocked, job_done, &job), 0);
  assert_int_equal(pnut_job_submit(loop, job_blocked, job_done, &job),
                   -EBUSY);

  sem_post(&job.release);
  sem_post(&job.release);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(job.runs, 2);
  assert_int_equal(job.dones, 2);

  pnut_loop_destroy(loop);
  sem_destroy(&job.release);
}

static void chain_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct job_s *job = arg;

  /* The queue holds one job, this one; it is freed before this runs */

  if (++job->dones < job->expect)
    {
      assert_int_equal(pnut_job_submit(loop, job_run, chain_done, job), 0);
    }
  else
    {
      pnut_loop_stop(loop, 0);
    }
}

static void test_worker_done_queues_next(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  struct job_s job;

  memset(&job, 0, sizeof(job));
  job.expect = 3;

  pnut_loop_defaults(&config);
  config.jobs = 1;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);
  assert_int_equal(pnut_job_submit(loop, job_run, chain_done, &job), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  assert_int_equal(job.runs, 3);
  pnut_loop_destroy(loop);
}

static void test_worker_none(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  struct job_s job;

  pnut_loop_defaults(&config);
  config.workers = 0;
  assert_int_equal(pnut_loop_create(&config, &loop), 0);
  assert_int_equal(pnut_job_submit(loop, job_run, job_done, &job), -ENOSYS);
  pnut_loop_destroy(loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_worker_runs_away_and_comes_back),
    cmocka_unit_test(test_worker_queue_full),
    cmocka_unit_test(test_worker_done_queues_next),
    cmocka_unit_test(test_worker_none),
  };

  return cmocka_run_group_tests_name("worker", tests, NULL, NULL);
}
