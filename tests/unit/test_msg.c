/****************************************************************************
 * pnut-os/tests/unit/test_msg.c
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

#include <cmocka.h>

#include <pnut/msg.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void test_msg_roundtrip(void **state)
{
  struct pnut_msghdr_s in;
  struct pnut_msghdr_s out;
  uint8_t buf[PNUT_MSG_HEADER];

  memset(&in, 0, sizeof(in));
  in.version  = PNUT_MSG_VERSION;
  in.kind     = PNUT_KIND_REPLY;
  in.iface    = 0x1234;
  in.method   = 0xabcd;
  in.mversion = 3;
  in.status   = PNUT_STATUS_DENIED;
  in.seq      = 0xfffe;
  in.len      = PNUT_MSG_PAYLOAD_MAX;
  in.app      = 0x0102;

  pnut_msg_encode(&in, buf);

  /* Little-endian, field by field (RFC 0023) */

  assert_int_equal(buf[0], 1);
  assert_int_equal(buf[1], PNUT_KIND_REPLY);
  assert_int_equal(buf[2], 0x34);
  assert_int_equal(buf[3], 0x12);
  assert_int_equal(buf[4], 0xcd);
  assert_int_equal(buf[5], 0xab);
  assert_int_equal(buf[6], 3);
  assert_int_equal(buf[7], PNUT_STATUS_DENIED);
  assert_int_equal(buf[8], 0xfe);
  assert_int_equal(buf[9], 0xff);
  assert_int_equal(buf[12], 0x02);
  assert_int_equal(buf[13], 0x01);

  assert_int_equal(pnut_msg_decode(buf, &out), 0);
  assert_memory_equal(&in, &out, sizeof(in));
}

static void test_msg_rejects(void **state)
{
  struct pnut_msghdr_s hdr;
  uint8_t buf[PNUT_MSG_HEADER];

  memset(&hdr, 0, sizeof(hdr));
  hdr.version = PNUT_MSG_VERSION;
  hdr.kind    = PNUT_KIND_REQUEST;

  hdr.len = PNUT_MSG_PAYLOAD_MAX + 1;
  pnut_msg_encode(&hdr, buf);
  assert_int_equal(pnut_msg_decode(buf, &hdr), -EPROTO);

  hdr.len = 0;
  hdr.version = 2;
  pnut_msg_encode(&hdr, buf);
  assert_int_equal(pnut_msg_decode(buf, &hdr), -EPROTO);

  hdr.version = PNUT_MSG_VERSION;
  hdr.kind = 0;
  pnut_msg_encode(&hdr, buf);
  assert_int_equal(pnut_msg_decode(buf, &hdr), -EPROTO);

  assert_string_equal(pnut_status_name(PNUT_STATUS_TIMEOUT), "timeout");
  assert_string_equal(pnut_status_name(99), "unknown");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_msg_roundtrip),
    cmocka_unit_test(test_msg_rejects),
  };

  return cmocka_run_group_tests_name("msg", tests, NULL, NULL);
}
