/****************************************************************************
 * pnut-os/src/lib/include/pnut/module.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_MODULE_H
#define __PNUT_OS_LIB_PNUT_MODULE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct pnut_settings_cache_s;

/* The library's count of log lines in the second so far, and of those
 * dropped over the rate (RFC 0026), for a module and for its loop
 */

struct pnut_lograte_s
{
  uint64_t second;                /* When the second began, ms */
  uint32_t dropped;               /* Lines dropped, not told yet */
  uint16_t lines;                 /* Lines logged in the second */
};

/* A part of a program: a service, or a client of one (RFC 0004).  The
 * program fills in the first fields and adds the module to its loop; the
 * library keeps the rest.
 */

struct pnut_module_s
{
  FAR const char *name;

  /* Called on the loop when it starts; a negated errno value stops the
   * program, and the module undoes what it set up (watches, timers)
   * itself.  The module calls pnut_module_ready() once it can serve, here
   * or later.
   */

  CODE int (*start)(FAR struct pnut_module_s *module,
                    FAR struct pnut_loop_s *loop);

  /* Called on the loop when it stops, if the module started */

  CODE void (*stop)(FAR struct pnut_module_s *module,
                    FAR struct pnut_loop_s *loop);

  /* Called on the loop when it is asked to stop, before any module stops,
   * for work that has to end in order first: a modem powered down
   * (RFC 0030).  It returns zero (OK) with nothing to finish, or
   * -EINPROGRESS and calls pnut_module_finished() once done; the loop
   * runs on meanwhile, for the program's finish time at most
   * (pnut_loop_config_s).  NULL for none.
   */

  CODE int (*finish)(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop);

  /* Kept by the library */

  FAR struct pnut_module_s *next;
  FAR struct pnut_module_s *prev;
  FAR struct pnut_loop_s *loop;
  struct pnut_lograte_s rate;
  uint8_t level;                  /* The least important level logged */

  /* The cache of its level alone, if the library opened one (RFC 0026) */

  FAR struct pnut_settings_cache_s *levels;
  bool started;
  bool ready;
  bool finishing;                 /* Its finish has not ended yet */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_module_add
 *
 * Description:
 *   Add a module to a loop, before pnut_loop_run().  Its log level is set
 *   to info here: call pnut_module_setlevel() after.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY if the loop runs already.
 *
 ****************************************************************************/

int pnut_module_add(FAR struct pnut_loop_s *loop,
                    FAR struct pnut_module_s *module);

/****************************************************************************
 * Name: pnut_module_ready
 *
 * Description:
 *   Say that the module can serve.  The program is ready once every module
 *   is (RFC 0006).
 *
 ****************************************************************************/

void pnut_module_ready(FAR struct pnut_module_s *module);

/****************************************************************************
 * Name: pnut_loop_ready
 *
 * Description:
 *   Whether every module of the loop is ready.
 *
 ****************************************************************************/

bool pnut_loop_ready(FAR struct pnut_loop_s *loop);

/****************************************************************************
 * Name: pnut_module_finished
 *
 * Description:
 *   Say that the module has finished what its finish handler began: the
 *   loop stops once every module has.
 *
 ****************************************************************************/

void pnut_module_finished(FAR struct pnut_module_s *module);

#endif /* __PNUT_OS_LIB_PNUT_MODULE_H */
