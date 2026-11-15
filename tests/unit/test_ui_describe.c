/****************************************************************************
 * pnut-os/tests/unit/test_ui_describe.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* What the system UI says of the device's state (describe.c): Wi-Fi's and
 * the mobile network's one-line states, a network's strength in a word,
 * the header's meters and the date.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <pnut/msg.h>

#include "describe.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR const char *wifi(pnut_wifi_info_state_t state, const char *name)
{
  static char buf[64];
  pnut_wifi_info_t w;

  memset(&w, 0, sizeof(w));
  w.state = state;
  strcpy(w.name, name);
  describe_wifi(&w, buf, sizeof(buf));
  return buf;
}

static void test_describe_wifi(void **state)
{
  assert_string_equal(wifi(PNUT_WIFI_INFO_STATE_OFF, ""), "Off");
  assert_string_equal(wifi(PNUT_WIFI_INFO_STATE_ON, ""), "On");
  assert_string_equal(wifi(PNUT_WIFI_INFO_STATE_JOINING, "Home"),
                      "Joining Home");
  assert_string_equal(wifi(PNUT_WIFI_INFO_STATE_JOINING, ""), "Joining");
  assert_string_equal(wifi(PNUT_WIFI_INFO_STATE_JOINED, "Home"),
                      "On \xc2\xb7 Home");
}

static FAR const char *cell(pnut_telephony_info_modem_t modem,
                            pnut_telephony_info_reason_t reason,
                            pnut_telephony_info_sim_t sim,
                            pnut_telephony_info_registration_t reg,
                            pnut_telephony_info_tech_t tech,
                            pnut_telephony_info_data_t data)
{
  static char buf[64];
  pnut_telephony_info_t t;

  memset(&t, 0, sizeof(t));
  t.modem        = modem;
  t.reason       = reason;
  t.sim          = sim;
  t.registration = reg;
  t.tech         = tech;
  t.data         = data;
  describe_cell(&t, buf, sizeof(buf));
  return buf;
}

#define M(x)  PNUT_TELEPHONY_INFO_MODEM_##x
#define R(x)  PNUT_TELEPHONY_INFO_REASON_##x
#define S(x)  PNUT_TELEPHONY_INFO_SIM_##x
#define G(x)  PNUT_TELEPHONY_INFO_REGISTRATION_##x
#define T(x)  PNUT_TELEPHONY_INFO_TECH_##x
#define D(x)  PNUT_TELEPHONY_INFO_DATA_##x

static void test_describe_cell(void **state)
{
  assert_string_equal(cell(M(OFF), R(DISABLED), S(SIM_UNKNOWN),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "Modem off");
  assert_string_equal(cell(M(OFF), R(NO_MODEM), S(SIM_UNKNOWN),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "No modem");
  assert_string_equal(cell(M(OFF), R(NO_SIM), S(ABSENT),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "No SIM");
  assert_string_equal(cell(M(FAILED), R(NO_ANSWER), S(SIM_UNKNOWN),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "Modem failed");
  assert_string_equal(cell(M(STARTING), R(NO_REASON), S(SIM_UNKNOWN),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "Starting");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(NEEDS_PIN),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "SIM locked");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(NEEDS_PUK),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "SIM needs its PUK");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(BLOCKED),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "SIM blocked");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(SEARCHING), T(NO_TECH), D(DATA_OFF)),
                      "Searching");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(NOT_REGISTERED), T(NO_TECH), D(DATA_OFF)),
                      "No network");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(DENIED), T(NO_TECH), D(DATA_OFF)),
                      "Network refused");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(HOME), T(LTE), D(DATA_OFF)),
                      "LTE \xc2\xb7 data off");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(ROAMING), T(GSM), D(DATA_ON)),
                      "GSM \xc2\xb7 data on");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(HOME), T(LTE), D(CONNECTING)),
                      "LTE \xc2\xb7 data connecting");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(HOME), T(LTE), D(DATA_FAILED)),
                      "LTE \xc2\xb7 data failed");
  assert_string_equal(cell(M(ON), R(NO_REASON), S(READY),
                           G(HOME), T(LTE), D(DATA_STANDBY)),
                      "LTE \xc2\xb7 data on standby");
}

static void test_describe_words(void **state)
{
  assert_string_equal(describe_strength(-50), "strong");
  assert_string_equal(describe_strength(-60), "strong");
  assert_string_equal(describe_strength(-61), "fair");
  assert_string_equal(describe_strength(-75), "fair");
  assert_string_equal(describe_strength(-76), "weak");
}

static void test_describe_meters(void **state)
{
  assert_int_equal(describe_signal_steps(0), 0);
  assert_int_equal(describe_signal_steps(1), 1);
  assert_int_equal(describe_signal_steps(2), 3);
  assert_int_equal(describe_signal_steps(3), 4);
  assert_int_equal(describe_signal_steps(4), 5);
  assert_int_equal(describe_signal_steps(9), 5);

  assert_int_equal(describe_battery_steps(0), 0);
  assert_int_equal(describe_battery_steps(1), 1);
  assert_int_equal(describe_battery_steps(20), 1);
  assert_int_equal(describe_battery_steps(21), 2);
  assert_int_equal(describe_battery_steps(100), 5);
  assert_int_equal(describe_battery_steps(150), 5);
}

static void test_describe_time(void **state)
{
  struct tm tm;
  char buf[32];

  memset(&tm, 0, sizeof(tm));
  tm.tm_year = 2026 - 1900;
  tm.tm_mon  = 8;
  tm.tm_mday = 22;
  tm.tm_wday = 2;
  tm.tm_hour = 14;
  tm.tm_min  = 7;

  describe_date(&tm, buf, sizeof(buf));
  assert_string_equal(buf, "Tue, 22 Sep");
  describe_clock(&tm, buf, sizeof(buf));
  assert_string_equal(buf, "14:07");

  tm.tm_hour = 9;
  tm.tm_min  = 5;
  describe_clock(&tm, buf, sizeof(buf));
  assert_string_equal(buf, "09:05");
}

static void test_describe_name(void **state)
{
  char buf[DESCRIBE_NAME_MAX];

  describe_name("Kawiarnia Pod Lip\xc4\x85", buf, sizeof(buf));
  assert_string_equal(buf, "Kawiarnia Pod Lip\xc4\x85");

  /* A byte that is not UTF-8 is shown as its code; so is a control */

  describe_name("Cafe\xff\x01!", buf, sizeof(buf));
  assert_string_equal(buf, "Cafe\\xff\\x01!");

  /* What the fonts cannot draw, or would draw as nothing, is shown as its
   * bytes, so that no name looks like another: a zero-width space, a
   * right-to-left override, a no-break space, a soft hyphen, a C1
   * control, an overlong '/', a surrogate; and the backslash itself
   */

  describe_name("Home\xe2\x80\x8b", buf, sizeof(buf));
  assert_string_equal(buf, "Home\\xe2\\x80\\x8b");
  describe_name("a\xe2\x80\xae" "b", buf, sizeof(buf));
  assert_string_equal(buf, "a\\xe2\\x80\\xaeb");
  describe_name("Home\xc2\xa0", buf, sizeof(buf));
  assert_string_equal(buf, "Home\\xc2\\xa0");
  describe_name("Ho\xc2\xad" "me", buf, sizeof(buf));
  assert_string_equal(buf, "Ho\\xc2\\xadme");
  describe_name("x\xc2\x85", buf, sizeof(buf));
  assert_string_equal(buf, "x\\xc2\\x85");
  describe_name("\xc0\xaf", buf, sizeof(buf));
  assert_string_equal(buf, "\\xc0\\xaf");
  describe_name("\xed\xa0\x80", buf, sizeof(buf));
  assert_string_equal(buf, "\\xed\\xa0\\x80");
  describe_name("a\\xffb", buf, sizeof(buf));
  assert_string_equal(buf, "a\\x5cxffb");

  /* A letter the fonts have, and one they have not (Cyrillic) */

  describe_name("\xc5\x81\xc3\xb3" "d\xc5\xba", buf, sizeof(buf));
  assert_string_equal(buf, "\xc5\x81\xc3\xb3" "d\xc5\xba");
  describe_name("lilyg\xd0\xbe", buf, sizeof(buf));
  assert_string_equal(buf, "lilyg\\xd0\\xbe");

  /* 32 such bytes still fit */

  describe_name("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                "\xff\xff\xff\xff\xff\xff\xff\xff", buf, sizeof(buf));
  assert_int_equal(strlen(buf), 32 * 4);
}

static void test_describe_wifi_failure(void **state)
{
  assert_string_equal(describe_wifi_failure(PNUT_STATUS_INVALID,
                                            PNUT_WIFI_ERROR_CODE_REFUSED),
                      "Wrong password, or out of reach");
  assert_string_equal(describe_wifi_failure(PNUT_STATUS_UNAVAILABLE,
                                            PNUT_WIFI_ERROR_CODE_OFF),
                      "Wi-Fi is off");
  assert_string_equal(describe_wifi_failure(PNUT_STATUS_TIMEOUT, 0),
                      "Wi-Fi did not answer");
  assert_string_equal(describe_wifi_failure(PNUT_STATUS_INTERNAL, 0),
                      "Wi-Fi failed");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_describe_wifi),
    cmocka_unit_test(test_describe_cell),
    cmocka_unit_test(test_describe_words),
    cmocka_unit_test(test_describe_meters),
    cmocka_unit_test(test_describe_time),
    cmocka_unit_test(test_describe_name),
    cmocka_unit_test(test_describe_wifi_failure),
  };

  return cmocka_run_group_tests_name("ui_describe", tests, NULL, NULL);
}
