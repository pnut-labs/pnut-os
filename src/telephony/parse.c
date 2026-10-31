/****************************************************************************
 * pnut-os/src/telephony/parse.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pnut/telephony.pb.h"

#include "parse.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QUARTER_HOUR    900         /* Seconds */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: parse_starts
 ****************************************************************************/

static bool parse_starts(FAR const char *line, FAR const char *prefix)
{
  return strncmp(line, prefix, strlen(prefix)) == 0;
}

/****************************************************************************
 * Name: parse_int
 *
 * Description:
 *   A field as a whole number, all of it.
 *
 ****************************************************************************/

static bool parse_int(FAR const char *args, int n, FAR long *value)
{
  char field[16];
  FAR char *end;

  if (!tel_field(args, n, field, sizeof(field)) || field[0] == '\0')
    {
      return false;
    }

  *value = strtol(field, &end, 10);
  return *end == '\0';
}

/****************************************************************************
 * Name: parse_zone
 *
 * Description:
 *   A zone in quarters of an hour, "+08" or "-20", with or without its
 *   sign, as seconds.
 *
 ****************************************************************************/

static bool parse_zone(FAR const char *s, FAR int32_t *offset)
{
  FAR char *end;
  long q;

  q = strtol(s, &end, 10);
  if (end == s || *end != '\0' || q < -56 || q > 56)
    {
      return false;
    }

  *offset = (int32_t)q * QUARTER_HOUR;
  return true;
}

/****************************************************************************
 * Name: parse_datetime
 *
 * Description:
 *   "yyyy/MM/dd,hh:mm:ss" or "yy/MM/dd,hh:mm:ss", as seconds since 1970 of
 *   that date and time taken as UTC.  rest, if not NULL, is left at what
 *   follows the seconds.
 *
 ****************************************************************************/

static bool parse_datetime(FAR const char *s, FAR int64_t *t,
                           FAR const char **rest)
{
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int sec;
  int n = 0;

  if (sscanf(s, "%d/%d/%d,%d:%d:%d%n", &y, &mo, &d, &h, &mi, &sec, &n)
      != 6)
    {
      return false;
    }

  *t = tel_utc(y, mo, d, h, mi, sec);
  if (rest != NULL)
    {
      *rest = s + n;
    }

  return *t >= 0;
}

/****************************************************************************
 * Name: parse_reg
 *
 * Description:
 *   27.007's registration states as pnut.TelephonyInfo.Registration.
 *
 ****************************************************************************/

static int parse_reg(long stat)
{
  switch (stat)
    {
      case 0:
      case 4:
        return PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;

      case 1:
      case 6:                       /* SMS only */
      case 9:                       /* CSFB not preferred */
        return PNUT_TELEPHONY_INFO_REGISTRATION_HOME;

      case 2:
        return PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING;

      case 3:
        return PNUT_TELEPHONY_INFO_REGISTRATION_DENIED;

      case 5:
      case 7:
      case 10:
        return PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR const char *tel_args(FAR const char *line)
{
  FAR const char *p = strchr(line, ':');

  if (p == NULL || (line[0] != '+' && line[0] != '*' &&
                    line[0] != '^'))
    {
      return line;
    }

  p++;
  while (*p == ' ')
    {
      p++;
    }

  return p;
}

bool tel_field(FAR const char *args, int n, FAR char *out, size_t size)
{
  FAR const char *p = args;
  bool quoted = false;
  size_t len = 0;

  /* To the field's start */

  while (n > 0)
    {
      if (*p == '\0')
        {
          return false;
        }

      if (*p == '"')
        {
          quoted = !quoted;
        }
      else if (*p == ',' && !quoted)
        {
          n--;
        }

      p++;
    }

  /* Its characters, without quotes */

  quoted = false;
  while (*p != '\0' && (quoted || *p != ','))
    {
      if (*p == '"')
        {
          quoted = !quoted;
        }
      else if (len + 1 < size)
        {
          out[len++] = *p;
        }

      p++;
    }

  if (size > 0)
    {
      out[len] = '\0';
    }

  return true;
}

int64_t tel_utc(int year, int month, int day, int hour, int min, int sec)
{
  int64_t days;
  int64_t y;
  int64_t era;
  int64_t yoe;
  int64_t doy;
  int64_t doe;
  int m;

  if (year < 100)
    {
      year += 2000;
    }

  if (year < 1970 || year > 2200 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
      sec < 0 || sec > 60)
    {
      return -1;
    }

  /* Days from 1970-01-01 (H. Hinnant's days_from_civil) */

  y   = month <= 2 ? year - 1 : year;
  m   = month;
  era = y / 400;
  yoe = y - era * 400;
  doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = era * 146097 + doe - 719468;

  return days * 86400 + hour * 3600 + min * 60 + sec;
}

int tel_network_time(FAR const char *line, FAR struct tel_time_s *t)
{
  FAR const char *args = tel_args(line);
  FAR const char *rest;
  char field[32];
  long v[6];
  long dst;
  int i;

  memset(t, 0, sizeof(*t));

  if (parse_starts(line, "+CTZEU:") || parse_starts(line, "+CTZE:"))
    {
      /* <tz>,<dst>[,<time>]: the time UTC in +CTZEU, local in +CTZE */

      if (!tel_field(args, 0, field, sizeof(field)) ||
          !parse_zone(field, &t->offset) || !parse_int(args, 1, &dst))
        {
          return -EINVAL;
        }

      t->has_offset = true;
      t->dst        = dst > 0;

      if (tel_field(args, 2, field, sizeof(field)) && field[0] != '\0')
        {
          if (!parse_datetime(field, &t->utc, NULL))
            {
              return -EINVAL;
            }

          if (parse_starts(line, "+CTZE:"))
            {
              t->utc -= t->offset;
            }

          t->has_time = true;
        }

      return OK;
    }

  if (parse_starts(line, "+CTZV:"))
    {
      /* <tz>, and on SIMCom's modems the time after it, in UTC, quoted or
       * not: "yy/MM/dd,hh:mm:ss" holds a comma of its own.  The A7682E
       * sends "+CTZV: +04,26/10/30,20:28:28" (seen 2026-10-30).
       */

      if (!tel_field(args, 0, field, sizeof(field)) ||
          !parse_zone(field, &t->offset))
        {
          return -EINVAL;
        }

      t->has_offset = true;
      rest = strchr(args, ',');
      if (rest != NULL)
        {
          rest++;
          while (*rest == '"' || *rest == ' ')
            {
              rest++;
            }

          if (parse_datetime(rest, &t->utc, NULL))
            {
              t->has_time = true;
            }
        }

      return OK;
    }

  if (parse_starts(line, "*PSUTTZ:"))
    {
      /* year,month,day,hour,min,sec,"<tz>",<dst>: the time UTC */

      for (i = 0; i < 6; i++)
        {
          if (!parse_int(args, i, &v[i]))
            {
              return -EINVAL;
            }
        }

      if (!tel_field(args, 6, field, sizeof(field)) ||
          !parse_zone(field, &t->offset) || !parse_int(args, 7, &dst))
        {
          return -EINVAL;
        }

      t->utc = tel_utc(v[0], v[1], v[2], v[3], v[4], v[5]);
      if (t->utc < 0)
        {
          return -EINVAL;
        }

      t->has_time   = true;
      t->has_offset = true;
      t->dst        = dst > 0;
      return OK;
    }

  return -EINVAL;
}

int tel_clock(FAR const char *line, FAR struct tel_time_s *t)
{
  char field[32];
  FAR const char *rest;

  memset(t, 0, sizeof(*t));

  if (!parse_starts(line, "+CCLK:") ||
      !tel_field(tel_args(line), 0, field, sizeof(field)) ||
      !parse_datetime(field, &t->utc, &rest) ||
      (*rest != '+' && *rest != '-') || !parse_zone(rest, &t->offset))
    {
      return -EINVAL;
    }

  t->utc       -= t->offset;
  t->has_time   = true;
  t->has_offset = true;
  return OK;
}

int tel_registration(FAR const char *line, bool answer)
{
  long stat;

  if (!parse_starts(line, "+CREG:") && !parse_starts(line, "+CEREG:"))
    {
      return -EINVAL;
    }

  if (!parse_int(tel_args(line), answer ? 1 : 0, &stat))
    {
      return -EINVAL;
    }

  return parse_reg(stat);
}

int tel_signal(FAR const char *line, FAR int32_t *dbm, FAR uint32_t *bars)
{
  long rssi;

  if (!parse_starts(line, "+CSQ:") || !parse_int(tel_args(line), 0, &rssi))
    {
      return -EINVAL;
    }

  if (rssi < 0 || rssi > 31)
    {
      *dbm  = 0;
      *bars = 0;
      return OK;
    }

  *dbm  = -113 + 2 * (int32_t)rssi;
  *bars = rssi >= 20 ? 4 : rssi >= 14 ? 3 : rssi >= 9 ? 2 :
          rssi >= 2 ? 1 : 0;
  return OK;
}

int tel_operator(FAR const char *line, FAR char *name, size_t size,
                 FAR int *tech)
{
  FAR const char *args = tel_args(line);
  long act;

  if (!parse_starts(line, "+COPS:"))
    {
      return -EINVAL;
    }

  if (size > 0)
    {
      name[0] = '\0';
    }

  *tech = PNUT_TELEPHONY_INFO_TECH_NO_TECH;

  /* <mode>[,<format>,<oper>[,<AcT>]] */

  if (!tel_field(args, 2, name, size))
    {
      return OK;
    }

  if (parse_int(args, 3, &act))
    {
      *tech = act == 7 || act == 9 ? PNUT_TELEPHONY_INFO_TECH_LTE :
              act == 0 || act == 1 || act == 3 ?
              PNUT_TELEPHONY_INFO_TECH_GSM :
              PNUT_TELEPHONY_INFO_TECH_NO_TECH;
    }

  return OK;
}

int tel_sim(FAR const char *line)
{
  FAR const char *args = tel_args(line);

  if (parse_starts(line, "+CPIN:"))
    {
      if (strcmp(args, "READY") == 0 || strcmp(args, "SIM PIN2") == 0 ||
          strcmp(args, "SIM PUK2") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_READY;
        }

      if (strcmp(args, "SIM PIN") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN;
        }

      if (strcmp(args, "SIM PUK") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK;
        }

      if (strstr(args, "NOT INSERTED") != NULL ||
          strstr(args, "NOT AVAILABLE") != NULL)
        {
          return PNUT_TELEPHONY_INFO_SIM_ABSENT;
        }

      return -EINVAL;
    }

  if (parse_starts(line, "+SIMCARD:"))
    {
      return strstr(args, "NOT AVAILABLE") != NULL ?
             PNUT_TELEPHONY_INFO_SIM_ABSENT : -EINVAL;
    }

  if (parse_starts(line, "+CME ERROR:"))
    {
      if (strstr(args, "not inserted") != NULL || strcmp(args, "10") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_ABSENT;
        }

      if (strstr(args, "PUK required") != NULL || strcmp(args, "12") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK;
        }

      if (strstr(args, "PIN required") != NULL || strcmp(args, "11") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN;
        }

      if (strstr(args, "blocked") != NULL)
        {
          return PNUT_TELEPHONY_INFO_SIM_BLOCKED;
        }

      if (strstr(args, "SIM failure") != NULL || strcmp(args, "13") == 0)
        {
          return PNUT_TELEPHONY_INFO_SIM_SIM_FAILED;
        }
    }

  return -EINVAL;
}

int tel_tries(FAR const char *line, FAR uint32_t *pin, FAR uint32_t *puk)
{
  FAR const char *args = tel_args(line);
  long a;
  long b;

  if (!parse_starts(line, "+SPIC:") || !parse_int(args, 0, &a) ||
      !parse_int(args, 1, &b) || a < 0 || b < 0)
    {
      return -EINVAL;
    }

  *pin = (uint32_t)a;
  *puk = (uint32_t)b;
  return OK;
}

int tel_sms_slot(FAR const char *line)
{
  char mem[8];
  long slot;

  if (!parse_starts(line, "+CMTI:") ||
      !tel_field(tel_args(line), 0, mem, sizeof(mem)) ||
      !parse_int(tel_args(line), 1, &slot) || slot < 0 ||
      slot > INT16_MAX)
    {
      return -EINVAL;
    }

  return strcmp(mem, "ME") == 0 ? (int)slot : -ENODEV;
}

int tel_sms_listed(FAR const char *line)
{
  long slot;
  long length;

  if (!parse_starts(line, "+CMGL:") || !parse_int(tel_args(line), 0, &slot)
      || !parse_int(tel_args(line), 3, &length) || slot < 0 ||
      slot > INT16_MAX || length <= 0)
    {
      return -EINVAL;
    }

  return (int)slot;
}

int tel_sms_length(FAR const char *line)
{
  long length;

  if (!parse_starts(line, "+CMGR:") ||
      !parse_int(tel_args(line), 2, &length) || length <= 0 ||
      length > 255)
    {
      return -EINVAL;
    }

  return (int)length;
}
