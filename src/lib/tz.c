/****************************************************************************
 * pnut-os/src/lib/tz.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <pnut/tz.h>

#include "pnut_tz.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TZ_DAY        86400
#define TZ_HOUR       3600

/* A transition's default time, 02:00 local */

#define TZ_DEFAULT_TIME  (2 * TZ_HOUR)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tz_leap
 ****************************************************************************/

static bool tz_leap(int64_t year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/****************************************************************************
 * Name: tz_days_from_civil
 *
 * Description:
 *   Days from 1970-01-01 to a date of the proleptic Gregorian calendar
 *   (Howard Hinnant's algorithm), month 1-12.
 *
 ****************************************************************************/

static int64_t tz_days_from_civil(int64_t y, int m, int d)
{
  int64_t era;
  int64_t yoe;
  int64_t doy;
  int64_t doe;

  y  -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/****************************************************************************
 * Name: tz_civil_from_days
 ****************************************************************************/

static void tz_civil_from_days(int64_t z, FAR int64_t *yp, FAR int *mp,
                               FAR int *dp)
{
  int64_t era;
  int64_t doe;
  int64_t yoe;
  int64_t doy;
  int64_t mp0;

  z  += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp0 = (5 * doy + 2) / 153;

  *dp = (int)(doy - (153 * mp0 + 2) / 5 + 1);
  *mp = (int)(mp0 < 10 ? mp0 + 3 : mp0 - 9);
  *yp = yoe + era * 400 + (*mp <= 2);
}

/****************************************************************************
 * Name: tz_to_tm
 *
 * Description:
 *   Seconds since the epoch, taken as a wall time, into a struct tm.
 *
 ****************************************************************************/

static void tz_to_tm(int64_t secs, FAR struct tm *tm)
{
  int64_t days = secs / TZ_DAY;
  int64_t rem = secs % TZ_DAY;
  int64_t year;
  int month;
  int day;

  if (rem < 0)
    {
      rem += TZ_DAY;
      days--;
    }

  tz_civil_from_days(days, &year, &month, &day);

  memset(tm, 0, sizeof(*tm));
  tm->tm_year = (int)(year - 1900);
  tm->tm_mon  = month - 1;
  tm->tm_mday = day;
  tm->tm_hour = (int)(rem / TZ_HOUR);
  tm->tm_min  = (int)(rem % TZ_HOUR / 60);
  tm->tm_sec  = (int)(rem % 60);
  tm->tm_wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01: Thursday */
  tm->tm_yday = (int)(days - tz_days_from_civil(year, 1, 1));
}

/****************************************************************************
 * Name: tz_from_tm
 *
 * Description:
 *   A struct tm, taken as a wall time, into seconds since the epoch.  The
 *   fields may be out of their ranges, as mktime() allows.
 *
 ****************************************************************************/

static int64_t tz_from_tm(FAR const struct tm *tm)
{
  int64_t year = (int64_t)tm->tm_year + 1900;
  int64_t mon = tm->tm_mon;

  year += mon / 12;
  mon  %= 12;
  if (mon < 0)
    {
      mon += 12;
      year--;
    }

  return tz_days_from_civil(year, (int)mon + 1, 1) * TZ_DAY +
         ((int64_t)tm->tm_mday - 1) * TZ_DAY +
         (int64_t)tm->tm_hour * TZ_HOUR + (int64_t)tm->tm_min * 60 +
         tm->tm_sec;
}

/****************************************************************************
 * Name: tz_number
 *
 * Description:
 *   A decimal number of at most max, its digits counted in *digits.
 *
 ****************************************************************************/

static FAR const char *tz_number(FAR const char *p, int max, FAR int *value)
{
  int n = 0;

  if (!isdigit((unsigned char)*p))
    {
      return NULL;
    }

  while (isdigit((unsigned char)*p))
    {
      n = n * 10 + (*p++ - '0');
      if (n > max)
        {
          return NULL;
        }
    }

  *value = n;
  return p;
}

/****************************************************************************
 * Name: tz_hms
 *
 * Description:
 *   [+|-]hh[:mm[:ss]], hours to 167 (RFC 8536), into seconds.
 *
 ****************************************************************************/

static FAR const char *tz_hms(FAR const char *p, FAR int32_t *secs)
{
  int sign = 1;
  int h;
  int m = 0;
  int s = 0;

  if (*p == '+' || *p == '-')
    {
      sign = *p++ == '-' ? -1 : 1;
    }

  p = tz_number(p, 167, &h);
  if (p != NULL && *p == ':')
    {
      p = tz_number(p + 1, 59, &m);
      if (p != NULL && *p == ':')
        {
          p = tz_number(p + 1, 59, &s);
        }
    }

  if (p != NULL)
    {
      *secs = sign * (h * TZ_HOUR + m * 60 + s);
    }

  return p;
}

/****************************************************************************
 * Name: tz_abbr
 *
 * Description:
 *   A zone abbreviation: three or more letters, or <...> with letters,
 *   digits, + and -.
 *
 ****************************************************************************/

static FAR const char *tz_abbr(FAR const char *p, FAR char *abbr)
{
  size_t n = 0;

  if (*p == '<')
    {
      p++;
      while (*p != '>')
        {
          if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-')
            {
              return NULL;
            }

          if (n < PNUT_TZ_ABBR_MAX)
            {
              abbr[n] = *p;
            }

          n++;
          p++;
        }

      p++;
    }
  else
    {
      while (isalpha((unsigned char)*p))
        {
          if (n < PNUT_TZ_ABBR_MAX)
            {
              abbr[n] = *p;
            }

          n++;
          p++;
        }
    }

  if (n < 3)
    {
      return NULL;
    }

  abbr[n < PNUT_TZ_ABBR_MAX ? n : PNUT_TZ_ABBR_MAX] = '\0';
  return p;
}

/****************************************************************************
 * Name: tz_when
 *
 * Description:
 *   A transition: Mm.w.d, Jn or n, then an optional /time.
 *
 ****************************************************************************/

static FAR const char *tz_when(FAR const char *p,
                               FAR struct pnut_tz_when_s *when)
{
  int a;
  int b;
  int c;

  memset(when, 0, sizeof(*when));

  if (*p == 'M')
    {
      p = tz_number(p + 1, 12, &a);
      if (p == NULL || *p != '.' || a < 1)
        {
          return NULL;
        }

      p = tz_number(p + 1, 5, &b);
      if (p == NULL || *p != '.' || b < 1)
        {
          return NULL;
        }

      p = tz_number(p + 1, 6, &c);
      if (p == NULL)
        {
          return NULL;
        }

      when->kind  = 'M';
      when->month = a;
      when->week  = b;
      when->wday  = c;
    }
  else if (*p == 'J')
    {
      p = tz_number(p + 1, 365, &a);
      if (p == NULL || a < 1)
        {
          return NULL;
        }

      when->kind = 'J';
      when->day  = a;
    }
  else
    {
      p = tz_number(p, 365, &a);
      if (p == NULL)
        {
          return NULL;
        }

      when->kind = 'n';
      when->day  = a;
    }

  when->time = TZ_DEFAULT_TIME;
  if (*p == '/')
    {
      p = tz_hms(p + 1, &when->time);
    }

  return p;
}

/****************************************************************************
 * Name: tz_transition
 *
 * Description:
 *   A transition's instant in a year, in seconds since the epoch as a
 *   wall time in the offset it is given in.
 *
 ****************************************************************************/

static int64_t tz_transition(FAR const struct pnut_tz_when_s *when,
                             int64_t year)
{
  int64_t day;
  int wday;
  int mdays;
  int d;

  static const uint8_t days[] =
  {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  if (when->kind == 'M')
    {
      day   = tz_days_from_civil(year, when->month, 1);
      wday  = (int)((day % 7 + 11) % 7);
      d     = 1 + (when->wday - wday + 7) % 7 + (when->week - 1) * 7;
      mdays = days[when->month - 1] +
              (when->month == 2 && tz_leap(year) ? 1 : 0);
      while (d > mdays)
        {
          d -= 7;
        }

      day = tz_days_from_civil(year, when->month, d);
    }
  else if (when->kind == 'J')
    {
      /* 1-365, never February 29 */

      day = tz_days_from_civil(year, 1, 1) + when->day - 1;
      if (tz_leap(year) && when->day >= 60)
        {
          day++;
        }
    }
  else
    {
      day = tz_days_from_civil(year, 1, 1) + when->day;
    }

  return day * TZ_DAY + when->time;
}

/****************************************************************************
 * Name: tz_in_dst
 *
 * Description:
 *   Whether an instant is in summer time.
 *
 ****************************************************************************/

static bool tz_in_dst(FAR const struct pnut_tz_s *tz, int64_t utc)
{
  struct tm tm;
  int64_t year;
  int64_t start;
  int64_t end;

  if (!tz->has_dst)
    {
      return false;
    }

  tz_to_tm(utc + tz->std_off, &tm);
  year  = (int64_t)tm.tm_year + 1900;
  start = tz_transition(&tz->start, year) - tz->std_off;
  end   = tz_transition(&tz->end, year) - tz->dst_off;

  /* Northern zones' summer is within a year, southern ones' across its
   * end
   */

  return start < end ? utc >= start && utc < end :
                       utc < end || utc >= start;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_tz_find
 ****************************************************************************/

FAR const char *pnut_tz_find(FAR const char *zone)
{
  size_t lo = 0;
  size_t hi = g_pnut_tz_nzones;
  size_t mid;
  int cmp;

  while (lo < hi)
    {
      mid = lo + (hi - lo) / 2;
      cmp = strcmp(zone, g_pnut_tz_zones[mid].name);
      if (cmp == 0)
        {
          return g_pnut_tz_rules[g_pnut_tz_zones[mid].rule];
        }

      if (cmp < 0)
        {
          hi = mid;
        }
      else
        {
          lo = mid + 1;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: pnut_tz_zone
 ****************************************************************************/

FAR const char *pnut_tz_zone(size_t index)
{
  return index < g_pnut_tz_nzones ? g_pnut_tz_zones[index].name : NULL;
}

/****************************************************************************
 * Name: pnut_tz_parse
 ****************************************************************************/

int pnut_tz_parse(FAR const char *rule, FAR struct pnut_tz_s *tz)
{
  FAR const char *p = rule;
  int32_t off;

  memset(tz, 0, sizeof(*tz));

  /* The standard time: its name and its offset, which POSIX gives west of
   * UTC
   */

  p = tz_abbr(p, tz->std_abbr);
  if (p == NULL || (p = tz_hms(p, &off)) == NULL)
    {
      return -EINVAL;
    }

  tz->std_off = -off;
  tz->dst_off = tz->std_off;

  if (*p == '\0')
    {
      return OK;
    }

  /* Summer time: its name, its offset (an hour ahead unless given), and
   * when it starts and ends (the US rules unless given)
   */

  p = tz_abbr(p, tz->dst_abbr);
  if (p == NULL)
    {
      return -EINVAL;
    }

  tz->has_dst = true;
  tz->dst_off = tz->std_off + TZ_HOUR;

  if (*p != ',' && *p != '\0')
    {
      p = tz_hms(p, &off);
      if (p == NULL)
        {
          return -EINVAL;
        }

      tz->dst_off = -off;
    }

  if (*p == '\0')
    {
      p = tz_when("M3.2.0", &tz->start);
      p = p != NULL ? tz_when("M11.1.0", &tz->end) : NULL;
      return p != NULL ? OK : -EINVAL;
    }

  if (*p != ',' || (p = tz_when(p + 1, &tz->start)) == NULL ||
      *p != ',' || (p = tz_when(p + 1, &tz->end)) == NULL || *p != '\0')
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: pnut_tz_local
 ****************************************************************************/

void pnut_tz_local(FAR const struct pnut_tz_s *tz, time_t utc,
                   FAR struct tm *local, FAR int32_t *offp)
{
  bool dst = tz_in_dst(tz, utc);
  int32_t off = dst ? tz->dst_off : tz->std_off;

  tz_to_tm((int64_t)utc + off, local);
  local->tm_isdst  = dst;
  local->tm_gmtoff = off;
  local->tm_zone   = dst ? tz->dst_abbr : tz->std_abbr;

  if (offp != NULL)
    {
      *offp = off;
    }
}

/****************************************************************************
 * Name: pnut_tz_utc
 ****************************************************************************/

time_t pnut_tz_utc(FAR const struct pnut_tz_s *tz,
                   FAR const struct tm *local)
{
  int64_t wall = tz_from_tm(local);
  int64_t as_std = wall - tz->std_off;
  int64_t as_dst = wall - tz->dst_off;
  bool std_ok;
  bool dst_ok;

  if (!tz->has_dst)
    {
      return (time_t)as_std;
    }

  std_ok = !tz_in_dst(tz, as_std);
  dst_ok = tz_in_dst(tz, as_dst);

  /* Twice: the first.  Never: forward by the gap, which reading it with
   * the offset in force before the gap gives, the smaller of the two:
   * the later instant (Europe/Dublin's summer time is its rule's
   * standard time, and its gap comes as the rule's summer time ends)
   */

  if (std_ok && dst_ok)
    {
      return (time_t)(as_std < as_dst ? as_std : as_dst);
    }

  if (std_ok || dst_ok)
    {
      return (time_t)(dst_ok ? as_dst : as_std);
    }

  return (time_t)(as_std > as_dst ? as_std : as_dst);
}
