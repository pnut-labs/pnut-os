/****************************************************************************
 * pnut-os/tests/unit/test_ipc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
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

#include <pnut/client.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/service.h>
#include <pnut/timer.h>

/* For CONFIG_PNUT_LIB_INFLIGHT's default */

#include "pnut_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IFACE         7
#define M_ECHO        1               /* Answers with the body it gets */
#define M_LATER       2               /* Answers after 30 ms */
#define M_SILENT      3               /* Never answers */
#define M_EVENT       4               /* Sends an event, then answers */
#define M_BIG         5               /* Answers with a whole message */
#define M_LATERBIG    6               /* The same, after 30 ms */
#define M_SMALL       7               /* Says 4 bytes, tries 8 first */

#define NMETHODS      (sizeof(g_methods) / sizeof(g_methods[0]))

/* One more client than a program may hold connections to a service */

#define CALLERS       (CONFIG_PNUT_LIB_PERCALLER + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fixture_s
{
  FAR struct pnut_loop_s *loop;
  FAR struct pnut_service_s *service;
  FAR struct pnut_client_s *client;
  struct pnut_request_s later;
  char rundir[64];
  int ups;
  int downs;
  int replies;
  int status[16];
  uint16_t lens[16];
  uint8_t body[PNUT_MSG_PAYLOAD_MAX];
  int events;
  pid_t pid;
  int stage;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void h_echo(FAR struct pnut_service_s *service,
                   FAR const struct pnut_request_s *req,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg);
static void h_later(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg);
static void h_silent(FAR struct pnut_service_s *service,
                     FAR const struct pnut_request_s *req,
                     FAR const uint8_t *payload, uint16_t len,
                     FAR void *arg);
static void h_event(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg);
static void h_big(FAR struct pnut_service_s *service,
                  FAR const struct pnut_request_s *req,
                  FAR const uint8_t *payload, uint16_t len, FAR void *arg);
static void h_laterbig(FAR struct pnut_service_s *service,
                       FAR const struct pnut_request_s *req,
                       FAR const uint8_t *payload, uint16_t len,
                       FAR void *arg);
static void h_small(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pnut_method_s g_methods[] =
{
  { IFACE, M_ECHO,     1, h_echo,     0  },  /* Any size */
  { IFACE, M_LATER,    1, h_later,    4  },
  { IFACE, M_SILENT,   1, h_silent,   16 },
  { IFACE, M_EVENT,    1, h_event,    1  },
  { IFACE, M_BIG,      1, h_big,      0  },
  { IFACE, M_LATERBIG, 1, h_laterbig, 0  },
  { IFACE, M_SMALL,    1, h_small,    4  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void h_echo(FAR struct pnut_service_s *service,
                   FAR const struct pnut_request_s *req,
                   FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->pid = req->pid;
  assert_int_equal(pnut_reply(req, PNUT_STATUS_OK, payload, len), 0);
}

static void later_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  pnut_reply(&f->later, PNUT_STATUS_OK, "late", 4);
}

static void h_later(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->later = *req;
  pnut_timer_start(f->loop, 30, 0, later_timer, f, NULL);
}

static void h_silent(FAR struct pnut_service_s *service,
                     FAR const struct pnut_request_s *req,
                     FAR const uint8_t *payload, uint16_t len,
                     FAR void *arg)
{
}

static void h_event(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  assert_int_equal(pnut_event(&req->from, IFACE, 99, "ev", 2), 0);
  pnut_reply(req, PNUT_STATUS_OK, NULL, 0);
}

static void h_big(FAR struct pnut_service_s *service,
                  FAR const struct pnut_request_s *req,
                  FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  static uint8_t big[PNUT_MSG_PAYLOAD_MAX];

  memset(big, (uint8_t)req->seq, sizeof(big));
  assert_int_equal(pnut_reply(req, PNUT_STATUS_OK, big, sizeof(big)), 0);
}

static void laterbig_timer(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  static uint8_t big[PNUT_MSG_PAYLOAD_MAX];

  memset(big, 0xab, sizeof(big));
  assert_int_equal(pnut_reply(&f->later, PNUT_STATUS_OK, big, sizeof(big)),
                   0);
}

static void h_laterbig(FAR struct pnut_service_s *service,
                       FAR const struct pnut_request_s *req,
                       FAR const uint8_t *payload, uint16_t len,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->later = *req;
  pnut_timer_start(f->loop, 30, 0, laterbig_timer, f, NULL);
}

static void h_small(FAR struct pnut_service_s *service,
                    FAR const struct pnut_request_s *req,
                    FAR const uint8_t *payload, uint16_t len, FAR void *arg)
{
  /* Over what the method said: refused, and the request still owed */

  assert_int_equal(pnut_reply(req, PNUT_STATUS_OK, "too long", 8),
                   -EMSGSIZE);
  assert_int_equal(pnut_reply(req, PNUT_STATUS_OK, "fits", 4), 0);
}

static void on_reply(FAR struct pnut_client_s *client, int status,
                     FAR const uint8_t *payload, uint16_t len,
                     FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->status[f->replies] = status;
  f->lens[f->replies] = len;
  if (len > 0)
    {
      memcpy(f->body, payload, len);
    }

  f->replies++;
}

static void on_event(FAR struct pnut_client_s *client, uint16_t iface,
                     uint16_t method, FAR const uint8_t *payload,
                     uint16_t len, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  assert_int_equal(method, 99);
  assert_memory_equal(payload, "ev", 2);
  f->events++;
}

static void stop_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static int setup(void **state)
{
  struct pnut_loop_config_s config;
  FAR struct fixture_s *f = calloc(1, sizeof(*f));

  /* On the computer a directory of sockets; on NuttX only a prefix of
   * the sockets' names, which leave no files: mkdir may fail there
   */

  snprintf(f->rundir, sizeof(f->rundir), "/tmp/pnut-test-%d", getpid());
  mkdir(f->rundir, 0700);

  pnut_loop_defaults(&config);
  config.rundir = f->rundir;
  assert_int_equal(pnut_loop_create(&config, &f->loop), 0);
  assert_int_equal(pnut_service_open(f->loop, "test", g_methods, NMETHODS,
                                     0, f, &f->service), 0);
  *state = f;
  return 0;
}

static int teardown(void **state)
{
  FAR struct fixture_s *f = *state;

  pnut_client_close(f->client);
  pnut_service_close(f->service);
  pnut_loop_destroy(f->loop);
  rmdir(f->rundir);
  free(f);
  return 0;
}

/* Calls made once the client is connected, from its state handler */

static void calls_state(FAR struct pnut_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  static uint8_t big[PNUT_MSG_PAYLOAD_MAX];
  int i;

  if (!up)
    {
      f->downs++;
      return;
    }

  f->ups++;
  for (i = 0; i < (int)sizeof(big); i++)
    {
      big[i] = (uint8_t)i;
    }

  /* A whole message: more than a local socket takes at once on NuttX */

  assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, big, sizeof(big), 0,
                             on_reply, f), 0);
  assert_int_equal(pnut_call(client, IFACE, 42, 1, NULL, 0, 0, on_reply,
                             f), 0);
  assert_int_equal(pnut_call(client, IFACE, M_ECHO, 2, NULL, 0, 0,
                             on_reply, f), 0);
  assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0, 50,
                             on_reply, f), 0);
  assert_int_equal(pnut_call(client, IFACE, M_LATER, 1, NULL, 0, 0,
                             on_reply, f), 0);
  assert_int_equal(pnut_call(client, IFACE, M_EVENT, 1, NULL, 0, 0,
                             on_reply, f), 0);
  assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, NULL,
                             PNUT_MSG_PAYLOAD_MAX + 1, 0, on_reply, f),
                   -EMSGSIZE);

  pnut_timer_start(f->loop, 200, 0, stop_timer, NULL, NULL);
}

static void test_ipc_calls(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, on_event,
                                    calls_state, f, &f->client), 0);
  assert_false(pnut_client_connected(f->client));
  assert_int_equal(pnut_call(f->client, IFACE, M_ECHO, 1, NULL, 0, 0,
                             on_reply, f), -ENOTCONN);

  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->ups, 1);
  assert_int_equal(f->replies, 6);

  /* In the order they were answered: echo, not found, invalid (another
   * version), event's reply; then the timeout (50 ms) and the late reply
   * (30 ms after its request, so before the timeout of 5 s).
   */

  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_int_equal(f->lens[0], PNUT_MSG_PAYLOAD_MAX);
  assert_int_equal(f->status[1], PNUT_STATUS_NOTFOUND);
  assert_int_equal(f->status[2], PNUT_STATUS_INVALID);
  assert_int_equal(f->status[3], PNUT_STATUS_OK);
  assert_int_equal(f->events, 1);

  for (i = 4; i < 6; i++)
    {
      assert_true(f->status[i] == PNUT_STATUS_OK ||
                  f->status[i] == PNUT_STATUS_TIMEOUT);
    }

  assert_int_not_equal(f->status[4], f->status[5]);
  assert_int_equal(f->pid, getpid());
}

/* Too many calls in flight */

static void busy_state(FAR struct pnut_client_s *client, bool up,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0, 20,
                                 on_reply, f), 0);
      assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0, 20,
                                 on_reply, f), 0);
      assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0, 20,
                                 on_reply, f), -EBUSY);
      pnut_timer_start(f->loop, 60, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_busy(void **state)
{
  FAR struct fixture_s *f = *state;

  assert_int_equal(pnut_client_open(f->loop, "test", 2, NULL, busy_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->replies, 2);
  assert_int_equal(f->status[0], PNUT_STATUS_TIMEOUT);
  assert_int_equal(f->status[1], PNUT_STATUS_TIMEOUT);
}

/* The service goes away and comes back: the call in flight fails at once,
 * and the client connects again by itself.
 */

static void reopen_timer(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  assert_int_equal(pnut_service_open(f->loop, "test", g_methods, NMETHODS,
                                     0, f, &f->service), 0);
}

static void restart_state(FAR struct pnut_client_s *client, bool up,
                          FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      f->ups++;
      if (f->ups == 1)
        {
          assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0, 0,
                                     on_reply, f), 0);
          pnut_service_close(f->service);
          f->service = NULL;
          pnut_timer_start(f->loop, 150, 0, reopen_timer, f, NULL);
        }
      else
        {
          pnut_loop_stop(f->loop, 0);
        }
    }
  else
    {
      f->downs++;
    }
}

static void test_ipc_restart(void **state)
{
  FAR struct fixture_s *f = *state;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL,
                                    restart_state, f, &f->client), 0);
  pnut_timer_start(f->loop, 3000, 0, stop_timer, NULL, NULL);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->ups, 2);
  assert_int_equal(f->downs, 1);
  assert_int_equal(f->replies, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_UNAVAILABLE);
}

/* A reply handler that closes its own client */

static void close_reply(FAR struct pnut_client_s *client, int status,
                        FAR const uint8_t *payload, uint16_t len,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  f->replies++;
  pnut_client_close(client);
  f->client = NULL;
  pnut_timer_start(f->loop, 20, 0, stop_timer, NULL, NULL);
}

static void close_state(FAR struct pnut_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, "x", 1, 0,
                                 close_reply, f), 0);
      assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, "y", 1, 0,
                                 close_reply, f), 0);
    }
}

static void test_ipc_close_in_handler(void **state)
{
  FAR struct fixture_s *f = *state;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL, close_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);

  /* The second reply, in the same read or a later one, is dropped with
   * the client
   */

  assert_int_equal(f->replies, 1);
}

/* A client gone before the service accepts it: the connection accepted
 * has no peer any more.  NuttX 13.1 followed a NULL pointer there, in
 * getsockopt(SO_PEERCRED); the service must take it and go on serving.
 */

static void gone_state(FAR struct pnut_client_s *client, bool up,
                       FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      f->ups++;
      assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, "z", 1, 0,
                                 on_reply, f), 0);
      pnut_timer_start(f->loop, 100, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_gone_before_accept(void **state)
{
  FAR struct fixture_s *f = *state;
  struct sockaddr_un addr;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/test", f->rundir);

  /* Connected and closed before the loop runs, so before the accept */

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  assert_true(fd >= 0);
  assert_int_equal(connect(fd, (FAR struct sockaddr *)&addr,
                           sizeof(addr)), 0);
  close(fd);

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL, gone_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->ups, 1);
  assert_int_equal(f->replies, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_int_equal(f->lens[0], 1);
}

/* Answers of the largest size to requests that came in one read: more
 * than a local socket takes at once on NuttX.  Each waits for room in the
 * send buffer, rather than overflowing it and losing the connection.
 */

static void paced_reply(FAR struct pnut_client_s *client, int status,
                        FAR const uint8_t *payload, uint16_t len,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  on_reply(client, status, payload, len, arg);
  if (f->replies == 8)
    {
      pnut_loop_stop(f->loop, 0);
    }
}

static void paced_state(FAR struct pnut_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  int i;

  if (up)
    {
      for (i = 0; i < 8; i++)
        {
          assert_int_equal(pnut_call(client, IFACE, M_BIG, 1, NULL, 0, 0,
                                     paced_reply, f), 0);
        }

      pnut_timer_start(f->loop, 2000, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_paced(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL, paced_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->replies, 8);
  for (i = 0; i < 8; i++)
    {
      assert_int_equal(f->status[i], PNUT_STATUS_OK);
      assert_int_equal(f->lens[i], PNUT_MSG_PAYLOAD_MAX);
    }
}

/* An answer of the largest size given later, while answers given at once
 * fill the send buffer: its room was kept, so it is sent, not lost.  The
 * buffer fills only where the kernel takes little at a time, as on NuttX;
 * test_conn covers the holding on the computer too.
 */

static void later_reply(FAR struct pnut_client_s *client, int status,
                        FAR const uint8_t *payload, uint16_t len,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  on_reply(client, status, payload, len, arg);
  if (f->replies == 7)
    {
      pnut_loop_stop(f->loop, 0);
    }
}

static void later_state(FAR struct pnut_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  int i;

  if (up)
    {
      assert_int_equal(pnut_call(client, IFACE, M_LATERBIG, 1, NULL, 0, 0,
                                 later_reply, f), 0);
      for (i = 0; i < 6; i++)
        {
          assert_int_equal(pnut_call(client, IFACE, M_BIG, 1, NULL, 0, 0,
                                     later_reply, f), 0);
        }

      pnut_timer_start(f->loop, 2000, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_later_answer(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL, later_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->replies, 7);
  for (i = 0; i < 7; i++)
    {
      assert_int_equal(f->status[i], PNUT_STATUS_OK);
      assert_int_equal(f->lens[i], PNUT_MSG_PAYLOAD_MAX);
    }
}

/* An answer over its method's replymax would take room kept for other
 * requests: it is refused, and the right one goes
 */

static void small_state(FAR struct pnut_client_s *client, bool up,
                        FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      assert_int_equal(pnut_call(client, IFACE, M_SMALL, 1, NULL, 0, 0,
                                 on_reply, f), 0);
      pnut_timer_start(f->loop, 100, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_replymax(void **state)
{
  FAR struct fixture_s *f = *state;

  assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL, small_state,
                                    f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->replies, 1);
  assert_int_equal(f->status[0], PNUT_STATUS_OK);
  assert_int_equal(f->lens[0], 4);
  assert_memory_equal(f->body, "fits", 4);
}

/* One request more than may wait for its answer is answered busy, at
 * once; the others time out, never answered
 */

static void pending_state(FAR struct pnut_client_s *client, bool up,
                          FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  int i;

  if (up)
    {
      for (i = 0; i < CONFIG_PNUT_LIB_INFLIGHT + 1; i++)
        {
          assert_int_equal(pnut_call(client, IFACE, M_SILENT, 1, NULL, 0,
                                     100, on_reply, f), 0);
        }

      pnut_timer_start(f->loop, 300, 0, stop_timer, NULL, NULL);
    }
}

static void test_ipc_service_busy(void **state)
{
  FAR struct fixture_s *f = *state;
  int i;

  assert_int_equal(pnut_client_open(f->loop, "test",
                                    CONFIG_PNUT_LIB_INFLIGHT + 1, NULL,
                                    pending_state, f, &f->client), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
  assert_int_equal(f->replies, CONFIG_PNUT_LIB_INFLIGHT + 1);
  assert_int_equal(f->status[0], PNUT_STATUS_BUSY);
  for (i = 1; i <= CONFIG_PNUT_LIB_INFLIGHT; i++)
    {
      assert_int_equal(f->status[i], PNUT_STATUS_TIMEOUT);
    }
}

/* A service with room for one connection closes the next as soon as it
 * accepts it: that client's tries grow apart, not every 100 ms
 */

static void backoff_state(FAR struct pnut_client_s *client, bool up,
                          FAR void *arg)
{
  FAR struct fixture_s *f = arg;

  if (up)
    {
      f->ups++;
    }
}

static void test_ipc_backoff(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR struct pnut_service_s *one;
  FAR struct pnut_client_s *first;

  assert_int_equal(pnut_service_open(f->loop, "one", g_methods, NMETHODS,
                                     1, f, &one), 0);
  assert_int_equal(pnut_client_open(f->loop, "one", 0, NULL, NULL, NULL,
                                    &first), 0);
  assert_int_equal(pnut_client_open(f->loop, "one", 0, NULL,
                                    backoff_state, f, &f->client), 0);
  pnut_timer_start(f->loop, 1000, 0, stop_timer, NULL, NULL);
  assert_int_equal(pnut_loop_run(f->loop), 0);

  /* Tries at 0, 100, 300 and 700 ms */

  assert_in_range(f->ups, 1, 5);

  pnut_client_close(first);
  pnut_service_close(one);
}

/* A program holds a few of a service's connections at most: one more,
 * from the same program, is closed as soon as it is accepted, and its
 * tries grow apart, while the others stay
 */

static void caller_state(FAR struct pnut_client_s *client, bool up,
                         FAR void *arg)
{
  FAR int *ups = arg;

  if (up)
    {
      (*ups)++;
    }
}

static void test_ipc_per_caller(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR struct pnut_client_s *clients[CALLERS];
  int ups[CALLERS];
  int i;

  memset(ups, 0, sizeof(ups));
  for (i = 0; i < CALLERS; i++)
    {
      assert_int_equal(pnut_client_open(f->loop, "test", 0, NULL,
                                        caller_state, &ups[i],
                                        &clients[i]), 0);
    }

  pnut_timer_start(f->loop, 1000, 0, stop_timer, NULL, NULL);
  assert_int_equal(pnut_loop_run(f->loop), 0);

  for (i = 0; i < CALLERS - 1; i++)
    {
      assert_int_equal(ups[i], 1);
      assert_true(pnut_client_connected(clients[i]));
    }

  /* Refused, and tried again: at 0, 100, 300 and 700 ms */

  assert_in_range(ups[CALLERS - 1], 2, 5);

  for (i = 0; i < CALLERS; i++)
    {
      pnut_client_close(clients[i]);
    }
}

/* A client takes one of the loop's timers, however many calls it has in
 * flight
 */

struct timers_s
{
  FAR struct pnut_loop_s *loop;
  int replies;
};

static void timers_reply(FAR struct pnut_client_s *client, int status,
                         FAR const uint8_t *payload, uint16_t len,
                         FAR void *arg)
{
  FAR struct timers_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  if (++t->replies == 8)
    {
      pnut_loop_stop(t->loop, 0);
    }
}

static void timers_state(FAR struct pnut_client_s *client, bool up,
                         FAR void *arg)
{
  int i;

  if (up)
    {
      for (i = 0; i < 8; i++)
        {
          assert_int_equal(pnut_call(client, IFACE, M_ECHO, 1, "t", 1, 0,
                                     timers_reply, arg), 0);
        }
    }
}

static void test_ipc_timers(void **state)
{
  FAR struct fixture_s *f = *state;
  struct pnut_loop_config_s config;
  FAR struct pnut_service_s *service;
  FAR struct pnut_client_s *client;
  struct timers_s t;

  /* The service's, the client's, and one to stop a test gone wrong */

  pnut_loop_defaults(&config);
  config.rundir = f->rundir;
  config.timers = 3;

  memset(&t, 0, sizeof(t));
  assert_int_equal(pnut_loop_create(&config, &t.loop), 0);
  assert_int_equal(pnut_service_open(t.loop, "timers", g_methods,
                                     NMETHODS, 0, f, &service), 0);
  assert_int_equal(pnut_client_open(t.loop, "timers", 0, NULL,
                                    timers_state, &t, &client), 0);
  assert_int_equal(pnut_timer_start(t.loop, 2000, 0, stop_timer, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(t.loop), 0);
  assert_int_equal(t.replies, 8);

  pnut_client_close(client);
  pnut_service_close(service);
  pnut_loop_destroy(t.loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_ipc_calls, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_restart, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_close_in_handler, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_ipc_gone_before_accept, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_ipc_paced, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_later_answer, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_ipc_replymax, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_service_busy, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_ipc_backoff, setup, teardown),
    cmocka_unit_test_setup_teardown(test_ipc_per_caller, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_ipc_timers, setup, teardown),
  };

  return cmocka_run_group_tests_name("ipc", tests, NULL, NULL);
}
