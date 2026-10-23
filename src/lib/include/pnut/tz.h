/****************************************************************************
 * pnut-os/src/lib/include/pnut/tz.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_TZ_H
#define __PNUT_OS_LIB_PNUT_TZ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The longest zone abbreviation kept, without its NUL */

#define PNUT_TZ_ABBR_MAX  7

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Time zones (RFC 0028): each zone of the tz database with its current
 * rule, a POSIX TZ string (CET-1CEST,M3.5.0,M10.5.0/3), compiled into the
 * library.  Any program converts times in any zone by name; NuttX's
 * localtime(), which takes one zone for the whole system, is not used.
 */

/* When summer time starts or ends in a year */

struct pnut_tz_when_s
{
  uint8_t kind;                   /* 'M' month, week, day; 'J' or 'n' */
  uint8_t month;                  /* 1-12 */
  uint8_t week;                   /* 1-5, 5 the last */
  uint8_t wday;                   /* 0-6, Sunday first */
  uint16_t day;                   /* Jn: 1-365; n: 0-365 */
  int32_t time;                   /* Seconds into the local day */
};

/* A zone's rule, parsed */

struct pnut_tz_s
{
  int32_t std_off;                /* Seconds east of UTC */
  int32_t dst_off;                /* Likewise in summer time */
  bool has_dst;
  struct pnut_tz_when_s start;    /* In local standard time */
  struct pnut_tz_when_s end;      /* In local summer time */
  char std_abbr[PNUT_TZ_ABBR_MAX + 1];
  char dst_abbr[PNUT_TZ_ABBR_MAX + 1];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: pnut_tz_find
 *
 * Description:
 *   A zone's current rule, by its name (Europe/Warsaw), or NULL for a name
 *   the table does not have.
 *
 ****************************************************************************/

FAR const char *pnut_tz_find(FAR const char *zone);

/****************************************************************************
 * Name: pnut_tz_zone
 *
 * Description:
 *   The table's zones in the order of their names, for a list to pick
 *   from: the name of the zone at index, or NULL past the last.
 *
 ****************************************************************************/

FAR const char *pnut_tz_zone(size_t index);

/****************************************************************************
 * Name: pnut_tz_parse
 *
 * Description:
 *   Parse a POSIX TZ rule (RFC 8536's form, which allows hours to 167 and
 *   negative transition times).
 *
 * Returned Value:
 *   Zero (OK), or -EINVAL for a rule that does not parse.
 *
 ****************************************************************************/

int pnut_tz_parse(FAR const char *rule, FAR struct pnut_tz_s *tz);

/****************************************************************************
 * Name: pnut_tz_local
 *
 * Description:
 *   The wall time of an instant in a zone: *local filled as gmtime_r()
 *   would for the wall time, with tm_isdst, tm_gmtoff and tm_zone set
 *   (tm_zone points into *tz); and the offset east of UTC, in seconds, in
 *   *offp if it is not NULL.  tm_isdst says the rule's second offset is in
 *   force, which for Europe/Dublin is winter's: show the offset or the
 *   abbreviation, never "summer time" from it.
 *
 ****************************************************************************/

void pnut_tz_local(FAR const struct pnut_tz_s *tz, time_t utc,
                   FAR struct tm *local, FAR int32_t *offp);

/****************************************************************************
 * Name: pnut_tz_utc
 *
 * Description:
 *   The instant of a wall time in a zone (tm_isdst is ignored).  A wall
 *   time that does not exist, in the hour skipped when summer time
 *   starts, moves forward by the gap; one that happens twice, when it
 *   ends, takes the first.
 *
 ****************************************************************************/

time_t pnut_tz_utc(FAR const struct pnut_tz_s *tz,
                   FAR const struct tm *local);

#ifdef __cplusplus
}
#endif

#endif /* __PNUT_OS_LIB_PNUT_TZ_H */
