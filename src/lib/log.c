/****************************************************************************
 * pnut-os/src/lib/log.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __NuttX__
#  include <syslog.h>
#endif

#include "pnut_internal.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef __NuttX__
static const int g_priority[] =
{
  LOG_ERR,                        /* PNUT_LOG_ERROR */
  LOG_WARNING,                    /* PNUT_LOG_WARNING */
  LOG_INFO,                       /* PNUT_LOG_INFO */
  LOG_DEBUG,                      /* PNUT_LOG_DEBUG */
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: log_clean
 *
 * Description:
 *   Make a formatted message one line: control characters become spaces,
 *   and a character cut short at the end goes.  len is what vsnprintf()
 *   said it wanted.
 *
 ****************************************************************************/

static void log_clean(FAR char *line, size_t size, int len)
{
  size_t end;
  size_t lead;
  size_t i;
  uint8_t c;

  if (len < 0)
    {
      line[0] = '\0';
      return;
    }

  end = (size_t)len < size ? (size_t)len : size - 1;

  for (i = 0; i < end; i++)
    {
      c = (uint8_t)line[i];
      if (c < 0x20 || c == 0x7f)
        {
          line[i] = ' ';
        }
    }

  if ((size_t)len < size || end == 0)
    {
      return;
    }

  /* Cut: back over the last character's continuation bytes to its lead,
   * which says how long the character should be
   */

  lead = end;
  while (lead > 0 && ((uint8_t)line[lead - 1] & 0xc0) == 0x80)
    {
      lead--;
    }

  if (lead == 0)
    {
      return;
    }

  c = (uint8_t)line[lead - 1];
  if (c >= 0xc0 &&
      end - (lead - 1) < (c >= 0xf0 ? 4u : c >= 0xe0 ? 3u : 2u))
    {
      line[lead - 1] = '\0';
    }
}

/****************************************************************************
 * Name: log_line
 *
 * Description:
 *   Write a line, marked with a name: to the system log on NuttX, to the
 *   standard error on the computer.
 *
 ****************************************************************************/

static void log_line(FAR const char *name, int level, FAR const char *fmt,
                     va_list ap)
{
  char line[CONFIG_PNUT_LIB_LOG_LINE];

  if (level < PNUT_LOG_ERROR)
    {
      level = PNUT_LOG_ERROR;
    }
  else if (level > PNUT_LOG_DEBUG)
    {
      level = PNUT_LOG_DEBUG;
    }

  log_clean(line, sizeof(line), vsnprintf(line, sizeof(line), fmt, ap));

#ifdef __NuttX__
  syslog(g_priority[level], "%s: %s\n", name, line);
#else
  fprintf(stderr, "%s: %s\n", name, line);
#endif
}

/****************************************************************************
 * Name: log_linef
 *
 * Description:
 *   log_line(), with the message's arguments.
 *
 ****************************************************************************/

static void log_linef(FAR const char *name, int level,
                      FAR const char *fmt, ...) printf_like(3, 4);

static void log_linef(FAR const char *name, int level,
                      FAR const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  log_line(name, level, fmt, ap);
  va_end(ap);
}

/****************************************************************************
 * Name: log_admit
 *
 * Description:
 *   Whether a module, or the loop, may log one more line this second.  A
 *   new second first says how many lines the last ones dropped.
 *
 ****************************************************************************/

static bool log_admit(FAR struct pnut_lograte_s *rate, FAR const char *name)
{
#if CONFIG_PNUT_LIB_LOG_RATE > 0
  uint64_t now = pnut_now();

  if (now - rate->second >= 1000)
    {
      pnut_log_dropped(rate, name);
      rate->second = now;
      rate->lines  = 0;
    }

  if (rate->lines >= CONFIG_PNUT_LIB_LOG_RATE)
    {
      if (rate->dropped < UINT32_MAX)
        {
          rate->dropped++;
        }

      return false;
    }

  rate->lines++;
#endif

  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pnut_vlog(FAR struct pnut_module_s *module, int level,
               FAR const char *fmt, va_list ap)
{
  if (level <= module->level && log_admit(&module->rate, module->name))
    {
      log_line(module->name, level, fmt, ap);
    }
}

void pnut_log(FAR struct pnut_module_s *module, int level,
              FAR const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  pnut_vlog(module, level, fmt, ap);
  va_end(ap);
}

void pnut_module_setlevel(FAR struct pnut_module_s *module, int level)
{
  module->level = level;
}

/****************************************************************************
 * Name: pnut_log_dropped
 *
 * Description:
 *   Say how many lines were dropped, if any were: a warning, whatever the
 *   level, and not counted against the rate.
 *
 ****************************************************************************/

void pnut_log_dropped(FAR struct pnut_lograte_s *rate, FAR const char *name)
{
  if (rate->dropped > 0)
    {
      log_linef(name, PNUT_LOG_WARNING,
                "Lines dropped, over %d a second: %" PRIu32,
                CONFIG_PNUT_LIB_LOG_RATE, rate->dropped);
      rate->dropped = 0;
    }
}

/****************************************************************************
 * Name: pnut_loop_log
 *
 * Description:
 *   A line for the loop itself, marked with the program's name; the loop
 *   logs info and above, at the modules' rate.
 *
 ****************************************************************************/

void pnut_loop_log(FAR struct pnut_loop_s *loop, int level,
                   FAR const char *fmt, ...)
{
  va_list ap;

  if (level <= PNUT_LOG_INFO && log_admit(&loop->rate, loop->config.name))
    {
      va_start(ap, fmt);
      log_line(loop->config.name, level, fmt, ap);
      va_end(ap);
    }
}
