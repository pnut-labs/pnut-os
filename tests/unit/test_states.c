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
#include <time.h>
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

/* Two services of NxInit's longest name, 63 bytes: reverse-DNS ids */

#define LONG_LISTED "org.example.applications." \
                    "a-long-reverse-dns-id.listed-from-boot"
#define LONG_LATE   "org.example.applications." \
                    "a-long-reverse-dns-id.started-as-later"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A stand-in for NxInit's control socket, on a thread of its own: the
 * module asks it "who" while the loop waits
 */

struct fake_service_s
{
  char name[64];
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
  char in[FAKE_CLIENTS][128];
  size_t len[FAKE_CLIENTS];
  bool watching[FAKE_CLIENTS];
  struct fake_service_s table[FAKE_SERVICES];
  int ntable;
  int asks;                             /* "who" commands */
  char last[128];                       /* The last start or stop */
  char stops[128];                      /* Every stop, ';' after each */
  bool silent;                          /* Leaves "who" unanswered */
  bool broken;                          /* A write failed */
  bool hold;                            /* Leaves "state" unanswered */
  int held;                             /* The client it left, or -1 */
  int accepts;                          /* Clients taken */
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
  char who[8][64];
  int rets[8];
  int asks[8];
  bool answered;
  FAR struct call_s *call;
  CODE void (*after)(FAR struct fixture_s *f);
  struct pnut_services_client_s client;
  int readies;                    /* config.ready told */
  uint32_t mark;                  /* now_ms(), at a step */
  int mark2;

  /* The services topic, for a test that reads it: the first messages,
   * and how many came
   */

  struct pnut_service_change_reader_s reader;
  bool reading;
  pnut_service_change_t seen[8];
  int nseen;
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
  char line[128];
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

static void fake_list(FAR struct fake_s *fake, int fd)
{
  char out[128];
  int j;

  snprintf(out, sizeof(out), "ok %d\n", fake->ntable);
  fake_send(fake, fd, out);
  for (j = 0; j < fake->ntable; j++)
    {
      snprintf(out, sizeof(out), "%s %s %d\n", fake->table[j].name,
               fake->table[j].state, fake->table[j].pid);
      fake_send(fake, fd, out);
    }
}

static void fake_line(FAR struct fake_s *fake, int i, FAR char *line)
{
  FAR struct fake_service_s *service;
  char out[128];
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
      if (fake->hold)
        {
          fake->held = i;
          return;
        }

      fake_list(fake, fd);
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
      if (strncmp(line, "stop ", 5) == 0)
        {
          strlcat(fake->stops, line, sizeof(fake->stops));
          strlcat(fake->stops, ";", sizeof(fake->stops));
        }

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
  if (fake->held == i)
    {
      fake->held = -1;
    }

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
  char c;
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

      /* 'q' ends the thread; 'k' lets every client go, as an NxInit
       * that restarts would
       */

      if (pfds[0].revents != 0)
        {
          if (read(fake->wake[0], &c, 1) != 1 || c != 'k')
            {
              return NULL;
            }

          pthread_mutex_lock(&fake->lock);
          for (i = 0; i < FAKE_CLIENTS; i++)
            {
              if (fake->clients[i] >= 0)
                {
                  fake_close(fake, i);
                }
            }

          pthread_mutex_unlock(&fake->lock);
          continue;
        }

      pthread_mutex_lock(&fake->lock);

      if (pfds[1].revents & POLLIN)
        {
          fd = accept(fake->listener, NULL, NULL);
          if (fd >= 0)
            {
              fake->accepts++;
            }

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

  fake->held = -1;
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

/* Each task's parent, for states_origin(): a command typed in the
 * console (30); one typed in a shell adb's daemon started (41, in 40); a
 * chain longer than the lookup goes (50 to 54); a task without a parent
 * (60); one whose parent cannot be told (70)
 */

static pid_t fake_parent(pid_t pid)
{
  static const pid_t parents[][2] =
    {
      { 30, 7 },
      { 41, 40 },
      { 40, 20 },
      { 50, 51 },
      { 51, 52 },
      { 52, 53 },
      { 53, 54 },
      { 54, 7 },
      { 60, 0 },
    };

  size_t i;

  for (i = 0; i < sizeof(parents) / sizeof(parents[0]); i++)
    {
      if (parents[i][0] == pid)
        {
          return parents[i][1];
        }
    }

  return -1;
}

/* The services topic's directory on the computer, with the latest message
 * in it, which a reader would get first in the next test
 */

static void topic_clean(FAR struct fixture_s *f)
{
  char path[128];

  snprintf(path, sizeof(path), "%s/topic.services/last", f->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.services", f->dir);
  rmdir(path);
}

static void fixture_setup(FAR struct fixture_s *f, FAR const char *table)
{
  struct pnut_loop_config_s config;
  struct states_config_s sconfig;
  int fd;

  memset(f, 0, sizeof(*f));
  memset(&sconfig, 0, sizeof(sconfig));

  /* On NuttX the directory is only a prefix of the sockets' names */

  snprintf(f->dir, sizeof(f->dir), "/tmp/pnut-states-%d", getpid());
  mkdir(f->dir, 0700);
  topic_clean(f);
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
  sconfig.parent   = fake_parent;
  sconfig.shells   = "console adbd";
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

static void topic_seen(FAR struct pnut_service_change_reader_s *reader,
                       FAR const pnut_service_change_t *msg, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (f->nseen < 8)
    {
      f->seen[f->nseen] = *msg;
    }

  f->nseen++;
}

static void fixture_read(FAR struct fixture_s *f)
{
  assert_int_equal(pnut_service_change_subscribe(f->loop, topic_seen, f,
                                                 &f->reader), 0);
  f->reading = true;
}

static void fixture_teardown(FAR struct fixture_s *f)
{
  if (f->reading)
    {
      pnut_service_change_unsubscribe(&f->reader);
    }

  pnut_loop_destroy(f->loop);
  states_deinit(&f->states);
  fake_stop(&f->fake);
  unlink(f->path);
  unlink(f->programs);
  topic_clean(f);
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
  char name[65];
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

  /* A program and its service named by a reverse-DNS id of 63 bytes,
   * NxInit's longest name; one byte more is refused
   */

  memset(name, 'a', sizeof(name));
  memcpy(name, "org.", 4);
  name[63] = '\0';
  snprintf(big, sizeof(big), "%s %s\n", name, name);
  assert_int_equal(programs_parse(&programs, big, strlen(big), &line), 0);
  assert_true(programs_runs(&programs, name, name));

  name[63] = 'a';
  name[64] = '\0';
  snprintf(big, sizeof(big), "%s %s\n", name, name);
  assert_int_equal(programs_parse(&programs, big, strlen(big), &line),
                   -EINVAL);
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

/****************************************************************************
 * Name: test_states_halt
 *
 * Description:
 *   The programs of the table that run, but the one named, are asked to
 *   stop; a stopped one, and a service that is no program (a shell), are
 *   not.  Told once they have stopped, or once the wait is over, with how
 *   many still run.
 *
 ****************************************************************************/

static void halted(FAR void *arg, int running)
{
  FAR struct fixture_s *f = arg;

  f->rets[0] = running;
  f->asks[0]++;
}

static bool told(FAR struct fixture_s *f)
{
  return f->asks[0] > 0;
}

static bool stops_sent(FAR struct fixture_s *f)
{
  bool sent;

  pthread_mutex_lock(&f->fake.lock);
  sent = strstr(f->fake.stops, "stop telephony;") != NULL &&
         strstr(f->fake.stops, "stop ui;") != NULL;
  pthread_mutex_unlock(&f->fake.lock);
  return sent;
}

static void mark_stopped(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "telephony", "stopped", 0, true);
  fake_set(&f->fake, "ui", "stopped", 0, true);
  f->until = told;
  f->then  = NULL;
}

static void halt_programs(FAR struct fixture_s *f)
{
  assert_int_equal(states_halt(&f->states, "system", 2000, halted, f), 0);
  assert_int_equal(states_halt(&f->states, "system", 2000, halted, f),
                   -EBUSY);
  f->until = stops_sent;
  f->then  = mark_stopped;
}

static void halt_briefly(FAR struct fixture_s *f)
{
  assert_int_equal(states_halt(&f->states, "system", 100, halted, f), 0);
  f->until = told;
  f->then  = NULL;
}

static void halt_setup(FAR struct fixture_s *f)
{
  fixture_setup(f, "system states\ntelephony telephony\nui ui\n"
                   "radios radio\n");
  fake_set(&f->fake, "system", "ready", 8, false);
  fake_set(&f->fake, "telephony", "ready", 20, false);
  fake_set(&f->fake, "ui", "ready", 21, false);
  fake_set(&f->fake, "radios", "stopped", 0, false);
  fake_set(&f->fake, "console", "ready", 7, false);
}

static void test_states_halt(void **state)
{
  struct fixture_s f;

  /* Asked, and stopped: told with none running */

  halt_setup(&f);
  f.until = listed;
  f.then  = halt_programs;
  fixture_run(&f);

  assert_int_equal(f.asks[0], 1);
  assert_int_equal(f.rets[0], 0);
  pthread_mutex_lock(&f.fake.lock);
  assert_string_equal(f.fake.stops, "stop telephony;stop ui;");
  pthread_mutex_unlock(&f.fake.lock);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* Asked, and not stopped in time: told with both running */

  halt_setup(&f);
  f.until = listed;
  f.then  = halt_briefly;
  fixture_run(&f);

  assert_int_equal(f.asks[0], 1);
  assert_int_equal(f.rets[0], 2);
  fixture_teardown(&f);

  /* None to stop but the shell, which is no program: told at once, on
   * the loop, NxInit asked nothing
   */

  fixture_setup(&f, "system states\ntelephony telephony\n");
  fake_set(&f.fake, "system", "ready", 8, false);
  fake_set(&f.fake, "telephony", "stopped", 0, false);
  fake_set(&f.fake, "console", "ready", 7, false);
  f.until = listed;
  f.then  = halt_briefly;
  fixture_run(&f);

  assert_int_equal(f.asks[0], 1);
  assert_int_equal(f.rets[0], 0);
  pthread_mutex_lock(&f.fake.lock);
  assert_string_equal(f.fake.stops, "");
  pthread_mutex_unlock(&f.fake.lock);
  fixture_teardown(&f);
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
 * Name: test_states_ready
 *
 * Description:
 *   Told once every program of the table that NxInit has has stayed ready
 *   for the hold: not while one is stopped (not started yet) or starting;
 *   a program that crashes soon after saying it is ready, and comes back
 *   within the hold, starts the wait again from its return; a program
 *   NxInit does not have (radios) and a service that is no program
 *   (console, starting) are passed over.  A failed tell is told again a
 *   hold later; a done one is not.
 ****************************************************************************/

#define READY_HOLD_MS 300

/* The steps' waits are measured on the timers' own clock: the fixture's
 * elapsed counts its ticks, which a busy machine (or the simulator) runs
 * late, and a wait counted in them would end after the hold's timer
 */

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int ready_told(FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->readies++;
  return f->readies == 1 ? -EIO : 0;
}

static bool telephony_is(FAR struct fixture_s *f, uint8_t state)
{
  FAR struct states_service_s *s = service(f, "telephony");

  return s != NULL && s->state == state;
}

static bool telephony_starting(FAR struct fixture_s *f)
{
  return telephony_is(f, PNUT_SERVICE_STATE_STATE_STARTING);
}

static bool telephony_ready(FAR struct fixture_s *f)
{
  return telephony_is(f, PNUT_SERVICE_STATE_STATE_READY);
}

/* A third of a hold since the crash, telephony still restarting */

static bool crashed_a_while(FAR struct fixture_s *f)
{
  return telephony_is(f, PNUT_SERVICE_STATE_STATE_RESTARTING) &&
         now_ms() - f->mark >= READY_HOLD_MS / 3;
}

/* Five sixths of a hold since telephony came back: a wait not started
 * again at its return, but at its first ready, would be over
 */

static bool back_a_while(FAR struct fixture_s *f)
{
  return telephony_ready(f) && now_ms() - f->mark >= 5 * READY_HOLD_MS / 6;
}

/* Told twice, then two holds more for any tell still due */

static bool told_twice(FAR struct fixture_s *f)
{
  if (f->readies >= 2 && f->mark2 == 0)
    {
      f->mark  = now_ms();
      f->mark2 = 1;
    }

  return f->mark2 != 0 && now_ms() - f->mark >= 2 * READY_HOLD_MS;
}

static void not_yet(FAR struct fixture_s *f)
{
  f->rets[2] = f->readies;
  f->until   = told_twice;
  f->then    = NULL;
}

static void telephony_back(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "telephony", "ready", 10, true);
  f->mark  = now_ms();
  f->until = back_a_while;
  f->then  = not_yet;
}

static void telephony_crash(FAR struct fixture_s *f)
{
  f->rets[1] = f->readies;
  fake_set(&f->fake, "telephony", "restarting", 0, true);
  f->mark  = now_ms();
  f->until = crashed_a_while;
  f->then  = telephony_back;
}

static void telephony_up(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "telephony", "ready", 9, true);
  f->until = telephony_ready;
  f->then  = telephony_crash;
}

static void telephony_start(FAR struct fixture_s *f)
{
  f->rets[0] = f->readies;
  fake_set(&f->fake, "telephony", "starting", 9, true);
  f->until = telephony_starting;
  f->then  = telephony_up;
}

static void test_states_ready(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "system states\ntelephony telephony\n"
                    "radios radio\n");
  f.states.config.ready      = ready_told;
  f.states.config.ready_arg  = &f;
  f.states.config.ready_hold = READY_HOLD_MS;

  fake_set(&f.fake, "system", "ready", 8, false);
  fake_set(&f.fake, "telephony", "stopped", 0, false);
  fake_set(&f.fake, "console", "starting", 7, false);

  f.until = listed;
  f.then  = telephony_start;
  fixture_run(&f);

  /* Nothing while telephony was stopped or starting, nor when it crashed
   * at once, nor before a whole hold after it came back; then told,
   * failed, told again a hold later, done
   */

  assert_int_equal(f.rets[0], 0);
  assert_int_equal(f.rets[1], 0);
  assert_int_equal(f.rets[2], 0);
  assert_int_equal(f.readies, 2);
  assert_int_equal(service(&f, "telephony")->state,
                   PNUT_SERVICE_STATE_STATE_READY);
  assert_int_equal(service(&f, "telephony")->pid, 10);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_ready_relisted
 *
 * Description:
 *   After a lost watch the states are old until NxInit has listed them
 *   again: an event before the list starts no hold, though the old states
 *   were all ready (telephony has crashed meanwhile); the list does.
 ****************************************************************************/

static int ready_counted(FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->readies++;
  return 0;
}

/* The fake has taken the module again, and it watches */

static bool rewatched(FAR struct fixture_s *f)
{
  bool watching = false;
  int i;

  pthread_mutex_lock(&f->fake.lock);
  for (i = 0; f->fake.accepts >= 2 && i < FAKE_CLIENTS; i++)
    {
      watching |= f->fake.watching[i];
    }

  pthread_mutex_unlock(&f->fake.lock);
  return watching;
}

static bool two_holds(FAR struct fixture_s *f)
{
  return now_ms() - f->mark >= 2 * READY_HOLD_MS;
}

static bool told_relisted(FAR struct fixture_s *f)
{
  return f->readies > 0 && two_holds(f);
}

static void relist(FAR struct fixture_s *f)
{
  FAR struct states_service_s *console = service(f, "console");

  /* The event came, before the list */

  assert_non_null(console);
  assert_int_equal(console->state, PNUT_SERVICE_STATE_STATE_READY);

  f->rets[1] = f->readies;
  fake_set(&f->fake, "telephony", "ready", 10, false);

  pthread_mutex_lock(&f->fake.lock);
  f->fake.hold = false;
  assert_true(f->fake.held >= 0);
  fake_list(&f->fake, f->fake.clients[f->fake.held]);
  pthread_mutex_unlock(&f->fake.lock);

  f->mark  = now_ms();
  f->until = told_relisted;
  f->then  = NULL;
}

static void event_unlisted(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "console", "ready", 7, true);
  f->mark  = now_ms();
  f->until = two_holds;
  f->then  = relist;
}

static void kick(FAR struct fixture_s *f)
{
  f->rets[0] = f->readies;
  fake_set(&f->fake, "telephony", "restarting", 0, false);

  pthread_mutex_lock(&f->fake.lock);
  f->fake.hold = true;
  pthread_mutex_unlock(&f->fake.lock);

  assert_int_equal(write(f->fake.wake[1], "k", 1), 1);
  f->until = rewatched;
  f->then  = event_unlisted;
}

static void test_states_ready_relisted(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "system states\ntelephony telephony\n");
  f.states.config.ready      = ready_counted;
  f.states.config.ready_arg  = &f;
  f.states.config.ready_hold = READY_HOLD_MS;

  fake_set(&f.fake, "system", "ready", 8, false);
  fake_set(&f.fake, "telephony", "ready", 9, false);

  f.until = listed;
  f.then  = kick;
  fixture_run(&f);

  /* Not told before the watch was lost, nor in the two holds without a
   * list; told once from the list
   */

  assert_int_equal(f.rets[0], 0);
  assert_int_equal(f.rets[1], 0);
  assert_int_equal(f.readies, 1);
  assert_int_equal(service(&f, "telephony")->pid, 10);
  assert_false(f.fake.broken);

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
  char answer[64];                /* Who's */
  bool shell;                     /* Who's: a shell's command */
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
      f->call->shell = out->shell;
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
  char last[128];

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
  assert_false(call.shell);

  /* A command typed in the console: no program's, the shell's */

  call.pid = 30;
  run_call("telephony", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_OK);
  assert_string_equal(call.answer, "");
  assert_true(call.shell);

  /* A task without a parent: no one's */

  call.pid   = 60;
  call.shell = false;
  run_call("telephony", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_NOTFOUND);
  assert_false(call.shell);

  /* The longest names: a program's, whole in Who's answer; a service's,
   * whole in NxInit's command
   */

  call.pid = 0;
  run_call(LONG_LISTED, &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_OK);
  assert_string_equal(call.answer, LONG_LISTED);

  call.kind = CALL_START;
  call.name = LONG_LATE;
  run_call("ui", &call, last, sizeof(last));
  assert_int_equal(call.status, PNUT_STATUS_NOTFOUND);
  assert_string_equal(last, "start " LONG_LATE);
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
 * Name: test_states_long_name
 *
 * Description:
 *   A service with the longest name NxInit takes, 63 bytes, a reverse-DNS
 *   id, is listed and named whole: from NxInit's list, and from its answer
 *   to "who" for a task it has not shown yet.
 ****************************************************************************/

static void ask_long(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, LONG_LATE, "starting", 12, false);

  f->rets[0] = states_who(&f->states, 7, &name);
  strlcpy(f->who[0], name != NULL ? name : "(none)", sizeof(f->who[0]));
  f->rets[1] = states_who(&f->states, 12, &name);
  strlcpy(f->who[1], name != NULL ? name : "(none)", sizeof(f->who[1]));

  f->done = true;
  pnut_loop_stop(f->loop, 0);
}

static void test_states_long_name(void **state)
{
  struct fixture_s f;

  assert_int_equal(strlen(LONG_LISTED), 63);
  assert_int_equal(strlen(LONG_LATE), 63);

  fixture_setup(&f, "");
  fake_set(&f.fake, LONG_LISTED, "ready", 7, false);

  f.until = listed;
  f.then  = ask_long;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_string_equal(f.who[0], LONG_LISTED);
  assert_int_equal(f.rets[1], OK);
  assert_string_equal(f.who[1], LONG_LATE);
  assert_false(f.fake.broken);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_origin
 *
 * Description:
 *   The program that started a task: the console for a command typed in
 *   it, adb's daemon for one typed in the shell it started; none past
 *   STATES_ORIGIN_DEPTH parents, for a task without a parent, or for one
 *   whose parent cannot be told.  NxInit is asked about a parent it has
 *   not shown once only.
 ****************************************************************************/

static void ask_origin(FAR struct fixture_s *f)
{
  static const pid_t pids[7] =
    {
      30, 41, 41, 50, 60, 70, 7
    };

  FAR const char *name;
  int i;

  for (i = 0; i < 7; i++)
    {
      f->rets[i] = states_origin(&f->states, pids[i], &name);
      strlcpy(f->who[i], name != NULL ? name : "(none)",
              sizeof(f->who[i]));
      f->asks[i] = fake_asks(&f->fake);
    }

  f->done = true;
  pnut_loop_stop(f->loop, 0);
}

static void test_states_origin(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "adbd", "ready", 20, false);

  f.until = listed;
  f.then  = ask_origin;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_string_equal(f.who[0], "console");
  assert_int_equal(f.asks[0], 0);

  /* adb's shell is asked of NxInit the first time, then remembered */

  assert_int_equal(f.rets[1], OK);
  assert_string_equal(f.who[1], "adbd");
  assert_int_equal(f.asks[1], 1);
  assert_int_equal(f.rets[2], OK);
  assert_string_equal(f.who[2], "adbd");
  assert_int_equal(f.asks[2], 1);

  assert_int_equal(f.rets[3], -ESRCH);
  assert_string_equal(f.who[3], "(none)");
  assert_int_equal(f.asks[3], 1 + STATES_ORIGIN_DEPTH);
  assert_int_equal(f.rets[4], -ESRCH);
  assert_int_equal(f.rets[5], -ESRCH);

  /* The console itself has no parent among the programs */

  assert_int_equal(f.rets[6], -ESRCH);
  assert_int_equal(f.asks[6], 1 + STATES_ORIGIN_DEPTH);
  assert_false(f.fake.broken);

  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_published
 *
 * Description:
 *   A task Who finds before the watch has shown it changes its service in
 *   the table at once, but the change is held (the owner's answer L28):
 *   it is published on the services topic when NxInit's own line about
 *   that service comes, though that line is the same, and once only.  The
 *   service starting (L23) is held so, and so is its new task with the
 *   state the watch showed last.  Before the first whole list nothing is
 *   published, as for the watch's lines, and the list's line about the
 *   service settles what was held.
 ****************************************************************************/

static bool topic_has(FAR struct fixture_s *f, FAR const char *name,
                      uint8_t state, int pid)
{
  int i;

  for (i = 0; i < f->nseen && i < 8; i++)
    {
      if (strcmp(f->seen[i].name, name) == 0 &&
          f->seen[i].state == state && f->seen[i].pid == pid)
        {
          return true;
        }
    }

  return false;
}

static bool late_published(FAR struct fixture_s *f)
{
  return topic_has(f, "late", PNUT_SERVICE_STATE_STATE_READY, 12);
}

/* A service's state and task in the table, kept in rets[i] and
 * rets[i + 1] at a step: what Get and List answer then
 */

static void table_kept(FAR struct fixture_s *f, int i, FAR const char *name)
{
  FAR struct states_service_s *kept = service(f, name);

  f->rets[i]     = kept != NULL ? kept->state : -1;
  f->rets[i + 1] = kept != NULL ? (int)kept->pid : -1;
}

/* Another service's changes, which mark where the topic has got to */

static bool mark_published(FAR struct fixture_s *f)
{
  return topic_has(f, "mark", PNUT_SERVICE_STATE_STATE_READY, 50);
}

static bool mark_again(FAR struct fixture_s *f)
{
  return topic_has(f, "mark", PNUT_SERVICE_STATE_STATE_READY, 51);
}

static bool mark_last(FAR struct fixture_s *f)
{
  return topic_has(f, "mark", PNUT_SERVICE_STATE_STATE_READY, 52);
}

/* The watch's line about "late" with its new task and the old state, as a
 * list's line could be
 */

static void late_relined(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "late", "ready", 13, true);
  fake_set(&f->fake, "mark", "ready", 52, true);
  f->until = mark_last;
  f->then  = NULL;
}

/* "late" again, as a new task while the watch still shows the old one
 * ready; then another service's change
 */

static void who_restarted(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "ready", 13, false);
  f->rets[1] = states_who(&f->states, 13, &name);
  table_kept(f, 2, "late");
  fake_set(&f->fake, "mark", "ready", 51, true);
  f->until = mark_again;
  f->then  = late_relined;
}

/* The watch's line about "late", the same as Who found, twice; then its
 * next
 */

static void late_lines(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  f->until = late_published;
  f->then  = who_restarted;
}

/* "late" started, Who asked before the watch's event; then another
 * service's change, on the topic before anything about "late"
 */

static void who_started(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  table_kept(f, 4, "late");
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = late_lines;
}

static bool list_held(FAR struct fixture_s *f)
{
  bool held;

  pthread_mutex_lock(&f->fake.lock);
  held = f->fake.held >= 0;
  pthread_mutex_unlock(&f->fake.lock);
  return held;
}

/* Who asked while the first list is held back; then the list, whose line
 * about "late" is the same as Who found, the watch's line the same again,
 * and its next
 */

static void who_unlisted(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  f->rets[1] = f->states.listed;

  pthread_mutex_lock(&f->fake.lock);
  f->fake.hold = false;
  fake_list(&f->fake, f->fake.clients[f->fake.held]);
  pthread_mutex_unlock(&f->fake.lock);

  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  f->until = late_published;
  f->then  = NULL;
}

/* The topic's message i: its service, state, task and number; a failure
 * says which message, and what it was
 */

static void seen_is(FAR struct fixture_s *f, int i, FAR const char *name,
                    uint8_t state, int pid, uint32_t seq)
{
  FAR const pnut_service_change_t *msg;

  if (i >= f->nseen || i >= 8)
    {
      fail_msg("message %d not seen: %d came", i, f->nseen);
    }

  msg = &f->seen[i];
  if (strcmp(msg->name, name) != 0 || msg->state != state ||
      msg->pid != pid || msg->seq != seq)
    {
      fail_msg("message %d is %s %d %d #%u, not %s %d %d #%u", i,
               msg->name, (int)msg->state, (int)msg->pid,
               (unsigned)msg->seq, name, (int)state, pid, (unsigned)seq);
    }
}

static void test_states_who_published(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = who_started;
  fixture_run(&f);

  /* In the table at Who: "late", which the list did not name, starting
   * with task 12; then task 13, with the state the watch showed last
   */

  assert_int_equal(f.rets[0], OK);
  assert_int_equal(f.rets[1], OK);
  assert_int_equal(f.rets[4], PNUT_SERVICE_STATE_STATE_STARTING);
  assert_int_equal(f.rets[5], 12);
  assert_int_equal(f.rets[2], PNUT_SERVICE_STATE_STATE_READY);
  assert_int_equal(f.rets[3], 13);

  /* Nothing at Who; the start when the watch's line came, once */

  seen_is(&f, 0, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 2);
  seen_is(&f, 2, "late", PNUT_SERVICE_STATE_STATE_READY, 12, 3);

  /* The new task, with the state the watch showed last: when the line
   * came
   */

  seen_is(&f, 3, "mark", PNUT_SERVICE_STATE_STATE_READY, 51, 4);
  seen_is(&f, 4, "late", PNUT_SERVICE_STATE_STATE_READY, 13, 5);
  seen_is(&f, 5, "mark", PNUT_SERVICE_STATE_STATE_READY, 52, 6);
  assert_int_equal(f.nseen, 6);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* Before the first list: the list, then the events; only the one that
   * differs from the list
   */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  f.fake.hold = true;
  fixture_read(&f);

  f.until = list_held;
  f.then  = who_unlisted;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_false(f.rets[1]);
  assert_int_equal(f.nseen, 1);
  assert_string_equal(f.seen[0].name, "late");
  assert_int_equal(f.seen[0].state, PNUT_SERVICE_STATE_STATE_READY);
  assert_int_equal(f.seen[0].pid, 12);
  assert_int_equal(f.seen[0].seq, 1);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_restarting
 *
 * Description:
 *   A service the watch last showed restarting has no task (the owner's
 *   answer L22), nor has one it showed stopped: a task Who finds for
 *   either is its new start.  The table has it starting at once, as Get
 *   and List answer; the topic has it once, when the watch's line about
 *   it comes (L28).
 ****************************************************************************/

/* "late" crashed, or stopped; its new task asked of Who before the
 * watch's event
 */

static void who_after_crash(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  table_kept(f, 1, "late");
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  f->until = late_published;
  f->then  = NULL;
}

static void test_states_who_restarting(void **state)
{
  struct fixture_s f;

  /* Restarting */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "restarting", 0, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = who_after_crash;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_int_equal(f.rets[1], PNUT_SERVICE_STATE_STATE_STARTING);
  assert_int_equal(f.rets[2], 12);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_READY, 12, 2);
  assert_int_equal(f.nseen, 2);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* Stopped */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "stopped", 0, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = who_after_crash;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_int_equal(f.rets[1], PNUT_SERVICE_STATE_STATE_STARTING);
  assert_int_equal(f.rets[2], 12);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_READY, 12, 2);
  assert_int_equal(f.nseen, 2);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_order
 *
 * Description:
 *   What Who finds, held, does not go ahead of NxInit's older lines still
 *   unread about the service: the topic keeps NxInit's order (L28).  A:
 *   the watch showed "late" ready with task 11, which stopped; its task 12
 *   is found while "stopped" and "starting" are unread.  B: "late"
 *   started, crashed and started again; its task 12 is found while those
 *   three lines are unread.
 ****************************************************************************/

/* A: then "ready", and the same line again, which adds nothing */

static void order_stopped(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "stopped", 0, true);
  fake_set(&f->fake, "late", "starting", 12, true);
  f->rets[0] = states_who(&f->states, 12, &name);
  fake_set(&f->fake, "late", "ready", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

static void order_restarted(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 11, true);
  fake_set(&f->fake, "late", "restarting", 0, true);
  fake_set(&f->fake, "late", "starting", 12, true);
  f->rets[0] = states_who(&f->states, 12, &name);
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

static void test_states_who_order(void **state)
{
  struct fixture_s f;

  /* A */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "ready", 11, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = order_stopped;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STOPPED, 0, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 2);
  seen_is(&f, 2, "late", PNUT_SERVICE_STATE_STATE_READY, 12, 3);
  seen_is(&f, 3, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 4);
  assert_int_equal(f.nseen, 4);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* B */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "stopped", 0, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = order_restarted;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STARTING, 11, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_RESTARTING, 0, 2);
  seen_is(&f, 2, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 3);
  seen_is(&f, 3, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 4);
  assert_int_equal(f.nseen, 4);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_relisted
 *
 * Description:
 *   After a lost watch, while NxInit lists its services again.  C: a task
 *   Who finds is held behind the list's older line about its service,
 *   which differs and is published; the watch's line then publishes the
 *   start.  D: a service the new list does not name is published as
 *   stopped, and what Who found for it before is held no more: a later
 *   line the same as the table publishes nothing.
 ****************************************************************************/

/* The module watches NxInit again; its list is held back */

static bool relisting(FAR struct fixture_s *f)
{
  return !f->states.current && list_held(f);
}

/* Half the list read: "console"; the line about "late" is to come */

static bool half_listed(FAR struct fixture_s *f)
{
  return f->states.listing == 1;
}

/* A whole list has come on the new watch */

static bool relisted(FAR struct fixture_s *f)
{
  int accepts;

  pthread_mutex_lock(&f->fake.lock);
  accepts = f->fake.accepts;
  pthread_mutex_unlock(&f->fake.lock);
  return accepts >= 2 && f->states.current;
}

static void list_send(FAR struct fixture_s *f, FAR const char *lines)
{
  pthread_mutex_lock(&f->fake.lock);
  fake_send(&f->fake, f->fake.clients[f->fake.held], lines);
  pthread_mutex_unlock(&f->fake.lock);
}

/* C: "late" started as NxInit took the list, which still has it stopped;
 * then the watch's line
 */

static void relist_who(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  list_send(f, "late stopped 0\n");
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

static void relist_half(FAR struct fixture_s *f)
{
  list_send(f, "ok 2\nconsole ready 7\n");
  f->until = half_listed;
  f->then  = relist_who;
}

static void relist_held(FAR struct fixture_s *f)
{
  pthread_mutex_lock(&f->fake.lock);
  f->fake.hold = true;
  pthread_mutex_unlock(&f->fake.lock);

  assert_int_equal(write(f->fake.wake[1], "k", 1), 1);
  f->until = relisting;
  f->then  = relist_half;
}

/* D: after the list, a line about "late" the same as the table */

static void unnamed_line(FAR struct fixture_s *f)
{
  fake_set(&f->fake, "late", "stopped", 0, true);
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

/* D: "late" found by Who, then gone from NxInit (a oneshot that ended)
 * while the watch is lost
 */

static void who_then_gone(FAR struct fixture_s *f)
{
  FAR const char *name;
  bool last;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);

  pthread_mutex_lock(&f->fake.lock);
  last = strcmp(f->fake.table[f->fake.ntable - 1].name, "late") == 0;
  f->fake.ntable--;
  pthread_mutex_unlock(&f->fake.lock);
  assert_true(last);

  assert_int_equal(write(f->fake.wake[1], "k", 1), 1);
  f->until = relisted;
  f->then  = unnamed_line;
}

static void test_states_who_relisted(void **state)
{
  struct fixture_s f;

  /* C */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "stopped", 0, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = relist_held;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STOPPED, 0, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 2);
  seen_is(&f, 2, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 3);
  assert_int_equal(f.nseen, 3);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* D */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = who_then_gone;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STOPPED, 0, 1);
  seen_is(&f, 1, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 2);
  assert_int_equal(f.nseen, 2);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_lost
 *
 * Description:
 *   A start Who found outlasts a lost watch: it is still held while
 *   NxInit lists its services again, and the new list's line about the
 *   service, the same as the table, publishes it (L28), once; the watch's
 *   same line after it adds nothing.
 ****************************************************************************/

/* The new list, which has "late" as Who found it; the watch's same line
 * again, then another service's change
 */

static void lost_relist(FAR struct fixture_s *f)
{
  list_send(f, "ok 2\nconsole ready 7\nlate starting 12\n");
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

/* "late" started and Who asked; the watch lost before NxInit's line about
 * it, and the new list held back
 */

static void who_lost(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);

  pthread_mutex_lock(&f->fake.lock);
  f->fake.hold = true;
  pthread_mutex_unlock(&f->fake.lock);

  assert_int_equal(write(f->fake.wake[1], "k", 1), 1);
  f->until = relisting;
  f->then  = lost_relist;
}

static void test_states_who_lost(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  fake_set(&f.fake, "late", "stopped", 0, false);
  fixture_read(&f);

  f.until = listed;
  f.then  = who_lost;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 1);
  seen_is(&f, 1, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 2);
  assert_int_equal(f.nseen, 2);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_who_first_list
 *
 * Description:
 *   Who during the first list, after the list's line about the service:
 *   what it finds is held all the same, and nothing is published before
 *   the whole list has come; then the watch's line, the same as the
 *   table, publishes the start, once.  Who before the first list, which
 *   does not name the service: the list has it stopped in the table, and
 *   publishes nothing about it.
 ****************************************************************************/

/* "late" started after NxInit took the list; the rest of the list, then
 * the watch's lines
 */

static void first_list_who(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  f->rets[1] = f->states.listed;
  list_send(f, "console ready 7\n");
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  f->until = late_published;
  f->then  = NULL;
}

static void first_list_half(FAR struct fixture_s *f)
{
  list_send(f, "ok 2\nlate stopped 0\n");
  f->until = half_listed;
  f->then  = first_list_who;
}

/* "late" started as NxInit took the list, which does not name it; Who
 * asked before the list, then another service's change
 */

static void who_before_list(FAR struct fixture_s *f)
{
  FAR const char *name;

  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[0] = states_who(&f->states, 12, &name);
  f->rets[1] = f->states.listed;
  list_send(f, "ok 1\nconsole ready 7\n");
  fake_set(&f->fake, "mark", "ready", 50, true);
  f->until = mark_published;
  f->then  = NULL;
}

static void test_states_who_first_list(void **state)
{
  FAR struct states_service_s *late;
  struct fixture_s f;

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  f.fake.hold = true;
  fixture_read(&f);

  f.until = list_held;
  f.then  = first_list_half;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_false(f.rets[1]);
  seen_is(&f, 0, "late", PNUT_SERVICE_STATE_STATE_STARTING, 12, 1);
  seen_is(&f, 1, "late", PNUT_SERVICE_STATE_STATE_READY, 12, 2);
  assert_int_equal(f.nseen, 2);
  assert_false(f.fake.broken);
  fixture_teardown(&f);

  /* The list does not name "late" */

  fixture_setup(&f, "");
  fake_set(&f.fake, "console", "ready", 7, false);
  f.fake.hold = true;
  fixture_read(&f);

  f.until = list_held;
  f.then  = who_before_list;
  fixture_run(&f);

  assert_int_equal(f.rets[0], OK);
  assert_false(f.rets[1]);
  late = service(&f, "late");
  assert_non_null(late);
  assert_int_equal(late->state, PNUT_SERVICE_STATE_STATE_STOPPED);
  assert_int_equal(late->pid, 0);
  seen_is(&f, 0, "mark", PNUT_SERVICE_STATE_STATE_READY, 50, 1);
  assert_int_equal(f.nseen, 1);
  assert_false(f.fake.broken);
  fixture_teardown(&f);
}

/****************************************************************************
 * Name: test_states_ready_who
 *
 * Description:
 *   A program of the table that NxInit's list did not name, found by Who
 *   before the watch has shown it, is waited for as a program started
 *   late: the hold begun before it is given up, and config.ready is told
 *   once it has stayed ready for a whole hold of its own.
 ****************************************************************************/

static bool half_hold(FAR struct fixture_s *f)
{
  return now_ms() - f->mark >= READY_HOLD_MS / 2;
}

/* Five sixths of a hold since "late" was found: the hold begun before it
 * would be over
 */

static bool late_ready_a_while(FAR struct fixture_s *f)
{
  FAR struct states_service_s *late = service(f, "late");

  return late != NULL && late->state == PNUT_SERVICE_STATE_STATE_READY &&
         now_ms() - f->mark >= 5 * READY_HOLD_MS / 6;
}

static void late_held(FAR struct fixture_s *f)
{
  f->rets[2] = f->readies;
  f->mark    = now_ms();
  f->until   = told_relisted;
  f->then    = NULL;
}

static void program_found(FAR struct fixture_s *f)
{
  FAR const char *name;

  f->rets[0] = f->readies;
  fake_set(&f->fake, "late", "starting", 12, false);
  f->rets[1] = states_who(&f->states, 12, &name);
  fake_set(&f->fake, "late", "starting", 12, true);
  fake_set(&f->fake, "late", "ready", 12, true);
  f->mark  = now_ms();
  f->until = late_ready_a_while;
  f->then  = late_held;
}

static void hold_begun(FAR struct fixture_s *f)
{
  f->mark  = now_ms();
  f->until = half_hold;
  f->then  = program_found;
}

static void test_states_ready_who(void **state)
{
  struct fixture_s f;

  fixture_setup(&f, "system states\nlate late\n");
  f.states.config.ready      = ready_counted;
  f.states.config.ready_arg  = &f;
  f.states.config.ready_hold = READY_HOLD_MS;

  fake_set(&f.fake, "system", "ready", 8, false);
  fake_set(&f.fake, "console", "ready", 7, false);

  f.until = listed;
  f.then  = hold_begun;
  fixture_run(&f);

  /* Not told within half a hold of the list, nor within a hold of
   * "late" coming; then told once
   */

  assert_int_equal(f.rets[0], 0);
  assert_int_equal(f.rets[1], OK);
  assert_int_equal(f.rets[2], 0);
  assert_int_equal(f.readies, 1);
  assert_false(f.fake.broken);
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
    cmocka_unit_test(test_states_ready),
    cmocka_unit_test(test_states_ready_relisted),
    cmocka_unit_test(test_states_halt),
    cmocka_unit_test(test_states_who),
    cmocka_unit_test(test_states_who_unanswered),
    cmocka_unit_test(test_states_calls),
    cmocka_unit_test(test_states_who_named),
    cmocka_unit_test(test_states_long_name),
    cmocka_unit_test(test_states_origin),
    cmocka_unit_test(test_states_who_published),
    cmocka_unit_test(test_states_who_restarting),
    cmocka_unit_test(test_states_who_order),
    cmocka_unit_test(test_states_who_relisted),
    cmocka_unit_test(test_states_who_lost),
    cmocka_unit_test(test_states_who_first_list),
    cmocka_unit_test(test_states_ready_who),
  };

  return cmocka_run_group_tests_name("states", tests, NULL, NULL);
}
