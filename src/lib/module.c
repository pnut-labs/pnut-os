/****************************************************************************
 * pnut-os/src/lib/module.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: module_checkready
 *
 * Description:
 *   The program is ready once every module is: the loop logs it, and
 *   tells NxInit (RFC 0006).
 *
 ****************************************************************************/

static void module_checkready(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_module_s *module;

  if (loop->ready)
    {
      return;
    }

  for (module = loop->first; module != NULL; module = module->next)
    {
      if (!module->ready)
        {
          return;
        }
    }

  loop->ready = true;
  pnut_loop_log(loop, PNUT_LOG_INFO, "Ready");
  pnut_ready_tell(loop);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_module_add(FAR struct pnut_loop_s *loop,
                    FAR struct pnut_module_s *module)
{
  if (loop->running)
    {
      return -EBUSY;
    }

  module->loop    = loop;
  module->level   = PNUT_LOG_INFO;
  module->started = false;
  module->ready   = false;
  module->next    = NULL;
  module->prev    = loop->last;

  if (loop->last != NULL)
    {
      loop->last->next = module;
    }
  else
    {
      loop->first = module;
    }

  loop->last = module;
  return OK;
}

void pnut_module_ready(FAR struct pnut_module_s *module)
{
  if (!module->ready)
    {
      module->ready = true;
      if (module->loop->running)
        {
          module_checkready(module->loop);
        }
    }
}

bool pnut_loop_ready(FAR struct pnut_loop_s *loop)
{
  return loop->ready;
}

/****************************************************************************
 * Name: pnut_module_startall
 *
 * Description:
 *   Start every module, in order.  If one fails, those started are stopped
 *   again, in reverse order.
 *
 ****************************************************************************/

int pnut_module_startall(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_module_s *module;
  int ret;

  loop->ready = false;

  for (module = loop->first; module != NULL; module = module->next)
    {
      if (module->start != NULL)
        {
          ret = module->start(module, loop);
          if (ret < 0)
            {
              pnut_log(module, PNUT_LOG_ERROR, "Failed to start: %d", ret);
              pnut_module_stopall(loop);
              return ret;
            }
        }

      module->started = true;
    }

  module_checkready(loop);
  return OK;
}

/****************************************************************************
 * Name: pnut_module_stopall
 *
 * Description:
 *   Stop every module that started, in reverse order.
 *
 ****************************************************************************/

void pnut_module_stopall(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_module_s *module;

  pnut_ready_stop(loop);

  for (module = loop->last; module != NULL; module = module->prev)
    {
      if (module->started)
        {
          module->started = false;
          if (module->stop != NULL)
            {
              module->stop(module, loop);
            }
        }

      module->ready = false;
    }

  loop->ready = false;
}
