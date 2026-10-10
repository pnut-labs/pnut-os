/****************************************************************************
 * pnut-os/tests/unit/test_states.c
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
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "programs.h"
#include "states.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FAKE_CLIENTS  4
#define FAKE_SERVICES 8
#define CHECK_MS      10
#define GIVEUP_MS     3000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A stand-in for NxInit's control socket, on a thread of its own: the
 * module asks it "who" while the loop waits
 */

struct fake_service_s
{
  char name[32];
  char state[16];
  int pid;
};

struct fake_s
{
  pthread_t thread;
  pthread_mutex_t lock;
  int listener;
  int wake[2];
  int clients[FAKE_CLIENTS];
  char in[FAKE_CLIENTS][80];
  size_t len[FAKE_CLIENTS];
  bool watching[FAKE_CLIENTS];
  struct fake_service_s table[FAKE_SERVICES];
  int ntable;
  int asks;                             /* "who" commands */
  char last[64];                        /* The last start or stop */
  bool silent;                          /* Leaves "who" unanswered */
  bool broken;                          /* A write failed */
};

struct fixture_s
{
  char dir[64];
  char path[96];
  char programs[96];
  struct fake_s fake;
  FAR struct pnut_loop_s *loop;
  struct states_s states;
  uint32_t elapsed;
  CODE bool (*until)(FAR struct fixture_s *f);
  CODE void (*then)(FAR struct fixture_s *f);
  bool done;

  /* What the steps saw, kept before the loop stops and the module frees
   * its table
   */

  bool ready;
  uint8_t nservices;
  struct states_service_s services[4];
  uint32_t seq;
  bool runs[2];
  char who[6][32];
  int rets[6];
  int asks[6];
  bool answered;
  FAR struct call_s *call;
  CODE void (*after)(FAR struct fixture_s *f);
  struct pnut_services_client_s client;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The fake NxInit */

/* On the fake's thread, where cmocka cannot fail a test: a write that
 * fails is noted, and the test checks the note
 */

static void fake_send(FAR struct fake_s *fake, int fd,
                      FAR const char *line)
{
  if (fd >= 0 && write(fd, line, strlen(line)) != (ssize_t)strlen(line))
    {
      fake->broken = true;
    }
}

static FAR struct fake_service_s *fake_find(FAR struct fake_s *fake,
                                            FAR const char *name)
{
  int i;

  for (i = 0; i < fake->ntable; i++)
    {
      if (strcmp(fake->table[i].name, name) == 0)
        {
          return &fake->table[i];
        }
    }

  return NULL;
}

static void fake_set(FAR struct fake_s *fake, FAR const char *name,
                     FAR const char *state, int pid, bool event)
{
  FAR struct fake_service_s *service;
  char line[80];
  int i;

  pthread_mutex_lock(&fake->lock);

  service = fake_find(fake, name);
  if (service == NULL)
    {
      assert_true(fake->ntable < FAKE_SERVICES);
      service = &fake->table[fake->ntable++];
      strlcpy(service->name, name, sizeof(service->name));
    }

  strlcpy(service->state, state, sizeof(service->state));
  service->pid = pid;

  if (event)
    {
      snprintf(line, sizeof(line), "event %s %s %d\n", name, state, pid);
      for (i = 0; i < FAKE_CLIENTS; i++)
        {
          if (fake->watching[i])
            {
              fake_send(fake, fake->clients[i], line);
            }
        }
    }

  pthread_mutex_unlock(&fake->lock);
}

static void fake_line(FAR struct fake_s *fake, int i, FAR char *line)
{
  FAR struct fake_service_s *service;
  char out[80];
  int fd = fake->clients[i];
  int pid;
  int j;

  if (strcmp(line, "watch") == 0)
    {
      fake->watching[i] = true;
      fake_send(fake, fd, "ok\n");
    }
  else if (strcmp(line, "state") == 0)
    {
      snprintf(out, sizeof(out), "ok %d\n", fake->ntable);
      fake_send(fake, fd, out);
      for (j = 0; j < fake->ntable; j++)
        {
          snprintf(out, sizeof(out), "%s %s %d\n", fake->table[j].name,
                   fake->table[j].state, fake->table[j].pid);
          fake_send(fake, fd, out);
        }
    }
  else if (sscanf(line, "who %d", &pid) == 1)
    {
      fake->asks++;
      if (fake->silent)
        {
          return;
        }

      for (j = 0; j < fake->ntable; j++)
        {
          if (fake->table[j].pid == pid && pid != 0)
            {
              break;
            }
        }

      if (j < fake->ntable)
        {
          snprintf(out, sizeof(out), "ok %s\n", fake->table[j].name);
          fake_send(fake, fd, out);
        }
      else
        {
          fake_send(fake, fd, "error 3 no such task\n");
        }
    }
  else if (strncmp(line, "start ", 6) == 0 || strncmp(line, "stop ", 5) == 0)
    {
      strlcpy(fake->last, line, sizeof(fake->last));
      service = fake_find(fake, strchr(line, ' ') + 1);
      fake_send(fake, fd, service != NULL ? "ok\n" :
                                            "error 2 no such service\n");
    }
  else
    {
      fake_send(fake, fd, "error 22 unknown command\n");
    }
}

static void fake_close(FAR struct fake_s *fake, int i)
{
  close(fake->clients[i]);
  fake->clients[i]  = -1;
  fake->watching[i] = false;
  fake->len[i]      = 0;
}

static FAR void *fake_main(FAR void *arg)
{
  FAR struct fake_s *fake = arg;
  struct pollfd pfds[2 + FAKE_CLIENTS];
  FAR char *nl;
  ssize_t n;
  int fd;
  int i;

  for (; ; )
    {
      pfds[0].fd     = fake->wake[0];
      pfds[0].events = POLLIN;
      pfds[1].fd     = fake->listener;
      pfds[1].events = POLLIN;
      for (i = 0; i < FAKE_CLIENTS; i++)
        {
          pfds[2 + i].fd     = fake->clients[i];
          pfds[2 + i].events = POLLIN;
        }

      if (poll(pfds, 2 + FAKE_CLIENTS, -1) < 0)
        {
          continue;
        }

      if (pfds[0].revents != 0)
        {
          return NULL;
        }

      pthread_mutex_lock(&fake->lock);

      if (pfds[1].revents & POLLIN)
        {
          fd = accept(fake->listener, NULL, NULL);
          for (i = 0; fd >= 0 && i < FAKE_CLIENTS; i++)
            {
              if (fake->clients[i] < 0)
                {
                  fake->clients[i] = fd;
                  fake_send(fake, fd, "nxinit 1\n");
                  fd = -1;
                }
            }

          if (fd >= 0)
            {
              fake_send(fake, fd, "error 16 too many clients\n");
              close(fd);
            }
        }

      for (i = 0; i < FAKE_CLIENTS; i++)
        {
          if (fake->clients[i] < 0 || pfds[2 + i].revents == 0)
            {
              continue;
            }

          n = read(fake->clients[i], fake->in[i] + fake->len[i],
                   sizeof(fake->in[i]) - fake->len[i]);
          if (n <= 0)
            {
              fake_close(fake, i);
              continue;
            }

          fake->len[i] += n;
          while ((nl = memchr(fake->in[i], '\n', fake->len[i])) != NULL)
            {
              *nl = '\0';
              fake_line(fake, i, fake->in[i]);
              fake->len[i] -= nl + 1 - fake->in[i];
              memmove(fake->in[i], nl + 1, fake->len[i]);
            }
        }

      pthread_mutex_unlock(&fake->lock);
    }
}

static void fake_start(FAR struct fake_s *fake, FAR const char *path)
{
  struct sockaddr_un addr;
  int i;

  fake->listener = socket(AF_UNIX, SOCK_STREAM, 0);
  assert_true(fake->listener >= 0);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  unlink(path);
  assert_int_equal(bind(fake->listener, (FAR struct sockaddr *)&addr,
                        sizeof(addr)), 0);
  assert_int_equal(listen(fake->listener, 4), 0);
  assert_int_equal(pipe(fake->wake), 0);

  for (i = 0; i < FAKE_CLIENTS; i++)
    {
      fake->clients[i] = -1;
    }

  pthread_mutex_init(&fake->lock, NULL);
  assert_int_equal(pthread_create(&fake->thread, NULL, fake_main, fake),
                   0);
}

static void fake_stop(FAR struct fake_s *fake)
{
  int i;

  assert_int_equal(write(fake->wake[1], "q", 1), 1);
  pthread_join(fake->thread, NULL);

  for (i = 0; i < FAKE_CLIENTS; i++)
    {
      if (fake->clients[i] >= 0)
        {
          close(fake->clients[i]);
        }
    }

  close(fake->listener);
  close(fake->wake[0]);
  close(fake->wake[1]);
  pthread_mutex_destroy(&fake->lock);
}

static int fake_asks(FAR struct fake_s *fake)
{
  int asks;

  pthread_mutex_lock(&fake->lock);
  asks = fake->asks;
  pthread_mutex_unlock(&fake->lock);
  return asks;
}

/* The program: a loop with the module, run step by step */

static void check(FAR struct pnut_loop_s *loop,
                  FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->elapsed += CHECK_MS;
  if (f->elapsed >= GIVEUP_MS)
    {
      pnut_loop_stop(loop, -ETIMEDOUT);
      return;
    }

  if (f->until != NULL && f->until(f))
    {
      f->until = NULL;
      if (f->then != NULL)
        {
          f->then(f);
        }
      else
        {
          f->ready     = f->states.module.ready;
          f->nservices = f->states.nservices;
          f->seq       = f->states.seq;
          memcpy(f->services, f->states.services,
                 (f->nservices < 4 ? f->nservices : 4) *
                 sizeof(f->services[0]));
          f->done = true;
          pnut_loop_stop(loop, 0);
        }
    }
}

static void fixture_setup(FAR struct fixture_s *f, FAR const char *table)
{
  struct pnut_loop_config_s config;
  struct states_config_s sconfig;
  int fd;

  memset(f, 0, sizeof(*f));

  /* On NuttX the directory is only a prefix of the sockets' names */

  snprintf(f->dir, sizeof(f->dir), "/tmp/pnut-states-%d", getpid());
  mkdir(f->dir, 0700);
  snprintf(f->path, sizeof(f->path), "%s/nxinit", f->dir);
  snprintf(f->programs, sizeof(f->programs), "/tmp/pnut-programs-%d",
           getpid());

  fd = open(f->programs, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert_true(fd >= 0);
  assert_int_equal(write(fd, table, strlen(table)), strlen(table));
  close(fd);

  fake_start(&f->fake, f->path);

  pnut_loop_defaults(&config);
  config.rundir  = f->dir;
  config.initctl = NULL;
  assert_int_equal(pnut_loop_create(&config, &f->loop), 0);

  sconfig.initctl  = f->path;
  sconfig.programs = f->programs;
  sconfig.services = 8;
  states_init(&f->states, &sconfig);
  assert_int_equal(pnut_module_add(f->loop, &f->states.module), 0);
}

static void fixture_run(FAR struct fixture_s *f)
{
  assert_int_equal(pnut_timer_start(f->loop, CHECK_MS, CHECK_MS, check, f,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_true(f->done);
}

static void fixture_teardown(FAR struct fixture_s *f)
{
  pnut_loop_destroy(f->loop);
  states_deinit(&f->states);
  fake_stop(&f->fake);
  unlink(f->path);
  unlink(f->programs);
  rmdir(f->dir);
}

static bool listed(FAR struct fixture_s *f)
{
  return f->states.listed;
}

static FAR struct states_service_s *service(FAR struct fixture_s *f,
                                            FAR const char *name)
{
  FAR struct states_service_s *services = f->done ? f->services :
                                          f->states.services;
  uint8_t n = f->done ? f->nservices : f->states.nservices;
  uint8_t i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(services[i].name, name) == 0)
        {
          return &services[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: test_states_programs
 *
 * Description:
 *   The programs' table: comments and blank lines, which program runs
 *   which service, and the lines it refuses.
 ****************************************************************************/

static void test_states_programs(void **state)
{
  static const char good[] =
    "# program  services\n"
    "\n"
    "system   states settings   # the first\n"
    "ui\tui\n"
    "runtime\n";
  struct programs_s programs;
  char big[PROGRAMS_TEXT_MAX + 1];
  int line = 0;
  int i;

  assert_int_equal(programs_parse(&programs, good, strlen(good), &line), 0);
  assert_int_equal(programs.nprograms, 3);
  assert_true(programs_runs(&programs, "system", "settings"));
  assert_true(programs_runs(&programs, "system", "states"));
  assert_true(programs_runs(&programs, "ui", "ui"));
  assert_false(programs_runs(&programs, "ui", "settings"));
  assert_false(programs_runs(&programs, "runtime", "runtime"));
  assert_false(programs_runs(&programs, "telephony", "telephony"));

  assert_int_equal(programs_parse(&programs, "a b\nA b\n", 8, &line),
                   -EINVAL);
  assert_int_equal(line, 2);
  assert_int_equal(programs.nprograms, 0);

  assert_int_equal(programs_parse(&programs, "a b\nc d\na e\n", 12,
                                  &line), -EINVAL);
  assert_int_equal(line, 3);

  memset(big, '\n', sizeof(big));
  assert_int_equal(programs_parse(&programs, big, sizeof(big), &line),
                   -E2BIG);

  for (i = 0; i <= PROGRAMS_MAX; i++)
    {
      snprintf(big + i * 4, 5, "p%02d\n", i);
    }

  assert_int_equal(programs_parse(&programs, big, (PROGRAMS_MAX + 1) * 4,
                                  &line), -E2BIG);
  assert_int_equal(line, PROGRAMS_MAX + 1);

  assert_int_equal(programs_load(&programs, "/nonexistent/programs", &line),
                   -ENOENT);
}

/****************************************************************************
 * Name: test_states_watch
 *
 * Description:
 *   The module lists NxInit's services, in the order of their names, is
 *   ready once it has, then follows their changes.
 ****************************************************************************/

static bool late_seen(FAR struct fixture_s *f)
{
  FAR struct states_service_s *late = service(f, "late");

  return late != NULL &&
         late->state == PNUT_SERVICE_STATE_STATE_STARTING;
}

static void send_events(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "system", "ready", 8, true);
  fake_set(&f->fake, "late", "starting", 12, true);
  f->until = late_seen;
  f->then  = NULL;
}

static void test_states_watch(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "system states settings\n");
  fake_set(&f.fake, "system", "starting", 8, false);
  fake_set(&f.fake, "console", "ready", 7, false);

  f.until = listed;
  f.then  = send_events;
  fixture_run(&f);

  assert_true(f.ready);
  assert_int_equal(f.nservices, 3);
  assert_string_equal(f.services[0].name, "console");
  assert_string_equal(f.services[1].name, "late");
  assert_string_equal(f.services[2].name, "system");
  assert_int_equal(service(&f, "console")->pid, 7);
  assert_int_equal(service(&f, "system")->state,
                   PNUT_SERVICE_STATE_STATE_READY);
  assert_int_equal(service(&f, "late")->pid, 12);

  /* The list itself announces nothing; the two events do */

  assert_int_equal(f.seq, 2);
  assert_true(states_runs(&f.states, "system", "settings"));
  assert_false(states_runs(&f.states, "console", "settings"));

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who
 *
 * Description:
 *   A task the watch has shown is known at once; one it has not is asked
 *   of NxInit; one NxInit does not know is remembered as a stranger.
 ****************************************************************************/

static void ask_who(FAR struct fixture_s *f)
{
  static const pid_t pids[6] =
    {
      7, 12, 12, 99, 99, 0
    };

  FAR const char *name;
  int i;

  /* "late" is started without an event yet, as a program just spawned */

  fake_set(&f->fake, "late", "starting", 12, false);

  for (i = 0; i < 6; i++)
    {
      f->rets[i] = states_who(&f->states, pids[i], &name);
      strlcpy(f->who[i], name != NULL ? name : "(none)",
              sizeof(f->who[i]));
      f->asks[i] = fake_asks(&f->fake);
    }

  f->done = true;
  pnut_loop_stop(f->loop, 0);
}

static void test_states_who(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);

  f.until = listed;
  f.then  = ask_who;
  fixture_run(&f);

  assert_string_equal(f.who[0], "console");
  assert_int_equal(f.asks[0], 0);
  assert_string_equal(f.who[1], "late");
  assert_int_equal(f.asks[1], 1);
  assert_string_equal(f.who[2], "late");
  assert_int_equal(f.asks[2], 1);
  assert_string_equal(f.who[3], "(none)");
  assert_int_equal(f.rets[3], -ESRCH);
  assert_int_equal(f.asks[3], 2);
  assert_int_equal(f.rets[4], -ESRCH);
  assert_int_equal(f.asks[4], 2);
  assert_int_equal(f.rets[5], -ESRCH);
  assert_int_equal(f.asks[5], 2);
  assert_false(f.fake.broken);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_unanswered
 *
 * Description:
 *   When NxInit does not answer "who", the task cannot be told now, which
 *   is not the same as no service's; NxInit is not asked again at once.
 ****************************************************************************/

static void ask_unanswered(FAR struct fixture_s *f)
{
  FAR const char *name;

  pthread_mutex_lock(&f->fake.lock);
  f->fake.silent = true;
  pthread_mutex_unlock(&f->fake.lock);

  f->rets[0] = states_who(&f->states, 12, &name);
  f->asks[0] = fake_asks(&f->fake);
  f->rets[1] = states_who(&f->states, 12, &name);
  f->asks[1] = fake_asks(&f->fake);

  f->done = true;
  pnut_loop_stop(f->loop, 0);
}

static void test_states_who_unanswered(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);

  f.until = listed;
  f.then  = ask_unanswered;
  fixture_run(&f);

  assert_int_equal(f.rets[0], -EAGAIN);
  assert_int_equal(f.asks[0], 1);
  assert_int_equal(f.rets[1], -EAGAIN);
  assert_int_equal(f.asks[1], 1);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_calls
 *
 * Description:
 *   pnut.Services through a client.  The system UI and the system program
 *   start and stop services, answered with NxInit's answer; a service
 *   NxInit does not have is not found; another program may not.  Anyone
 *   asks who a task is.
 ****************************************************************************/

enum call_e
{
  CALL_START,
  CALL_STOP,
  CALL_WHO,
};

struct call_s
{
  enum call_e kind;
  FAR const char *name;           /* The service, or for Who the program */
  pid_t pid;                      /* Who's; 0 for the caller's own */
  int status;
  uint32_t code;
  char answer[32];                /* Who's */
};

static bool answered(FAR struct fixture_s *f)
{
  return f->answered;
}

static void call_record(FAR struct fixture_s *f, int status,
                        FAR const pnut_error_t *err)
{
  FAR struct call_s *call = f->call;

  call->status = status;
  call->code   = err != NULL ? err->code : 0;
  f->answered  = true;
}

static void call_replied(FAR struct pnut_services_client_s *client,
                         int status, FAR const pnut_services_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  call_record(arg, status, err);
}

static void call_who_replied(FAR struct pnut_services_client_s *client,
                             int status,
                             FAR const pnut_services_who_reply_t *out,
                             FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (out != NULL)
    {
      strlcpy(f->call->answer, out->name, sizeof(f->call->answer));
    }

  call_record(f, status, err);
}

static void call_state(FAR struct pnut_services_client_s *client,
                       bool up, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  pnut_services_name_request_t in;
  pnut_services_who_request_t who;

  if (!up)
    {
      return;
    }

  memset(&in, 0, sizeof(in));
  if (f->call->name != NULL)
    {
      strlcpy(in.name, f->call->name, sizeof(in.name));
    }

  switch (f->call->kind)
    {
      case CALL_START:
        assert_int_equal(pnut_services_start(client, &in, 2000,
                                             call_replied, f), 0);
        break;

      case CALL_STOP:
        assert_int_equal(pnut_services_stop(client, &in, 2000,
                                            call_replied, f), 0);
        break;

      default:
        memset(&who, 0, sizeof(who));
        who.pid = f->call->pid != 0 ? f->call->pid : getpid();
        assert_int_equal(pnut_services_who(client, &who, 2000,
                                           call_who_replied, f), 0);
        break;
    }
}

static void connect_client(FAR struct fixture_s *f)
{
  assert_int_equal(pnut_services_connect(f->loop, STATES_SERVICE,
                                         call_state, f, &f->client), 0);
  f->until = answered;
  f->then  = f->after;
}

/* Run one call as the program `me`, with a service "console" NxInit has,
 * and keep what NxInit was last asked to start or stop
 */

static void run_call(FAR const char *me, FAR struct call_s *call,
                     FAR char *last, size_t size)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  if (me != NULL)
    {
      fake_set(&f.fake, me, "ready", getpid(), false);
    }
  else
    {
      f.fake.silent = true;       /* Who calls cannot be told */
    }

  fake_set(&f.fake, "console", "ready", 7, false);

  f.call  = call;
  f.until = listed;
  f.then  = connect_client;
  fixture_run(&f);

  pthread_mutex_lock(&f.fake.lock);
  strlcpy(last, f.fake.last, size);
  pthread_mutex_unlock(&f.fake.lock);
  assert_false(f.fake.broken);

  pnut_services_disconnect(&f.client);
  fixture_teardown(&f);
}

static void test_states_calls(void **state)
{
  struct call_s call;
  char last[64];

  memset(&call, 0, sizeof(call));
  call.kind = CALL_START;
  call.name = "console";
  run_call("ui", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_OK);
  assert_string_equal(last, "start console");

  call.kind = CALL_STOP;
  run_call("system", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_OK);
  assert_string_equal(last, "stop console");

  call.kind = CALL_START;
  call.name = "nosuch";
  run_call("ui", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_NOTFOUND);
  assert_int_equal(call.code, PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE);
  assert_string_equal(last, "start nosuch");

  call.name = "console";
  run_call("telephony", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_DENIED);
  assert_string_equal(last, "");

  call.kind = CALL_START;
  run_call(NULL, &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_UNAVAILABLE);
  assert_string_equal(last, "");

  call.kind = CALL_WHO;
  call.pid  = 0;
  run_call("telephony", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_OK);
  assert_string_equal(call.answer, "telephony");
}

/****************************************************************************
 * Name: test_states_who_named
 *
 * Description:
 *   A pid another program names in Who is not remembered as a stranger:
 *   it may be one of a program about to start, which then is known.
 ****************************************************************************/

static void named_started(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 77, false);
  f->rets[0] = states_who(&f->states, 77, &name);
  strlcpy(f->who[0], name != NULL ? name : "(none)", sizeof(f->who[0]));

  f->done = true;
  pnut_loop_stop(f->loop, 0);
}

static void test_states_who_named(void **state)
{
  struct fixture_s f;
  struct call_s call;

  memset(&call, 0, sizeof(call));
  call.kind = CALL_WHO;
  call.pid  = 77;

  fixture_setup(&f, "");
  fake_set(&f.fake, "ui", "ready", getpid(), false);

  f.call  = &call;
  f.after = named_started;
  f.until = listed;
  f.then  = connect_client;
  fixture_run(&f);

  assert_int_equal(call.status, PNUT_STATUS_NOTFOUND);
  assert_int_equal(f.rets[0], OK);
  assert_string_equal(f.who[0], "late");

  pnut_services_disconnect(&f.client);
  fixture_teardown(&f);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_states_programs),
    cmocka_unit_test(test_states_watch),
    cmocka_unit_test(test_states_who),
    cmocka_unit_test(test_states_who_unanswered),
    cmocka_unit_test(test_states_calls),
    cmocka_unit_test(test_states_who_named),
  };

  return cmocka_run_group_tests_name("states", tests, NULL, NULL);
}
