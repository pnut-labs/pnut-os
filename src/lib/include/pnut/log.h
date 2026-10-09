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
 *   message has no newline, and is cut at CONFIG_PNUT_LIB_LOG_LINE
 *   characters.
 *
 ****************************************************************************/

void pnut_log(FAR const struct pnut_module_s *module, int level,
              FAR const char *fmt, ...) printf_like(3, 4);

void pnut_vlog(FAR const struct pnut_module_s *module, int level,
               FAR const char *fmt, va_list ap) printf_like(3, 0);

/****************************************************************************
 * Name: pnut_module_setlevel
 *
 * Description:
 *   Set the least important level a module logs; info by default.
 *
 ****************************************************************************/

void pnut_module_setlevel(FAR struct pnut_module_s *module, int level);

#endif /* __PNUT_OS_LIB_PNUT_LOG_H */
