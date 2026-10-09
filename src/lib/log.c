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

#include <stdarg.h>
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

  vsnprintf(line, sizeof(line), fmt, ap);

#ifdef __NuttX__
  syslog(g_priority[level], "%s: %s\n", name, line);
#else
  fprintf(stderr, "%s: %s\n", name, line);
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pnut_vlog(FAR const struct pnut_module_s *module, int level,
               FAR const char *fmt, va_list ap)
{
  if (level <= module->level)
    {
      log_line(module->name, level, fmt, ap);
    }
}

void pnut_log(FAR const struct pnut_module_s *module, int level,
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
 * Name: pnut_loop_log
 *
 * Description:
 *   A line for the loop itself, marked with the program's name; the loop
 *   logs info and above.
 *
 ****************************************************************************/

void pnut_loop_log(FAR struct pnut_loop_s *loop, int level,
                   FAR const char *fmt, ...)
{
  va_list ap;

  if (level <= PNUT_LOG_INFO)
    {
      va_start(ap, fmt);
      log_line(loop->config.name, level, fmt, ap);
      va_end(ap);
    }
}
