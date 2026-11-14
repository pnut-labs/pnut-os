/****************************************************************************
 * pnut-os/tests/unit/test_location.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Location (RFC 0035) against a fake u-blox receiver: it acknowledges the
 * configuration, dumps its orbit data when asked, and says what the test
 * makes it.  The receiver on only while asked; a fix answering a Current
 * and kept; the orbit data dumped, kept and given back with the time and
 * the last position; old input forgotten; positions rounded unless
 * precise; Follow at its interval.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "location.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOCATION_SERVICE "location"
#define FAKE_DBD         3          /* Entries the fake dumps */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct loct_s;
typedef CODE bool (*loct_step_t)(FAR struct loct_s *t);

struct loct_s
{
  FAR struct pnut_loop_s *loop;
  loct_step_t steps;
  int last;
  struct location_s loc;
  struct pnut_module_s module;
  struct pnut_location_client_s client;
  struct pnut_location_info_reader_s reader;
  struct pnut_gnss_time_reader_s gt_reader;
  FAR struct pnut_publisher_s *time_pub;
  char dir[64];
  int step;
  int ticks;
  bool answered;                  /* The last call's answer came */
  int status;
  uint32_t code;
  pnut_location_current_reply_t reply;
  pnut_location_info_t state;
  pnut_gnss_time_t gt;
  int gts;

  /* A Follow */

  uint16_t stream;
  int items;
  uint64_t item_ms[8];
  pnut_location_position_t item;

  /* The fake receiver: what it says goes into the pipe */

  int pipe[2];
  bool opened;
  int opens;
  bool greeted;                   /* Said its first word since on */
  int deaf;                       /* Configurations not acknowledged */
  bool stale;                     /* Old output waiting at open */
  struct ubx_parser_s parser;
  int valsets;
  int ini_time;
  int ini_time_acc_s;             /* The time's accuracy, given */
  int ini_pos;
  uint32_t ini_pos_acc;
  int dbd_in;
  uint8_t dbd_first[FAKE_DBD];
  int polls;
  bool hold_acks;                 /* Its acks of saved entries wait */
  int acks_held;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void fake_say(FAR struct loct_s *t, uint8_t cls, uint8_t id,
                     FAR const uint8_t *payload, uint16_t len)
{
  uint8_t buf[UBX_PAYLOAD_MAX + UBX_OVERHEAD];
  size_t n = ubx_build(buf, sizeof(buf), cls, id, payload, len);

  assert_int_equal(write(t->pipe[1], buf, n), (ssize_t)n);
}

static void put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* A NAV-PVT: a 3D fix of an accuracy (0 for none), its time resolved */

static void fake_pvt(FAR struct loct_s *t, uint32_t hacc_mm, int32_t lat,
                     int32_t lon)
{
  uint8_t p[92];

  memset(p, 0, sizeof(p));
  p[4]  = 0xea;                   /* 2026-11-13 12:34:56 */
  p[5]  = 0x07;
  p[6]  = 11;
  p[7]  = 13;
  p[8]  = 12;
  p[9]  = 34;
  p[10] = 56;
  p[11] = UBX_PVT_VALID_DATE | UBX_PVT_VALID_TIME | UBX_PVT_RESOLVED;
  put32(&p[12], 30);
  if (hacc_mm > 0)
    {
      p[20] = 3;
      p[21] = UBX_PVT_FIX_OK;
      p[23] = 9;
      put32(&p[24], (uint32_t)lon);
      put32(&p[28], (uint32_t)lat);
      put32(&p[36], 112000);
      put32(&p[40], hacc_mm);
      put32(&p[60], 1400);
    }
  else
    {
      p[23] = 2;
    }

  fake_say(t, UBX_CLASS_NAV, UBX_NAV_PVT, p, sizeof(p));
}

/* The receiver's acknowledgement of a saved entry given back: the same
 * message as a dump's end, msgPayloadStart the entry's first bytes
 */

static void fake_ack_dbd(FAR struct loct_s *t)
{
  uint8_t ack[8];

  memset(ack, 0, sizeof(ack));
  ack[0] = 1;
  ack[3] = UBX_MGA_DBD;
  fake_say(t, UBX_CLASS_MGA, UBX_MGA_ACK, ack, sizeof(ack));
}

static int fake_open(FAR void *arg, FAR const char *path)
{
  FAR struct loct_s *t = arg;

  assert_string_equal(path, "/dev/fake_gnss");
  assert_false(t->opened);
  t->opened  = true;
  t->greeted = false;
  t->opens++;
  ubx_parser_init(&t->parser);
  if (t->stale)
    {
      fake_pvt(t, 5000, 100, 200);
    }

  return t->pipe[0];
}

static void fake_close(FAR void *arg)
{
  FAR struct loct_s *t = arg;
  uint8_t c;

  assert_true(t->opened);
  t->opened = false;
  while (read(t->pipe[0], &c, 1) == 1);
}

static ssize_t fake_read(FAR void *arg, FAR uint8_t *buf, size_t len)
{
  FAR struct loct_s *t = arg;
  ssize_t n = read(t->pipe[0], buf, len);

  return n < 0 ? 0 : n;
}

/* What Location says to the receiver: configuration acknowledged, the
 * time, a position and orbit data counted, a dump answered
 */

static ssize_t fake_write(FAR void *arg, FAR const uint8_t *buf,
                          size_t len)
{
  FAR struct loct_s *t = arg;
  FAR const struct ubx_frame_s *f = &t->parser.frame;
  uint8_t ack[2];
  uint8_t entry[20];
  uint8_t end[8];
  size_t i;
  int k;

  assert_true(t->opened);
  for (i = 0; i < len; i++)
    {
      if (!ubx_parse(&t->parser, buf[i]))
        {
          continue;
        }

      if (f->cls == UBX_CLASS_CFG && f->id == UBX_CFG_VALSET)
        {
          t->valsets++;
          if (t->deaf > 0)
            {
              t->deaf--;
              continue;
            }

          ack[0] = f->cls;
          ack[1] = f->id;
          fake_say(t, UBX_CLASS_ACK, UBX_ACK_ACK, ack, 2);
        }
      else if (f->cls == UBX_CLASS_MGA && f->id == UBX_MGA_INI &&
               f->payload[0] == 0x10)
        {
          t->ini_time++;
          t->ini_time_acc_s = f->payload[16] | (f->payload[17] << 8);
        }
      else if (f->cls == UBX_CLASS_MGA && f->id == UBX_MGA_INI &&
               f->payload[0] == 0x01)
        {
          assert_int_equal(t->ini_time, 1);     /* The time first */
          t->ini_pos++;
          t->ini_pos_acc = f->payload[16] | (f->payload[17] << 8) |
                           (f->payload[18] << 16) | (f->payload[19] << 24);
        }
      else if (f->cls == UBX_CLASS_MGA && f->id == UBX_MGA_DBD &&
               f->len == 0)
        {
          /* Acks of saved entries come late, among the dump: they end
           * nothing
           */

          t->polls++;
          while (t->acks_held > 0)
            {
              fake_ack_dbd(t);
              t->acks_held--;
            }

          for (k = 0; k < FAKE_DBD; k++)
            {
              memset(entry, 0, sizeof(entry));
              entry[12] = (uint8_t)(0x40 + k);
              fake_say(t, UBX_CLASS_MGA, UBX_MGA_DBD, entry, sizeof(entry));
            }

          memset(end, 0, sizeof(end));
          end[0] = 1;
          end[3] = UBX_MGA_DBD;
          put32(&end[4], FAKE_DBD);
          fake_say(t, UBX_CLASS_MGA, UBX_MGA_ACK, end, sizeof(end));
        }
      else if (f->cls == UBX_CLASS_MGA && f->id == UBX_MGA_DBD)
        {
          assert_int_equal(t->ini_time, 1);
          assert_true(t->dbd_in < FAKE_DBD);
          t->dbd_first[t->dbd_in++] = f->payload[12];
          if (t->hold_acks)
            {
              t->acks_held++;
            }
          else
            {
              fake_ack_dbd(t);
            }
        }
    }

  return len;
}

static const struct location_dev_ops_s g_fake =
{
  fake_open,
  fake_close,
  fake_read,
  fake_write
};

/* Calls */

static void loct_answered(FAR struct pnut_location_client_s *client,
                          int status,
                          FAR const pnut_location_current_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct loct_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  if (out != NULL)
    {
      t->reply = *out;
    }

  t->answered = true;
}

static void loct_current(FAR struct loct_s *t, uint32_t max_age,
                         uint32_t accuracy, uint32_t timeout, bool precise)
{
  pnut_location_current_request_t req;

  memset(&req, 0, sizeof(req));
  memset(&t->reply, 0, sizeof(t->reply));
  req.max_age_s  = max_age;
  req.accuracy_m = accuracy;
  req.timeout_s  = timeout;
  req.precise    = precise;
  t->answered    = false;
  assert_int_equal(pnut_location_current(&t->client, &req, 30000,
                                         loct_answered, t), 0);
}

static void loct_opened(FAR struct pnut_location_client_s *client,
                        int status, FAR const pnut_error_t *err,
                        FAR void *arg)
{
  FAR struct loct_s *t = arg;

  t->status   = status;
  t->answered = true;
}

static void loct_item(FAR struct pnut_location_client_s *client, int status,
                      FAR const pnut_location_position_t *item,
                      FAR void *arg)
{
  FAR struct loct_s *t = arg;
  struct timespec ts;

  if (item == NULL)
    {
      return;
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  if (t->items < 8)
    {
      t->item_ms[t->items] = (uint64_t)ts.tv_sec * 1000 +
                             ts.tv_nsec / 1000000;
    }

  t->item = *item;
  t->items++;
  pnut_location_follow_credit(client, t->stream, 1);
}

static void loct_time_source(FAR struct loct_s *t, int source)
{
  pnut_time_change_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.source = source;
  msg.seq    = 1;
  assert_int_equal(pnut_time_change_publish(t->time_pub, &msg), 0);
}

/* A state file as Location writes it: kept a minute ago, a fix of 10 m
 * then, and the fake's FAKE_DBD orbit data entries; damaged, the second
 * says it is longer than the receiver's queue, yet within the file
 * (padding follows)
 */

static void loct_state(FAR struct loct_s *t, bool damaged)
{
  uint8_t buf[44 + FAKE_DBD * 28 + 1200];
  uint8_t entry[20];
  char path[96];
  int64_t now = time(NULL);
  size_t n = 44;
  FILE *f;
  int k;

  memset(buf, 0, sizeof(buf));
  put32(&buf[0], 0x434c4e50);
  buf[4] = 1;
  buf[5] = 1;
  put32(&buf[8], (uint32_t)(now - 60));
  put32(&buf[12], (uint32_t)((now - 60) >> 32));
  put32(&buf[16], 522297000);
  put32(&buf[20], 210122000);
  put32(&buf[24], 110000);
  put32(&buf[28], 10000);
  put32(&buf[32], (uint32_t)(now - 60));
  put32(&buf[36], (uint32_t)((now - 60) >> 32));
  for (k = 0; k < FAKE_DBD; k++)
    {
      memset(entry, 0, sizeof(entry));
      entry[12] = (uint8_t)(0x40 + k);
      n += ubx_build(&buf[n], sizeof(buf) - n, UBX_CLASS_MGA, UBX_MGA_DBD,
                     entry, sizeof(entry));
    }

  if (damaged)
    {
      buf[44 + 28 + 4] = 1100 & 0xff;
      buf[44 + 28 + 5] = 1100 >> 8;
      n += 1200;
    }

  put32(&buf[40], (uint32_t)(n - 44));
  snprintf(path, sizeof(path), "%s/state", t->dir);
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buf, 1, n, f), n);
  fclose(f);
}

/* The state file's orbit data, in bytes; -1 for none */

static long loct_saved(FAR struct loct_s *t)
{
  uint8_t head[44];
  char path[96];
  FILE *f;
  long n;

  snprintf(path, sizeof(path), "%s/state", t->dir);
  f = fopen(path, "rb");
  if (f == NULL)
    {
      return -1;
    }

  n = fread(head, 1, sizeof(head), f) == sizeof(head) ?
      (long)(head[40] | (head[41] << 8) | (head[42] << 16)) : -1;
  fclose(f);
  return n;
}

/****************************************************************************
 * Name: loct_fix
 *
 * Description:
 *   A Current for 50 m: the receiver switched on and configured; no fix,
 *   then one of 80 m, do not answer; one of 20 m does, precisely.  The
 *   topic says on and fixed, never where; Time gets the satellites' time.
 *   Nobody asking, the receiver dumps its orbit data, which is kept, and
 *   is switched off.  A Current that a recent fix answers leaves it off.
 *
 ****************************************************************************/

static bool loct_fix(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        assert_false(t->state.on);
        loct_current(t, 0, 50, 10, true);
        break;

      case 1:
        if (!t->opened || t->valsets < 1 || !t->state.on)
          {
            return false;
          }

        fake_pvt(t, 0, 0, 0);
        fake_pvt(t, 80000, 522297001, 210122001);
        break;

      case 2:                     /* Not yet: 80 m */
        if (t->ticks < 3)
          {
            return false;
          }

        assert_true(t->state.fix);
        assert_int_equal(t->state.satellites, 9);
        assert_false(t->answered);
        fake_pvt(t, 20000, 522297002, 210122002);
        break;

      case 3:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->reply.fresh);
        assert_int_equal(t->reply.position.lat, 522297002);
        assert_int_equal(t->reply.position.lon, 210122002);
        assert_int_equal(t->reply.position.accuracy_mm, 20000);
        assert_int_equal(t->reply.position.alt_mm, 112000);
        assert_int_equal(t->reply.position.utc, 1794573296);
        assert_false(t->reply.position.approximate);
        assert_true(t->gts >= 1);
        assert_int_equal(t->gt.utc, 1794573296);
        break;

      case 4:                     /* Dumped, kept, off */
        if (t->opened || t->state.on)
          {
            return false;
          }

        assert_int_equal(t->polls, 1);
        assert_int_equal(loct_saved(t), FAKE_DBD * 28);
        loct_current(t, 60, 0, 10, true);
        break;

      case 5:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->reply.fresh);
        assert_int_equal(t->reply.position.lat, 522297002);
        assert_false(t->opened);
        assert_int_equal(t->opens, 1);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_restore
 *
 * Description:
 *   With the clock from NTP, the receiver is given the time, the last
 *   position strayed by a minute at 30 m/s, and the kept orbit data, in
 *   that order.  Its acks of the orbit data late, the dump waits for them
 *   (they are the same message as its end) a while, then goes on, and
 *   the acks coming among it end nothing.  With a clock of no source,
 *   nothing.
 *
 ****************************************************************************/

static bool loct_restore(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        loct_state(t, false);
        loct_time_source(t, PNUT_TIME_INFO_SOURCE_NTP);
        t->hold_acks = true;
        loct_current(t, 0, 0, 10, true);
        break;

      case 1:
        if (t->dbd_in < FAKE_DBD)
          {
            return false;
          }

        assert_int_equal(t->ini_time, 1);
        assert_int_equal(t->ini_time_acc_s, 10);  /* NTP's, drifted */
        assert_int_equal(t->ini_pos, 1);
        assert_true(t->ini_pos_acc >= (10000 + 60 * 30000) / 10);
        assert_true(t->ini_pos_acc <= (10000 + 62 * 30000) / 10);
        assert_int_equal(t->dbd_first[0], 0x40);
        assert_int_equal(t->dbd_first[2], 0x42);
        fake_pvt(t, 5000, 1, 2);
        break;

      case 2:                     /* Answered; no dump while acks wait */
        if (!t->answered || t->ticks < 60)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->opened);
        assert_int_equal(t->polls, 0);
        assert_int_equal(t->acks_held, FAKE_DBD);
        break;

      case 3:                     /* Dumped whole, off */
        if (t->opened)
          {
            return false;
          }

        assert_int_equal(t->polls, 1);
        assert_int_equal(t->acks_held, 0);
        t->hold_acks = false;
        assert_int_equal(loct_saved(t), FAKE_DBD * 28);

        loct_time_source(t, PNUT_TIME_INFO_SOURCE_SAVED);
        t->ini_time = 0;
        t->ini_pos  = 0;
        t->dbd_in   = 0;
        loct_current(t, 0, 0, 10, true);
        break;

      case 4:
        if (!t->opened || t->ticks < 4)
          {
            return false;
          }

        assert_int_equal(t->ini_time, 0);
        assert_int_equal(t->ini_pos, 0);
        assert_int_equal(t->dbd_in, 0);
        fake_pvt(t, 5000, 1, 2);
        break;

      case 5:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_damaged
 *
 * Description:
 *   Saved orbit data with a frame longer than any: the frames before it
 *   are given, the rest left, and the receiver still goes off.  The
 *   network's time is given as less accurate than NTP's.
 *
 ****************************************************************************/

static bool loct_damaged(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        loct_state(t, true);
        loct_time_source(t, PNUT_TIME_INFO_SOURCE_NETWORK);
        loct_current(t, 0, 0, 10, true);
        break;

      case 1:
        if (t->dbd_in < 1 || t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->dbd_in, 1);
        assert_int_equal(t->dbd_first[0], 0x40);
        assert_int_equal(t->ini_time_acc_s, 60);  /* The network's */
        fake_pvt(t, 5000, 1, 2);
        break;

      case 2:
        if (!t->answered || t->opened)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->polls, 1);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_stale
 *
 * Description:
 *   What the receiver said before it was switched off, still in the
 *   driver, a fix among it, answers nobody: with no position in time, the
 *   Current fails.
 *
 ****************************************************************************/

static bool loct_stale(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        t->stale = true;
        loct_current(t, 0, 0, 1, true);
        break;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_TIMEOUT);
        assert_int_equal(t->code, PNUT_LOCATION_ERROR_CODE_NO_POSITION);
        assert_int_equal(t->opens, 1);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_round
 *
 * Description:
 *   An approximate position: on the grid, the grid's accuracy, no
 *   altitude or speed; an unfresh answer is the best known.
 *
 ****************************************************************************/

static bool loct_round(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        loct_current(t, 0, 0, 10, false);
        break;

      case 1:
        if (!t->opened || t->valsets < 1)
          {
            return false;
          }

        fake_pvt(t, 3000, 522297000, -38512345);
        break;

      case 2:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->reply.position.approximate);
        assert_int_equal(t->reply.position.lat, 522500000);
        assert_int_equal(t->reply.position.lon, -38500000);
        assert_int_equal(t->reply.position.accuracy_mm, LOCATION_GRID_MM);
        assert_int_equal(t->reply.position.alt_mm, 0);
        assert_int_equal(t->reply.position.speed_mms, 0);
        loct_current(t, 0, 1, 1, true);
        break;

      case 3:                     /* 1 m never comes: the best known */
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_false(t->reply.fresh);
        assert_int_equal(t->reply.position.lat, 522297000);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_follow
 *
 * Description:
 *   Follow every 2 s with a fix each second: positions 2 s apart; ended,
 *   the receiver goes off.
 *
 ****************************************************************************/

static bool loct_follow(FAR struct loct_s *t)
{
  pnut_location_follow_request_t req;

  switch (t->step)
    {
      case 0:
        memset(&req, 0, sizeof(req));
        req.interval_s = 2;
        req.precise    = true;
        t->answered    = false;
        assert_int_equal(pnut_location_follow(&t->client, &req, 1, 30000,
                                              loct_opened, loct_item, t,
                                              &t->stream), 0);
        break;

      case 1:
        if (!t->answered || !t->opened)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        fake_pvt(t, 3000, 10, 20);
        break;

      default:                    /* A fix each 0.98 s, for 5 s */
        if (t->step < 7)
          {
            if (t->ticks < 49)
              {
                return false;
              }

            fake_pvt(t, 3000, 10 + t->step, 20);
            break;
          }

        if (t->step == 7)
          {
            /* An epoch a little early still makes the next item */

            assert_int_equal(t->items, 3);
            assert_true(t->item_ms[1] - t->item_ms[0] >= 1900);
            assert_int_equal(pnut_location_follow_cancel(&t->client,
                                                         t->stream), 0);
            break;
          }

        if (t->opened)
          {
            return false;
          }

        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_deaf
 *
 * Description:
 *   A receiver that misses its configuration twice is sent it again,
 *   then answers.
 *
 ****************************************************************************/

static bool loct_deaf(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        t->deaf = 2;
        loct_current(t, 0, 0, 10, true);
        break;

      case 1:
        if (t->valsets < 3 || t->deaf > 0)
          {
            return false;
          }

        fake_pvt(t, 4000, 3, 4);
        break;

      case 2:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->valsets, 3);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_none
 ****************************************************************************/

static bool loct_none(FAR struct loct_s *t)
{
  switch (t->step)
    {
      case 0:
        loct_current(t, 0, 0, 1, true);
        break;

      case 1:
        if (!t->answered)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_LOCATION_ERROR_CODE_NO_RECEIVER);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: loct_run
 ****************************************************************************/

static void loct_tick(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct loct_s *t = arg;
  static const char hello[] = "$GNTXT,01,01,02,u-blox AG*4C\r\n";

  /* The receiver up a tick after it is switched on */

  if (t->opened && !t->greeted)
    {
      t->greeted = true;
      assert_int_equal(write(t->pipe[1], hello, sizeof(hello) - 1),
                       sizeof(hello) - 1);
    }

  t->ticks++;
  if (t->step > t->last || !t->steps(t))
    {
      return;
    }

  t->ticks = 0;
  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void loct_state_cb(FAR struct pnut_location_info_reader_s *reader,
                          FAR const pnut_location_info_t *msg, FAR void *arg)
{
  FAR struct loct_s *t = arg;

  t->state = *msg;
}

static void loct_gt(FAR struct pnut_gnss_time_reader_s *reader,
                    FAR const pnut_gnss_time_t *msg, FAR void *arg)
{
  FAR struct loct_s *t = arg;

  t->gt = *msg;
  t->gts++;
}

static int loct_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct loct_s *t = (FAR struct loct_s *)
    ((FAR uint8_t *)module - offsetof(struct loct_s, module));

  assert_int_equal(pnut_location_info_subscribe(loop, loct_state_cb, t,
                                                &t->reader), 0);
  assert_int_equal(pnut_gnss_time_subscribe(loop, loct_gt, t,
                                            &t->gt_reader), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &t->time_pub), 0);
  assert_int_equal(pnut_location_connect(loop, LOCATION_SERVICE, NULL,
                                         NULL, &t->client), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, loct_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void loct_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct loct_s *t = (FAR struct loct_s *)
    ((FAR uint8_t *)module - offsetof(struct loct_s, module));

  pnut_location_info_unsubscribe(&t->reader);
  pnut_gnss_time_unsubscribe(&t->gt_reader);
  pnut_topic_unadvertise(t->time_pub);
  pnut_location_disconnect(&t->client);
}

static void loct_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct loct_s *t = arg;

  print_error("Stuck at step %d (answered %d, opened %d, on %d)\n",
              t->step, t->answered, t->opened, t->state.on);
  pnut_loop_stop(loop, -ETIMEDOUT);
}

static void loct_run(loct_step_t steps, int last, bool receiver)
{
  FAR struct loct_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct location_config_s lconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  t->steps = steps;
  t->last  = last;
  assert_int_equal(pipe(t->pipe), 0);
  assert_int_equal(fcntl(t->pipe[0], F_SETFL, O_NONBLOCK), 0);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-location-%d", (int)getpid());
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir  = t->dir;
  config.workers = 1;
  config.jobs    = 2;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&lconfig, 0, sizeof(lconfig));
  lconfig.ops       = &g_fake;
  lconfig.arg       = t;
  lconfig.device    = receiver ? "/dev/fake_gnss" : "";
  lconfig.chip      = "MIA-M10Q";
  lconfig.dir       = t->dir;
  lconfig.linger_ms = 300;
  location_init(&t->loc, &lconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->loc.module), 0);

  t->module.name  = "test";
  t->module.start = loct_start;
  t->module.stop  = loct_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 15000, 0, loct_timeout, t, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  location_deinit(&t->loc);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);
  assert_false(t->opened);
  close(t->pipe[0]);
  close(t->pipe[1]);
  assert_int_equal(system(cmd), 0);
  free(t);
}

static void test_location_fix(void **state)
{
  loct_run(loct_fix, 5, true);
}

static void test_location_restore(void **state)
{
  loct_run(loct_restore, 5, true);
}

static void test_location_damaged(void **state)
{
  loct_run(loct_damaged, 2, true);
}

static void test_location_stale(void **state)
{
  loct_run(loct_stale, 1, true);
}

static void test_location_round(void **state)
{
  loct_run(loct_round, 3, true);
}

static void test_location_follow(void **state)
{
  loct_run(loct_follow, 8, true);
}

static void test_location_deaf(void **state)
{
  loct_run(loct_deaf, 2, true);
}

static void test_location_none(void **state)
{
  loct_run(loct_none, 1, false);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_location_fix),
    cmocka_unit_test(test_location_restore),
    cmocka_unit_test(test_location_damaged),
    cmocka_unit_test(test_location_stale),
    cmocka_unit_test(test_location_round),
    cmocka_unit_test(test_location_follow),
    cmocka_unit_test(test_location_deaf),
    cmocka_unit_test(test_location_none),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
