/****************************************************************************
 * pnut-os/tests/unit/test_mux.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony's multiplexer (RFC 0030): 27.010's basic option, its frames
 * checked against the standard's examples, the queue against a UART that
 * takes a few bytes at a time, and the modem's messages and restarts.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "mux.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A fake UART, and what the multiplexer told */

struct mux_test_s
{
  struct mux_s mux;
  uint8_t out[4096];              /* What the UART took */
  size_t nout;
  size_t chunk;                   /* It takes this many at a time; 0 all */
  bool full;                      /* It takes none now */
  uint8_t data[2048];             /* The AT channel's data */
  size_t ndata;
  uint8_t data_dlci;
  int events[16];
  uint8_t dlcis[16];
  int nevents;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static ssize_t mux_test_write(FAR void *arg, FAR const uint8_t *buf,
                              size_t len)
{
  FAR struct mux_test_s *t = arg;

  if (t->full)
    {
      return 0;
    }

  if (t->chunk > 0 && len > t->chunk)
    {
      len = t->chunk;
    }

  assert_true(t->nout + len <= sizeof(t->out));
  memcpy(t->out + t->nout, buf, len);
  t->nout += len;
  return len;
}

static void mux_test_data(FAR void *arg, uint8_t dlci,
                          FAR const uint8_t *buf, size_t len)
{
  FAR struct mux_test_s *t = arg;

  assert_true(t->ndata + len <= sizeof(t->data));
  memcpy(t->data + t->ndata, buf, len);
  t->ndata    += len;
  t->data_dlci = dlci;
}

static void mux_test_event(FAR void *arg, uint8_t dlci, int event)
{
  FAR struct mux_test_s *t = arg;

  assert_true(t->nevents < 16);
  t->dlcis[t->nevents]    = dlci;
  t->events[t->nevents++] = event;
}

static const struct mux_ops_s g_mux_test_ops =
{
  .write = mux_test_write,
  .data  = mux_test_data,
  .event = mux_test_event,
};

static void mux_test_init(FAR struct mux_test_s *t)
{
  memset(t, 0, sizeof(*t));
  mux_init(&t->mux, &g_mux_test_ops, t);
}

/* 27.010's FCS, written apart from the multiplexer's */

static uint8_t mux_test_fcs(FAR const uint8_t *p, size_t n)
{
  uint8_t crc = 0xff;
  int i;

  while (n-- > 0)
    {
      crc ^= *p++;
      for (i = 0; i < 8; i++)
        {
          crc = (crc & 1) ? (crc >> 1) ^ 0xe0 : crc >> 1;
        }
    }

  return 0xff - crc;
}

/* A frame as the modem sends it: its information 127 bytes at most */

static size_t mux_test_frame(FAR uint8_t *buf, uint8_t dlci, uint8_t ctrl,
                             bool command, FAR const void *info, size_t n)
{
  buf[0] = 0xf9;
  buf[1] = (dlci << 2) | (command ? 0x02 : 0) | 0x01;
  buf[2] = ctrl;
  buf[3] = (n << 1) | 0x01;
  memcpy(buf + 4, info, n);
  buf[4 + n] = mux_test_fcs(buf + 1, 3);
  buf[5 + n] = 0xf9;
  return n + 6;
}

/* A control channel message from the modem */

static size_t mux_test_control(FAR uint8_t *buf, uint8_t type,
                               FAR const uint8_t *value, size_t n)
{
  uint8_t msg[8];

  msg[0] = type | 0x02;
  msg[1] = (n << 1) | 0x01;
  memcpy(msg + 2, value, n);
  return mux_test_frame(buf, 0, 0xef, false, msg, n + 2);
}

static void mux_test_feed(FAR struct mux_test_s *t, FAR const void *buf,
                          size_t len)
{
  mux_input(&t->mux, buf, len);
}

/****************************************************************************
 * Name: test_mux_open
 *
 * Description:
 *   SABM and UA as 27.010 gives them, and the modem's answer opening the
 *   channel.
 *
 ****************************************************************************/

static void test_mux_open(void **state)
{
  static const uint8_t sabm0[] =
  {
    0xf9, 0x03, 0x3f, 0x01, 0x1c, 0xf9
  };

  static const uint8_t ua0[] =
  {
    0xf9, 0x03, 0x73, 0x01, 0xd7, 0xf9
  };

  struct mux_test_s t;

  mux_test_init(&t);

  /* Outside the multiplexer no channel opens */

  assert_int_equal(mux_open(&t.mux, 0), -ENOTCONN);

  mux_start(&t.mux);
  assert_int_equal(mux_open(&t.mux, 0), 0);
  assert_true(mux_waiting(&t.mux));
  assert_int_equal(mux_flush(&t.mux), 0);
  assert_false(mux_waiting(&t.mux));
  assert_int_equal(t.nout, sizeof(sabm0));
  assert_memory_equal(t.out, sabm0, sizeof(sabm0));

  /* The UA, with flags doubled before it */

  mux_test_feed(&t, "\xf9", 1);
  mux_test_feed(&t, ua0, sizeof(ua0));
  assert_int_equal(t.nevents, 1);
  assert_int_equal(t.events[0], MUX_OPENED);
  assert_int_equal(t.dlcis[0], 0);
  assert_true(t.mux.opened & 1);

  /* A second UA tells nothing new */

  mux_test_feed(&t, ua0, sizeof(ua0));
  assert_int_equal(t.nevents, 1);
}

/****************************************************************************
 * Name: test_mux_send
 *
 * Description:
 *   A channel's bytes in frames of 127 at most, whole, in order, through a
 *   UART that takes three bytes at a time; and the modem's back.
 *
 ****************************************************************************/

static void test_mux_send(void **state)
{
  struct mux_test_s t;
  uint8_t text[300];
  uint8_t frame[160];
  size_t off;
  size_t n;
  size_t i;
  int ret;

  mux_test_init(&t);
  t.chunk = 3;

  /* Plain AT: the bytes as they are, the AT channel's only */

  assert_int_equal(mux_send(&t.mux, MUX_AT, "AT\r", 3), 0);
  assert_int_equal(mux_send(&t.mux, MUX_DATA, "x", 1), -ENOTCONN);
  assert_int_equal(mux_flush(&t.mux), 0);
  assert_int_equal(t.nout, 3);
  assert_memory_equal(t.out, "AT\r", 3);

  /* In the multiplexer, only on a channel open */

  mux_start(&t.mux);
  assert_int_equal(mux_send(&t.mux, MUX_AT, "AT\r", 3), -ENOTCONN);
  n = mux_test_frame(frame, MUX_AT, 0x73, false, NULL, 0);
  mux_test_feed(&t, frame, n);
  assert_true(t.mux.opened & (1 << MUX_AT));

  for (i = 0; i < sizeof(text); i++)
    {
      text[i] = (uint8_t)i;
    }

  t.nout = 0;
  assert_int_equal(mux_send(&t.mux, MUX_AT, text, sizeof(text)), 0);

  /* Three bytes a call: every call but the last leaves bytes waiting */

  do
    {
      ret = mux_flush(&t.mux);
    }
  while (ret == -EAGAIN || mux_waiting(&t.mux));

  assert_int_equal(ret, 0);
  assert_int_equal(t.nout, sizeof(text) + 3 * 6);

  /* 127, 127 and 46 bytes, each a UIH frame of the AT channel's */

  off = 0;
  for (i = 0; i < 3; i++)
    {
      n = i < 2 ? 127 : 46;
      assert_int_equal(t.out[off], 0xf9);
      assert_int_equal(t.out[off + 1], (MUX_AT << 2) | 0x03);
      assert_int_equal(t.out[off + 2], 0xef);
      assert_int_equal(t.out[off + 3], (n << 1) | 1);
      assert_memory_equal(t.out + off + 4, text + i * 127, n);
      assert_int_equal(t.out[off + 4 + n],
                       mux_test_fcs(t.out + off + 1, 3));
      assert_int_equal(t.out[off + 5 + n], 0xf9);
      off += n + 6;
    }

  /* The modem's: a frame split across reads */

  n = mux_test_frame(frame, MUX_AT, 0xef, false, "\r\nOK\r\n", 6);
  mux_test_feed(&t, frame, 5);
  assert_int_equal(t.ndata, 0);
  mux_test_feed(&t, frame + 5, n - 5);
  assert_int_equal(t.ndata, 6);
  assert_int_equal(t.data_dlci, MUX_AT);
  assert_memory_equal(t.data, "\r\nOK\r\n", 6);

  /* One with a bad FCS is dropped, and the next read */

  n = mux_test_frame(frame, MUX_AT, 0xef, false, "BAD", 3);
  frame[n - 2] ^= 0x55;
  mux_test_feed(&t, frame, n);
  assert_int_equal(t.ndata, 6);
  assert_int_equal(t.mux.bad, 1);
  n = mux_test_frame(frame, MUX_AT, 0xef, false, "GOOD", 4);
  mux_test_feed(&t, frame, n);
  assert_int_equal(t.ndata, 10);
}

/****************************************************************************
 * Name: test_mux_room
 *
 * Description:
 *   A full queue refuses a send whole, and tells of room once the UART
 *   has taken it.
 *
 ****************************************************************************/

static void test_mux_room(void **state)
{
  struct mux_test_s t;
  uint8_t frame[16];
  uint8_t big[MUX_N1];
  size_t n;
  int sent = 0;

  mux_test_init(&t);
  mux_start(&t.mux);
  n = mux_test_frame(frame, MUX_AT, 0x73, false, NULL, 0);
  mux_test_feed(&t, frame, n);
  memset(big, 'a', sizeof(big));

  t.full = true;
  while (mux_room(&t.mux) >= sizeof(big))
    {
      assert_int_equal(mux_send(&t.mux, MUX_AT, big, sizeof(big)), 0);
      sent++;
    }

  assert_int_equal(sent, MUX_QUEUE / (MUX_N1 + 6));
  assert_int_equal(mux_send(&t.mux, MUX_AT, big, sizeof(big)), -EAGAIN);
  assert_int_equal(mux_flush(&t.mux), -EAGAIN);
  assert_int_equal(t.nevents, 1);             /* The UA's */

  t.full = false;
  assert_int_equal(mux_flush(&t.mux), 0);
  assert_int_equal(t.nevents, 2);
  assert_int_equal(t.events[1], MUX_ROOM);
  assert_int_equal(t.nout, sent * (MUX_N1 + 6));
}

/****************************************************************************
 * Name: test_mux_control
 *
 * Description:
 *   The modem's control messages: its signals and flow control answered,
 *   and its close-down.
 *
 ****************************************************************************/

static void test_mux_control(void **state)
{
  struct mux_test_s t;
  uint8_t frame[32];
  uint8_t value[2];
  size_t n;

  mux_test_init(&t);
  mux_start(&t.mux);
  n = mux_test_frame(frame, MUX_AT, 0x73, false, NULL, 0);
  mux_test_feed(&t, frame, n);
  t.nevents = 0;

  /* MSC for the AT channel with flow control: answered, and sends wait */

  value[0] = (MUX_AT << 2) | 0x03;
  value[1] = 0x8f;
  n = mux_test_control(frame, 0xe1, value, 2);
  mux_test_feed(&t, frame, n);
  assert_true(t.mux.held);
  assert_int_equal(mux_send(&t.mux, MUX_AT, "AT\r", 3), -EAGAIN);
  assert_int_equal(mux_room(&t.mux), 0);
  assert_int_equal(mux_flush(&t.mux), 0);
  assert_int_equal(t.out[4], 0xe1);           /* The answer: no C/R */
  assert_int_equal(t.out[6], value[0]);
  assert_int_equal(t.out[7], value[1]);

  /* Flow control on again: room told */

  t.nout = 0;
  n = mux_test_control(frame, 0xa1, NULL, 0);
  mux_test_feed(&t, frame, n);
  assert_false(t.mux.held);
  assert_int_equal(t.nevents, 1);
  assert_int_equal(t.events[0], MUX_ROOM);
  assert_int_equal(mux_send(&t.mux, MUX_AT, "AT\r", 3), 0);

  /* Close-down: answered, and the multiplexer is over */

  t.nevents = 0;
  n = mux_test_control(frame, 0xc1, NULL, 0);
  mux_test_feed(&t, frame, n);
  assert_false(t.mux.active);
  assert_int_equal(t.nevents, 1);
  assert_int_equal(t.events[0], MUX_ENDED);
}

/****************************************************************************
 * Name: test_mux_restart
 *
 * Description:
 *   A modem that restarted speaks plain AT: "RDY" between frames, even
 *   right after one, tells of it; the same inside a frame does not.
 *
 ****************************************************************************/

static void test_mux_restart(void **state)
{
  struct mux_test_s t;
  uint8_t frame[32];
  size_t n;

  mux_test_init(&t);
  mux_start(&t.mux);

  n = mux_test_frame(frame, MUX_AT, 0xef, false, "\r\nRDY\r\n", 7);
  mux_test_feed(&t, frame, n);
  assert_int_equal(t.nevents, 0);

  /* Right after a frame's flag, which may open the next */

  mux_test_feed(&t, "\r\nRDY\r\n", 7);
  assert_int_equal(t.nevents, 1);
  assert_int_equal(t.events[0], MUX_RESTARTED);

  /* Or later, with noise first */

  mux_test_feed(&t, "\x00\xff*ATREADY: 1\r\n", 15);
  assert_int_equal(t.nevents, 2);
  assert_int_equal(t.events[1], MUX_RESTARTED);

  /* Reset, the queue is dropped; a kick closes a multiplexer left from
   * before, and ends the close-down as a line in plain AT
   */

  assert_int_equal(mux_send(&t.mux, MUX_AT, "x", 1), -ENOTCONN);
  mux_reset(&t.mux);
  assert_false(mux_waiting(&t.mux));
  assert_int_equal(mux_kick(&t.mux), 0);
  assert_int_equal(mux_flush(&t.mux), 0);
  assert_int_equal(t.nout, 9);
  assert_int_equal(t.out[1], 0x03);
  assert_int_equal(t.out[2], 0xef);
  assert_int_equal(t.out[4], 0xc3);
  assert_int_equal(t.out[8], '\r');
  assert_false(t.mux.active);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_mux_open),
    cmocka_unit_test(test_mux_send),
    cmocka_unit_test(test_mux_room),
    cmocka_unit_test(test_mux_control),
    cmocka_unit_test(test_mux_restart),
  };

  return cmocka_run_group_tests_name("mux", tests, NULL, NULL);
}
