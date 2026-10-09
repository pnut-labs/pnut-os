/****************************************************************************
 * pnut-os/tests/unit/test_conn.c
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

/* The connection layer is the library's own */

#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A connection over a socketpair, its peer read by the test.  Its owner
 * answers each request at once with a whole message, as a service does,
 * and leaves it for later while the send buffer has no room for that, or
 * while blocked.
 */

struct fixture_s
{
  FAR struct pnut_loop_s *loop;
  struct pnut_conn_s conn;
  int peer;
  int offers;
  int msgs;
  int closed;
  bool blocked;
  uint8_t rx[8 * PNUT_MSG_MAX];
  size_t rxlen;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int on_message(FAR struct pnut_conn_s *conn,
                      FAR const struct pnut_msghdr_s *hdr,
                      FAR const uint8_t *payload)
{
  FAR struct fixture_s *f = conn->owner;
  static uint8_t big[PNUT_MSG_PAYLOAD_MAX];
  struct pnut_msghdr_s reply;

  f->offers++;
  if (f->blocked || conn->txsize - conn->txlen < PNUT_MSG_MAX)
    {
      return PNUT_CONN_WAIT;
    }

  f->msgs++;

  memset(big, (uint8_t)hdr->seq, sizeof(big));
  memset(&reply, 0, sizeof(reply));
  reply.version = PNUT_MSG_VERSION;
  reply.kind    = PNUT_KIND_REPLY;
  reply.seq     = hdr->seq;
  reply.len     = sizeof(big);

  assert_int_equal(pnut_conn_send(conn, &reply, big), 0);
  return PNUT_CONN_NEXT;
}

static void on_closed(FAR struct pnut_conn_s *conn)
{
  FAR struct fixture_s *f = conn->owner;

  f->closed++;
}

static void stop_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

/* The peer reads 1 KB a millisecond: a slow client */

static void drain_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct fixture_s *f = arg;
  size_t room = sizeof(f->rx) - f->rxlen;
  ssize_t n;

  n = read(f->peer, f->rx + f->rxlen, room < 1024 ? room : 1024);
  if (n > 0)
    {
      f->rxlen += n;
    }
}

static void run_for(FAR struct fixture_s *f, uint32_t ms)
{
  assert_int_equal(pnut_timer_start(f->loop, ms, 0, stop_timer, NULL,
                                    NULL), 0);
  assert_int_equal(pnut_loop_run(f->loop), 0);
}

static void write_requests(FAR struct fixture_s *f, int n)
{
  uint8_t buf[8 * PNUT_MSG_HEADER];
  struct pnut_msghdr_s hdr;
  int i;

  for (i = 0; i < n; i++)
    {
      memset(&hdr, 0, sizeof(hdr));
      hdr.version  = PNUT_MSG_VERSION;
      hdr.kind     = PNUT_KIND_REQUEST;
      hdr.iface    = 7;
      hdr.method   = 1;
      hdr.mversion = 1;
      hdr.seq      = i + 1;
      pnut_msg_encode(&hdr, buf + PNUT_MSG_HEADER * i);
    }

  assert_int_equal(write(f->peer, buf, PNUT_MSG_HEADER * n),
                   PNUT_MSG_HEADER * n);
}

static int setup(void **state)
{
  FAR struct fixture_s *f = calloc(1, sizeof(*f));
  int one = 1;
  int sv[2];

  assert_int_equal(pnut_loop_create(NULL, &f->loop), 0);
  assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv),
                   0);

  /* The kernel's smallest send buffer, as NuttX's 1 KB FIFOs: answers
   * pile up in the connection's own buffer
   */

  setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &one, sizeof(one));

  assert_int_equal(pnut_conn_init(&f->conn, f->loop, 2 * PNUT_MSG_MAX),
                   0);
  f->conn.message = on_message;
  f->conn.closed  = on_closed;
  f->conn.owner   = f;
  assert_int_equal(pnut_conn_attach(&f->conn, sv[0]), 0);
  f->peer = sv[1];

  *state = f;
  return 0;
}

static int teardown(void **state)
{
  FAR struct fixture_s *f = *state;

  pnut_conn_deinit(&f->conn);
  if (f->peer >= 0)
    {
      close(f->peer);
    }

  pnut_loop_destroy(f->loop);
  free(f);
  return 0;
}

/* Six requests in one write, each answered with a whole message: the
 * connection is held after two or three, reading nothing more, goes on as
 * the peer drains it, and every answer arrives whole and in order.
 */

static void test_conn_hold_and_resume(void **state)
{
  FAR struct fixture_s *f = *state;
  struct pnut_msghdr_s hdr;
  size_t off;
  int i;

  write_requests(f, 6);
  run_for(f, 20);

  assert_in_range(f->msgs, 1, 3);
  assert_true(f->conn.held);
  assert_int_equal(f->conn.events, EPOLLOUT);

  assert_int_equal(pnut_timer_start(f->loop, 1, 1, drain_timer, f, NULL),
                   0);
  run_for(f, 1000);

  assert_int_equal(f->msgs, 6);
  assert_false(f->conn.held);
  assert_int_equal(f->conn.txlen, 0);
  assert_int_equal(f->conn.events, EPOLLIN);
  assert_int_equal(f->rxlen, 6 * PNUT_MSG_MAX);

  for (off = 0, i = 1; i <= 6; i++, off += PNUT_MSG_MAX)
    {
      assert_int_equal(pnut_msg_decode(f->rx + off, &hdr), 0);
      assert_int_equal(hdr.seq, i);
      assert_int_equal(f->rx[off + PNUT_MSG_HEADER], (uint8_t)i);
      assert_int_equal(f->rx[off + PNUT_MSG_MAX - 1], (uint8_t)i);
    }

  assert_int_equal(f->closed, 0);
}

/* A peer gone while the connection is held: it is lost, once, rather than
 * waiting for room that never comes
 */

static void test_conn_held_peer_closes(void **state)
{
  FAR struct fixture_s *f = *state;

  write_requests(f, 6);
  run_for(f, 20);
  assert_true(f->conn.held);

  close(f->peer);
  f->peer = -1;
  run_for(f, 50);

  assert_int_equal(f->closed, 1);
  assert_true(f->conn.fd < 0);
}

/* A message left for later with nothing to send, for room its owner
 * keeps: the connection waits without spinning, and offers it again once
 * the owner gives the room back
 */

static void test_conn_recheck(void **state)
{
  FAR struct fixture_s *f = *state;

  f->blocked = true;
  write_requests(f, 1);
  run_for(f, 50);

  assert_int_equal(f->offers, 1);
  assert_true(f->conn.held);
  assert_int_equal(f->conn.events, 0);

  f->blocked = false;
  pnut_conn_recheck(&f->conn);
  run_for(f, 20);

  assert_int_equal(f->offers, 2);
  assert_int_equal(f->msgs, 1);
  assert_false(f->conn.held);
  assert_int_equal(f->conn.events & EPOLLIN, EPOLLIN);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_conn_hold_and_resume, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_conn_held_peer_closes, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_conn_recheck, setup, teardown),
  };

  return cmocka_run_group_tests_name("conn", tests, NULL, NULL);
}
