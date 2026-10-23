/****************************************************************************
 * pnut-os/tests/unit/test_tz.c
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
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <pnut/tz.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void zone(FAR const char *name, FAR struct pnut_tz_s *tz)
{
  FAR const char *rule = pnut_tz_find(name);

  assert_non_null(rule);
  assert_int_equal(pnut_tz_parse(rule, tz), 0);
}

/* An instant, from a UTC date and time */

static time_t utc(int y, int mo, int d, int h, int mi)
{
  struct pnut_tz_s tz;
  struct tm tm;

  assert_int_equal(pnut_tz_parse("UTC0", &tz), 0);
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = y - 1900;
  tm.tm_mon  = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = mi;
  return pnut_tz_utc(&tz, &tm);
}

/* The wall time of an instant, as "YYYY-MM-DD HH:MM" and the offset */

static void expect_local(FAR const struct pnut_tz_s *tz, time_t t,
                         FAR const char *wall, int32_t off, int isdst)
{
  struct tm tm;
  int32_t got;
  char buf[64];

  pnut_tz_local(tz, t, &tm, &got);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min);
  assert_string_equal(buf, wall);
  assert_int_equal(got, off);
  assert_int_equal(tm.tm_isdst, isdst);
}

/* The instant of a wall time */

static time_t from_local(FAR const struct pnut_tz_s *tz, int y, int mo,
                         int d, int h, int mi)
{
  struct tm tm;

  memset(&tm, 0, sizeof(tm));
  tm.tm_year = y - 1900;
  tm.tm_mon  = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = mi;
  return pnut_tz_utc(tz, &tm);
}

/* The epoch, and a known date: the day of the week too */

static void test_tz_utc(void **state)
{
  struct pnut_tz_s tz;
  struct tm tm;

  assert_int_equal(utc(1970, 1, 1, 0, 0), 0);
  assert_int_equal(utc(2026, 10, 23, 12, 0), 1792756800);
  assert_int_equal(utc(1969, 12, 31, 23, 59), -60);

  zone("UTC", &tz);
  pnut_tz_local(&tz, 1792756800, &tm, NULL);
  assert_int_equal(tm.tm_wday, 5);          /* A Friday */
  assert_int_equal(tm.tm_yday, 295);
}

/* Warsaw: CET-1CEST,M3.5.0,M10.5.0/3; in 2026 summer time runs from
 * 29 March 01:00 UTC to 25 October 01:00 UTC
 */

static void test_tz_warsaw(void **state)
{
  struct pnut_tz_s tz;

  zone("Europe/Warsaw", &tz);
  assert_true(tz.has_dst);
  assert_int_equal(tz.std_off, 3600);
  assert_int_equal(tz.dst_off, 7200);
  assert_string_equal(tz.std_abbr, "CET");
  assert_string_equal(tz.dst_abbr, "CEST");

  expect_local(&tz, utc(2026, 1, 15, 12, 0), "2026-01-15 13:00", 3600, 0);
  expect_local(&tz, utc(2026, 3, 29, 0, 59), "2026-03-29 01:59", 3600, 0);
  expect_local(&tz, utc(2026, 3, 29, 1, 0), "2026-03-29 03:00", 7200, 1);
  expect_local(&tz, utc(2026, 10, 25, 0, 59), "2026-10-25 02:59", 7200,
               1);
  expect_local(&tz, utc(2026, 10, 25, 1, 0), "2026-10-25 02:00", 3600, 0);

  /* 02:30 on the spring night never happens: forward by the gap */

  assert_int_equal(from_local(&tz, 2026, 3, 29, 2, 30),
                   utc(2026, 3, 29, 1, 30));

  /* 02:30 on the autumn night happens twice: the first */

  assert_int_equal(from_local(&tz, 2026, 10, 25, 2, 30),
                   utc(2026, 10, 25, 0, 30));

  /* An ordinary time both ways */

  assert_int_equal(from_local(&tz, 2026, 7, 1, 12, 0),
                   utc(2026, 7, 1, 10, 0));

  /* Before 1970, in summer time */

  expect_local(&tz, utc(1969, 7, 1, 12, 0), "1969-07-01 14:00", 7200, 1);
}

/* Dublin: IST-1GMT0,M10.5.0,M3.5.0/1, its rule's "summer time" winter's:
 * the gap comes as it ends, in March
 */

static void test_tz_dublin(void **state)
{
  struct pnut_tz_s tz;
  struct tm tm;

  zone("Europe/Dublin", &tz);
  assert_int_equal(tz.std_off, 3600);
  assert_int_equal(tz.dst_off, 0);
  assert_string_equal(tz.std_abbr, "IST");
  assert_string_equal(tz.dst_abbr, "GMT");

  expect_local(&tz, utc(2026, 7, 1, 12, 0), "2026-07-01 13:00", 3600, 0);
  expect_local(&tz, utc(2026, 1, 15, 12, 0), "2026-01-15 12:00", 0, 1);

  /* 01:30 on 29 March never happens: forward by the gap, 02:30 IST */

  assert_int_equal(from_local(&tz, 2026, 3, 29, 1, 30),
                   utc(2026, 3, 29, 1, 30));

  /* 01:30 on 25 October happens twice: the first, in IST */

  assert_int_equal(from_local(&tz, 2026, 10, 25, 1, 30),
                   utc(2026, 10, 25, 0, 30));

  /* For strftime's %z and %Z */

  pnut_tz_local(&tz, utc(2026, 7, 1, 12, 0), &tm, NULL);
  assert_int_equal(tm.tm_gmtoff, 3600);
  assert_string_equal(tm.tm_zone, "IST");
}

/* Sydney: AEST-10AEDT,M10.1.0,M4.1.0/3, summer across the year's end */

static void test_tz_sydney(void **state)
{
  struct pnut_tz_s tz;

  zone("Australia/Sydney", &tz);
  expect_local(&tz, utc(2026, 1, 1, 0, 0), "2026-01-01 11:00", 39600, 1);
  expect_local(&tz, utc(2026, 7, 1, 0, 0), "2026-07-01 10:00", 36000, 0);

  /* Summer time ends 5 April 2026 03:00 local (16:00 UTC the day before)
   * and starts 4 October 02:00 local (16:00 UTC the day before)
   */

  expect_local(&tz, utc(2026, 4, 4, 15, 59), "2026-04-05 02:59", 39600,
               1);
  expect_local(&tz, utc(2026, 4, 4, 16, 0), "2026-04-05 02:00", 36000, 0);
  expect_local(&tz, utc(2026, 10, 3, 15, 59), "2026-10-04 01:59", 36000,
               0);
  expect_local(&tz, utc(2026, 10, 3, 16, 0), "2026-10-04 03:00", 39600,
               1);

  /* The year's end, both ways */

  expect_local(&tz, utc(2026, 12, 31, 13, 0), "2027-01-01 00:00", 39600,
               1);
  assert_int_equal(from_local(&tz, 2027, 1, 1, 0, 30),
                   utc(2026, 12, 31, 13, 30));
}

/* Without summer time; a half hour; three quarters, with summer time */

static void test_tz_others(void **state)
{
  struct pnut_tz_s tz;

  zone("Asia/Tokyo", &tz);
  assert_false(tz.has_dst);
  expect_local(&tz, utc(2026, 7, 1, 0, 0), "2026-07-01 09:00", 32400, 0);

  zone("Asia/Kolkata", &tz);
  expect_local(&tz, utc(2026, 7, 1, 0, 0), "2026-07-01 05:30", 19800, 0);

  zone("Pacific/Chatham", &tz);
  expect_local(&tz, utc(2026, 1, 1, 0, 0), "2026-01-01 13:45", 49500, 1);
  expect_local(&tz, utc(2026, 7, 1, 0, 0), "2026-07-01 12:45", 45900, 0);

  zone("America/Sao_Paulo", &tz);
  assert_string_equal(tz.std_abbr, "-03");
  expect_local(&tz, utc(2026, 7, 1, 12, 0), "2026-07-01 09:00", -10800,
               0);

  /* Santiago: Saturday 24:00, which is Sunday 00:00, 04:00 UTC */

  zone("America/Santiago", &tz);
  expect_local(&tz, utc(2026, 9, 6, 3, 59), "2026-09-05 23:59", -14400,
               0);
  expect_local(&tz, utc(2026, 9, 6, 4, 0), "2026-09-06 01:00", -10800, 1);

  /* Nuuk: a negative time, Saturday 23:00 */

  zone("America/Nuuk", &tz);
  expect_local(&tz, utc(2026, 3, 29, 0, 59), "2026-03-28 22:59", -7200,
               0);
  expect_local(&tz, utc(2026, 3, 29, 1, 0), "2026-03-29 00:00", -3600, 1);

  /* Lord Howe: a half-hour gap, 02:00 to 02:30 */

  zone("Australia/Lord_Howe", &tz);
  assert_int_equal(tz.dst_off - tz.std_off, 1800);
  assert_int_equal(from_local(&tz, 2026, 10, 4, 2, 15),
                   utc(2026, 10, 3, 15, 45));
}

/* The US default rules, Jn and n, and rules that do not parse */

static void test_tz_rules(void **state)
{
  struct pnut_tz_s tz;

  assert_int_equal(pnut_tz_parse("EST5EDT", &tz), 0);
  expect_local(&tz, utc(2026, 7, 1, 12, 0), "2026-07-01 08:00", -14400,
               1);
  expect_local(&tz, utc(2026, 12, 1, 12, 0), "2026-12-01 07:00", -18000,
               0);

  /* A summer offset given, the same as the default */

  assert_int_equal(pnut_tz_parse("EST5EDT4", &tz), 0);
  assert_int_equal(tz.dst_off, -14400);

  /* J60 is 1 March, leap year or not; J59 28 February */

  assert_int_equal(pnut_tz_parse("AAA0BBB,J60/0,J300/0", &tz), 0);
  expect_local(&tz, utc(2028, 2, 29, 23, 59), "2028-02-29 23:59", 0, 0);
  expect_local(&tz, utc(2028, 3, 1, 0, 0), "2028-03-01 01:00", 3600, 1);
  assert_int_equal(pnut_tz_parse("AAA0BBB,J59/0,J300/0", &tz), 0);
  expect_local(&tz, utc(2028, 2, 28, 0, 0), "2028-02-28 01:00", 3600, 1);

  /* 59, counted from 0, is 29 February in a leap year, 1 March in
   * another
   */

  assert_int_equal(pnut_tz_parse("AAA0BBB,59/0,300/0", &tz), 0);
  expect_local(&tz, utc(2028, 2, 29, 0, 0), "2028-02-29 01:00", 3600, 1);
  expect_local(&tz, utc(2027, 2, 28, 12, 0), "2027-02-28 12:00", 0, 0);
  expect_local(&tz, utc(2027, 3, 1, 0, 0), "2027-03-01 01:00", 3600, 1);

  /* A transition 167 hours into its day, 29 March: 4 April, 23:00 */

  assert_int_equal(pnut_tz_parse("AAA0BBB,M3.5.0/167,M10.5.0", &tz), 0);
  expect_local(&tz, utc(2026, 4, 4, 22, 59), "2026-04-04 22:59", 0, 0);
  expect_local(&tz, utc(2026, 4, 4, 23, 0), "2026-04-05 00:00", 3600, 1);

  /* Minutes in the offset */

  assert_int_equal(pnut_tz_parse("<+0545>-5:45", &tz), 0);
  assert_int_equal(tz.std_off, 20700);

  assert_int_equal(pnut_tz_parse("", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("AB1", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("CET", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M13.5.0,M10.5.0", &tz),
                   -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M3.5.0", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M3.5.0,M10.5.0x", &tz),
                   -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M3.6.0,M10.5.0", &tz),
                   -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M3.5.7,M10.5.0", &tz),
                   -EINVAL);
  assert_int_equal(pnut_tz_parse("AAA0BBB,J0,J300", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("AAA0BBB,J366,J300", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("AAA0BBB,366,300", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("<CET-1", &tz), -EINVAL);
  assert_int_equal(pnut_tz_parse("CET-1CEST,M3.5.0,M10.5.0/", &tz),
                   -EINVAL);
  assert_int_equal(pnut_tz_parse("CE1", &tz), -EINVAL);
}

/* The table: every zone's rule parses; names are in order */

static void test_tz_table(void **state)
{
  struct pnut_tz_s tz;
  FAR const char *prev = NULL;
  FAR const char *name;
  size_t i;

  for (i = 0; (name = pnut_tz_zone(i)) != NULL; i++)
    {
      FAR const char *rule = pnut_tz_find(name);

      assert_non_null(rule);
      assert_int_equal(pnut_tz_parse(rule, &tz), 0);
      if (prev != NULL)
        {
          assert_true(strcmp(prev, name) < 0);
        }

      prev = name;
    }

  assert_true(i > 400);
  assert_null(pnut_tz_zone(i));
  assert_null(pnut_tz_find("Europe/Nowhere"));
  assert_null(pnut_tz_find(""));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_tz_utc),
    cmocka_unit_test(test_tz_warsaw),
    cmocka_unit_test(test_tz_dublin),
    cmocka_unit_test(test_tz_sydney),
    cmocka_unit_test(test_tz_others),
    cmocka_unit_test(test_tz_rules),
    cmocka_unit_test(test_tz_table),
  };

  return cmocka_run_group_tests_name("tz", tests, NULL, NULL);
}
