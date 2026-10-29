/****************************************************************************
 * pnut-os/src/lib/include/pnut/log.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_LOG_H
#define __PNUT_OS_LIB_PNUT_LOG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdarg.h>

#include <pnut/compiler.h>
#include <pnut/module.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The levels of RFC 0026, most important first */

enum pnut_level_e
{
  PNUT_LOG_ERROR = 0,             /* Something failed; someone should look */
  PNUT_LOG_WARNING,               /* Something unexpected, handled */
  PNUT_LOG_INFO,                  /* What the system did */
  PNUT_LOG_DEBUG,                 /* What a developer needs to follow */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_log
 *
 * Description:
 *   Log a line for a module, to the system log, marked with the module's
 *   name; dropped if level is less important than the module's.  The
 *   message has no newline: control characters in it become spaces, so
 *   that it stays one line, and it is cut short of
 *   CONFIG_PNUT_LIB_LOG_LINE bytes, at a whole character.  A module
 *   logging more than CONFIG_PNUT_LIB_LOG_RATE lines a second has the
 *   rest of that second's lines dropped, and a warning says how many,
 *   whatever the module's level: with its next line logged after the
 *   second, or as it stops.  RFC 0026 limits each service or app; the
 *   library limits each module, and the loop's own lines alike.
 *
 ****************************************************************************/

void pnut_log(FAR struct pnut_module_s *module, int level,
              FAR const char *fmt, ...) printf_like(3, 4);

void pnut_vlog(FAR struct pnut_module_s *module, int level,
               FAR const char *fmt, va_list ap) printf_like(3, 0);

/****************************************************************************
 * Name: pnut_module_setlevel
 *
 * Description:
 *   Set the least important level a module logs; info by default.  With
 *   the loop's levels, the setting PNUT_SETTINGS_CACHE_LEVEL sets it once
 *   Settings' value is read, and whenever it changes (RFC 0026).
 *
 ****************************************************************************/

void pnut_module_setlevel(FAR struct pnut_module_s *module, int level);

#endif /* __PNUT_OS_LIB_PNUT_LOG_H */
