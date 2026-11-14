/****************************************************************************
 * pnut-os/tests/unit/test_ubx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Location's UBX codec (RFC 0035): frames built and parsed among NMEA and
 * noise, the checksum against the interface description's example,
 * CFG-VALSET's encoding by key size, NAV-PVT, MGA-INI and the acks.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "ubx.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Feed bytes; the number of frames they complete, the last in the parser */

static int feed(FAR struct ubx_parser_s *p, FAR const uint8_t *data,
                size_t len)
{
  int frames = 0;
  size_t i;

  for (i = 0; i < len; i++)
    {
      frames += ubx_parse(p, data[i]);
    }

  return frames;
}

static void put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/****************************************************************************
 * Name: test_ubx_checksum
 *
 * Description:
 *   UBX-CFG-MSG with payload f0 01 00 ends in fb 11.
 *
 ****************************************************************************/

static void test_ubx_checksum(void **state)
{
  static const uint8_t payload[] =
  {
    0xf0, 0x01, 0x00
  };

  static const uint8_t frame[] =
  {
    0xb5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xf0, 0x01, 0x00, 0xfb, 0x11
  };

  uint8_t buf[16];

  assert_int_equal(ubx_build(buf, sizeof(buf), 0x06, 0x01, payload, 3),
                   11);
  assert_memory_equal(buf, frame, sizeof(frame));
  assert_int_equal(ubx_build(buf, 10, 0x06, 0x01, payload, 3), 0);
}

/****************************************************************************
 * Name: test_ubx_parse
 *
 * Description:
 *   A frame among an NMEA sentence and noise; one with a wrong checksum
 *   counted and dropped; one too long, or a length from noise, counted,
 *   the next found; a frame of no payload.
 *
 ****************************************************************************/

static void test_ubx_parse(void **state)
{
  static const char nmea[] = "$GNGGA,,,,,,0,00,99.99,,,,,,*56\r\n";
  static const uint8_t payload[] =
  {
    0x06, 0x8a
  };

  struct ubx_parser_s p;
  uint8_t buf[400];
  uint8_t big[300];
  size_t n;

  ubx_parser_init(&p);
  assert_int_equal(feed(&p, (FAR const uint8_t *)nmea, strlen(nmea)), 0);
  assert_int_equal(feed(&p, (FAR const uint8_t *)"\xb5\xb5", 2), 0);

  n = ubx_build(buf, sizeof(buf), UBX_CLASS_ACK, UBX_ACK_ACK, payload, 2);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(p.frame.cls, UBX_CLASS_ACK);
  assert_int_equal(p.frame.id, UBX_ACK_ACK);
  assert_int_equal(p.frame.len, 2);
  assert_memory_equal(p.frame.payload, payload, 2);

  buf[n - 1] ^= 0xff;
  assert_int_equal(feed(&p, buf, n), 0);
  assert_int_equal(p.errors, 1);

  memset(big, 0x24, sizeof(big));                 /* '$' among them */
  n = ubx_build(buf, sizeof(buf), UBX_CLASS_NAV, 0x35, big, sizeof(big));
  assert_int_equal(feed(&p, buf, n), 0);
  assert_int_equal(p.errors, 2);

  /* A length from noise: the frame right after it is still found */

  assert_int_equal(feed(&p, (FAR const uint8_t *)"\xb5\x62\x01\x07\xff\xff",
                        6), 0);
  assert_int_equal(p.errors, 3);
  n = ubx_build(buf, sizeof(buf), UBX_CLASS_ACK, UBX_ACK_ACK, payload, 2);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(p.frame.cls, UBX_CLASS_ACK);

  n = ubx_build(buf, sizeof(buf), UBX_CLASS_MGA, UBX_MGA_DBD, NULL, 0);
  assert_int_equal(n, 8);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(p.frame.cls, UBX_CLASS_MGA);
  assert_int_equal(p.frame.len, 0);
}

/****************************************************************************
 * Name: test_ubx_valset
 *
 * Description:
 *   Each value as wide as its key says: L and U1 a byte, U2 two.
 *
 ****************************************************************************/

static void test_ubx_valset(void **state)
{
  static const struct ubx_item_s items[] =
  {
    { UBX_KEY_UART1OUTPROT_NMEA, 0 },
    { UBX_KEY_RATE_MEAS, 1000 },
    { UBX_KEY_MSGOUT_NAV_PVT_UART1, 1 }
  };

  static const uint8_t payload[] =
  {
    0x00, 0x01, 0x00, 0x00,
    0x02, 0x00, 0x74, 0x10, 0x00,
    0x01, 0x00, 0x21, 0x30, 0xe8, 0x03,
    0x07, 0x00, 0x91, 0x20, 0x01
  };

  struct ubx_item_s bad =
  {
    0x50000001, 0
  };

  struct ubx_parser_s p;
  uint8_t buf[64];
  size_t n;

  n = ubx_valset(buf, sizeof(buf), items, 3);
  assert_int_equal(n, sizeof(payload) + UBX_OVERHEAD);
  ubx_parser_init(&p);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(p.frame.cls, UBX_CLASS_CFG);
  assert_int_equal(p.frame.id, UBX_CFG_VALSET);
  assert_memory_equal(p.frame.payload, payload, sizeof(payload));

  assert_int_equal(ubx_valset(buf, sizeof(buf), &bad, 1), 0);
}

/****************************************************************************
 * Name: test_ubx_pvt
 *
 * Description:
 *   A solution decoded field by field; its UTC from its date and time,
 *   given only when both are valid and resolved.
 *
 ****************************************************************************/

static void test_ubx_pvt(void **state)
{
  struct ubx_frame_s f;
  struct ubx_pvt_s pvt;
  int64_t utc;

  memset(&f, 0, sizeof(f));
  f.cls = UBX_CLASS_NAV;
  f.id  = UBX_NAV_PVT;
  f.len = 92;
  put32(&f.payload[0], 475000);
  f.payload[4]  = 0xea;                           /* 2026 */
  f.payload[5]  = 0x07;
  f.payload[6]  = 11;
  f.payload[7]  = 13;
  f.payload[8]  = 12;
  f.payload[9]  = 34;
  f.payload[10] = 56;
  f.payload[11] = UBX_PVT_VALID_DATE | UBX_PVT_VALID_TIME | UBX_PVT_RESOLVED;
  put32(&f.payload[12], 25);
  f.payload[20] = 3;
  f.payload[21] = UBX_PVT_FIX_OK;
  f.payload[23] = 11;
  put32(&f.payload[24], 210122000);               /* 21.0122 E */
  put32(&f.payload[28], (uint32_t)-338688000);    /* 33.8688 S */
  put32(&f.payload[36], 112000);
  put32(&f.payload[40], 3500);
  put32(&f.payload[60], 1400);
  put32(&f.payload[64], 27000000);

  assert_true(ubx_nav_pvt(&f, &pvt));
  assert_int_equal(pvt.year, 2026);
  assert_int_equal(pvt.fix, 3);
  assert_int_equal(pvt.numsv, 11);
  assert_int_equal(pvt.lon, 210122000);
  assert_int_equal(pvt.lat, -338688000);
  assert_int_equal(pvt.hmsl_mm, 112000);
  assert_int_equal(pvt.hacc_mm, 3500);
  assert_int_equal(pvt.gspeed_mms, 1400);
  assert_int_equal(pvt.head_e5, 27000000);
  assert_true(ubx_pvt_utc(&pvt, &utc));
  assert_int_equal(utc, 1794573296);

  pvt.valid &= ~UBX_PVT_RESOLVED;
  assert_false(ubx_pvt_utc(&pvt, &utc));

  pvt.valid = UBX_PVT_VALID_DATE | UBX_PVT_VALID_TIME | UBX_PVT_RESOLVED;
  pvt.year  = 2024;
  pvt.month = 2;
  pvt.day   = 29;
  pvt.hour  = 23;
  pvt.min   = 59;
  pvt.sec   = 59;
  assert_true(ubx_pvt_utc(&pvt, &utc));
  assert_int_equal(utc, 1709251199);

  f.len = 91;
  assert_false(ubx_nav_pvt(&f, &pvt));
  f.len = 92;
  f.id  = 0x03;
  assert_false(ubx_nav_pvt(&f, &pvt));
}

/****************************************************************************
 * Name: test_ubx_ini
 *
 * Description:
 *   MGA-INI-TIME_UTC carries the date and time of a UTC second, leap
 *   seconds unknown, the accuracy split in seconds and nanoseconds;
 *   MGA-INI-POS_LLH the position as given.
 *
 ****************************************************************************/

static void test_ubx_ini(void **state)
{
  struct ubx_parser_s p;
  FAR const uint8_t *q;
  uint8_t buf[64];
  size_t n;

  n = ubx_ini_time(buf, sizeof(buf), 1794573296, 250000000, 2500);
  ubx_parser_init(&p);
  assert_int_equal(feed(&p, buf, n), 1);
  q = p.frame.payload;
  assert_int_equal(p.frame.cls, UBX_CLASS_MGA);
  assert_int_equal(p.frame.id, UBX_MGA_INI);
  assert_int_equal(p.frame.len, 24);
  assert_int_equal(q[0], 0x10);
  assert_int_equal(q[3], 0x80);
  assert_int_equal(q[4] | (q[5] << 8), 2026);
  assert_int_equal(q[6], 11);
  assert_int_equal(q[7], 13);
  assert_int_equal(q[8], 12);
  assert_int_equal(q[9], 34);
  assert_int_equal(q[10], 56);
  assert_int_equal(q[12] | (q[13] << 8) | (q[14] << 16) | (q[15] << 24),
                   250000000);
  assert_int_equal(q[16] | (q[17] << 8), 2);
  assert_int_equal(q[20] | (q[21] << 8) | (q[22] << 16) | (q[23] << 24),
                   500000000);

  n = ubx_ini_time(buf, sizeof(buf), 946684800, 0, 1000);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(q[4] | (q[5] << 8), 2000);
  assert_int_equal(q[6], 1);
  assert_int_equal(q[7], 1);
  assert_int_equal(q[8], 0);

  n = ubx_ini_pos(buf, sizeof(buf), 522297000, 210122000, 11000, 500000);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_int_equal(p.frame.len, 20);
  assert_int_equal(q[0], 0x01);
  assert_int_equal(q[4] | (q[5] << 8) | (q[6] << 16) | (q[7] << 24),
                   522297000);
  assert_int_equal(q[16] | (q[17] << 8) | (q[18] << 16) | (q[19] << 24),
                   500000);
}

/****************************************************************************
 * Name: test_ubx_acks
 *
 * Description:
 *   ACK-NAK names the message it refuses; MGA-ACK after a database dump
 *   gives how many entries were sent.
 *
 ****************************************************************************/

static void test_ubx_acks(void **state)
{
  static const uint8_t nak[] =
  {
    0x06, 0x8a
  };

  uint8_t mga[8] =
  {
    0x01, 0x00, 0x00, 0x80, 0, 0, 0, 0
  };

  struct ubx_parser_s p;
  uint8_t buf[32];
  uint32_t start;
  uint8_t info;
  uint8_t msgid;
  uint8_t cls;
  uint8_t id;
  bool ok;
  size_t n;

  ubx_parser_init(&p);
  n = ubx_build(buf, sizeof(buf), UBX_CLASS_ACK, UBX_ACK_NAK, nak, 2);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_true(ubx_ack(&p.frame, &cls, &id, &ok));
  assert_false(ok);
  assert_int_equal(cls, UBX_CLASS_CFG);
  assert_int_equal(id, UBX_CFG_VALSET);
  assert_false(ubx_mga_ack(&p.frame, &ok, &info, &msgid, &start));

  put32(&mga[4], 117);
  n = ubx_build(buf, sizeof(buf), UBX_CLASS_MGA, UBX_MGA_ACK, mga, 8);
  assert_int_equal(feed(&p, buf, n), 1);
  assert_true(ubx_mga_ack(&p.frame, &ok, &info, &msgid, &start));
  assert_true(ok);
  assert_int_equal(info, 0);
  assert_int_equal(msgid, UBX_MGA_DBD);
  assert_int_equal(start, 117);
  assert_false(ubx_ack(&p.frame, &cls, &id, &ok));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_ubx_checksum),
    cmocka_unit_test(test_ubx_parse),
    cmocka_unit_test(test_ubx_valset),
    cmocka_unit_test(test_ubx_pvt),
    cmocka_unit_test(test_ubx_ini),
    cmocka_unit_test(test_ubx_acks),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
