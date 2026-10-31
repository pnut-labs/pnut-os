/****************************************************************************
 * pnut-os/tests/unit/test_telparse.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony's reading of the modem's lines (RFC 0030): the network's time
 * in each form a modem reports it, the modem's clock, registration,
 * signal, operator and the SIM.
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

#include "pnut/telephony.pb.h"

#include "parse.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define T_UTC   1793363696          /* 2026-10-30 12:34:56 UTC */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void test_telparse_fields(void **state)
{
  char out[32];

  assert_string_equal(tel_args("+COPS: 0,0,\"Orange, PL\",7"),
                      "0,0,\"Orange, PL\",7");
  assert_string_equal(tel_args("RING"), "RING");
  assert_true(tel_field("0,0,\"Orange, PL\",7", 2, out, sizeof(out)));
  assert_string_equal(out, "Orange, PL");
  assert_true(tel_field("0,0,\"Orange, PL\",7", 3, out, sizeof(out)));
  assert_string_equal(out, "7");
  assert_false(tel_field("0,0", 2, out, sizeof(out)));
  assert_true(tel_field("abcdef", 0, out, 4));
  assert_string_equal(out, "abc");

  assert_int_equal(tel_utc(2026, 10, 30, 12, 34, 56), T_UTC);
  assert_int_equal(tel_utc(26, 10, 30, 12, 34, 56), T_UTC);
  assert_int_equal(tel_utc(2000, 1, 1, 0, 0, 0), 946684800);
  assert_int_equal(tel_utc(2026, 13, 1, 0, 0, 0), -1);
  assert_int_equal(tel_utc(1969, 12, 31, 0, 0, 0), -1);
}

static void test_telparse_time(void **state)
{
  struct tel_time_s t;

  /* Local time with its zone, summer time out: one hour east */

  assert_int_equal(tel_network_time("+CTZE: \"+04\",0,"
                                    "\"2026/10/30,13:34:56\"", &t), 0);
  assert_true(t.has_time);
  assert_true(t.has_offset);
  assert_int_equal(t.utc, T_UTC);
  assert_int_equal(t.offset, 3600);
  assert_false(t.dst);

  /* UTC, two hours east with summer time in */

  assert_int_equal(tel_network_time("+CTZEU: \"+08\",1,"
                                    "\"2026/07/01,10:00:00\"", &t), 0);
  assert_int_equal(t.utc, 1782900000);
  assert_int_equal(t.offset, 7200);
  assert_true(t.dst);

  /* The zone alone, quoted or not, west too */

  assert_int_equal(tel_network_time("+CTZV: \"-20\"", &t), 0);
  assert_false(t.has_time);
  assert_true(t.has_offset);
  assert_int_equal(t.offset, -18000);
  assert_int_equal(tel_network_time("+CTZV: +32", &t), 0);
  assert_int_equal(t.offset, 28800);

  /* SIMCom's, with the time in UTC after it, quoted or not, as the
   * A7682E sends it
   */

  assert_int_equal(tel_network_time("+CTZV: +04,26/10/30,12:34:56", &t), 0);
  assert_true(t.has_time);
  assert_int_equal(t.utc, T_UTC);
  assert_int_equal(t.offset, 3600);
  assert_int_equal(tel_network_time("+CTZV: \"+04\",\"26/10/30,12:34:56\"",
                                    &t), 0);
  assert_int_equal(t.utc, T_UTC);

  /* SIMCom's older form: UTC */

  assert_int_equal(tel_network_time("*PSUTTZ: 2026,10,30,12,34,56,"
                                    "\"+04\",0", &t), 0);
  assert_int_equal(t.utc, T_UTC);
  assert_int_equal(t.offset, 3600);

  /* Not a time, or a broken one */

  assert_int_equal(tel_network_time("+CSQ: 20,99", &t), -EINVAL);
  assert_int_equal(tel_network_time("+CTZE: \"+04\"", &t), -EINVAL);
  assert_int_equal(tel_network_time("+CTZV: \"+99\"", &t), -EINVAL);

  /* The modem's clock: local time, its zone in quarter hours */

  assert_int_equal(tel_clock("+CCLK: \"26/10/30,13:34:56+04\"", &t), 0);
  assert_int_equal(t.utc, T_UTC);
  assert_int_equal(t.offset, 3600);
  assert_int_equal(tel_clock("+CCLK: \"26/10/30,07:34:56-20\"", &t), 0);
  assert_int_equal(t.utc, T_UTC);
  assert_int_equal(tel_clock("+CCLK: \"26/10/30,13:34:56\"", &t), -EINVAL);
}

static void test_telparse_state(void **state)
{
  char name[24];
  uint32_t bars;
  uint32_t pin;
  uint32_t puk;
  int32_t dbm;
  int tech;

  /* Registration: first when unasked, second in an answer */

  assert_int_equal(tel_registration("+CEREG: 1,\"1A2B\",\"01A2B3C4\",7",
                                    false),
                   PNUT_TELEPHONY_INFO_REGISTRATION_HOME);
  assert_int_equal(tel_registration("+CEREG: 2,5,\"1A2B\",\"01A2B3C4\",7",
                                    true),
                   PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING);
  assert_int_equal(tel_registration("+CREG: 0,3", true),
                   PNUT_TELEPHONY_INFO_REGISTRATION_DENIED);
  assert_int_equal(tel_registration("+CREG: 2", false),
                   PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING);
  assert_int_equal(tel_registration("+CSQ: 1", false), -EINVAL);

  /* Signal */

  assert_int_equal(tel_signal("+CSQ: 20,99", &dbm, &bars), 0);
  assert_int_equal(dbm, -73);
  assert_int_equal(bars, 4);
  assert_int_equal(tel_signal("+CSQ: 5,99", &dbm, &bars), 0);
  assert_int_equal(bars, 1);
  assert_int_equal(tel_signal("+CSQ: 99,99", &dbm, &bars), 0);
  assert_int_equal(dbm, 0);
  assert_int_equal(bars, 0);

  /* Operator */

  assert_int_equal(tel_operator("+COPS: 0,0,\"Orange PL\",7", name,
                                sizeof(name), &tech), 0);
  assert_string_equal(name, "Orange PL");
  assert_int_equal(tech, PNUT_TELEPHONY_INFO_TECH_LTE);
  assert_int_equal(tel_operator("+COPS: 0,0,\"Plus\",0", name, sizeof(name),
                                &tech), 0);
  assert_int_equal(tech, PNUT_TELEPHONY_INFO_TECH_GSM);
  assert_int_equal(tel_operator("+COPS: 0", name, sizeof(name), &tech), 0);
  assert_string_equal(name, "");
  assert_int_equal(tech, PNUT_TELEPHONY_INFO_TECH_NO_TECH);

  /* The SIM */

  assert_int_equal(tel_sim("+CPIN: READY"),
                   PNUT_TELEPHONY_INFO_SIM_READY);
  assert_int_equal(tel_sim("+CPIN: SIM PIN"),
                   PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN);
  assert_int_equal(tel_sim("+CPIN: SIM PUK"),
                   PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK);
  assert_int_equal(tel_sim("+CME ERROR: SIM not inserted"),
                   PNUT_TELEPHONY_INFO_SIM_ABSENT);
  assert_int_equal(tel_sim("+CME ERROR: 10"),
                   PNUT_TELEPHONY_INFO_SIM_ABSENT);
  assert_int_equal(tel_sim("+SIMCARD: NOT AVAILABLE"),
                   PNUT_TELEPHONY_INFO_SIM_ABSENT);
  assert_int_equal(tel_sim("+CME ERROR: SIM busy"), -EINVAL);

  assert_int_equal(tel_tries("+SPIC: 3,10,3,10", &pin, &puk), 0);
  assert_int_equal(pin, 3);
  assert_int_equal(puk, 10);
  assert_int_equal(tel_tries("+SPIC: x", &pin, &puk), -EINVAL);
}

/****************************************************************************
 * Name: test_telparse_sms
 *
 * Description:
 *   SMS in the modem's memory: the slot one was stored in, one listed,
 *   the length of one read; lines of other kinds, or broken, refused.
 *
 ****************************************************************************/

static void test_telparse_sms(void **state)
{
  assert_int_equal(tel_sms_slot("+CMTI: \"ME\",3"), 3);
  assert_int_equal(tel_sms_slot("+CMTI: \"SM\",12"), -ENODEV);
  assert_int_equal(tel_sms_slot("+CMTI: \"ME\",40000"), -EINVAL);
  assert_int_equal(tel_sms_slot("+CMTI: \"ME\""), -EINVAL);
  assert_int_equal(tel_sms_slot("+CMGL: 3,1,,24"), -EINVAL);

  assert_int_equal(tel_sms_listed("+CMGL: 3,1,,24"), 3);
  assert_int_equal(tel_sms_listed("+CMGL: 0,0,\"\",160"), 0);
  assert_int_equal(tel_sms_listed("+CMGL: 3,1"), -EINVAL);
  assert_int_equal(tel_sms_listed("+CMGL: x,1,,24"), -EINVAL);

  assert_int_equal(tel_sms_length("+CMGR: 1,,24"), 24);
  assert_int_equal(tel_sms_length("+CMGR: 0,\"\",159"), 159);
  assert_int_equal(tel_sms_length("+CMGR: 1,,0"), -EINVAL);
  assert_int_equal(tel_sms_length("+CMGR: 1,,300"), -EINVAL);
  assert_int_equal(tel_sms_length("+CSQ: 20,99"), -EINVAL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_telparse_fields),
    cmocka_unit_test(test_telparse_time),
    cmocka_unit_test(test_telparse_state),
    cmocka_unit_test(test_telparse_sms),
  };

  return cmocka_run_group_tests_name("telparse", tests, NULL, NULL);
}
