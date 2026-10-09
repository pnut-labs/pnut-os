/****************************************************************************
 * pnut-os/src/system/settings.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_SETTINGS_H
#define __PNUT_OS_SYSTEM_SETTINGS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/settings.pnut.h"
#include "settings_store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The service's socket's name */

#define SETTINGS_SERVICE      "settings"

/* An owner's file and the one written beside it: <dir>/<owner>.pb */

#define SETTINGS_PATH_MAX     192

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct settings_config_s
{
  FAR const char *dir;            /* Where the owners' files are; kept */
  struct settings_limits_s limits;
  uint32_t delay;                 /* Milliseconds from a change to its write */
};

/* A write of an owner's file, on a worker.  The module does not touch it,
 * nor the buffer it names, until it has finished.
 */

struct settings_write_s
{
  char path[SETTINGS_PATH_MAX];
  char temp[SETTINGS_PATH_MAX];
  FAR const uint8_t *data;
  size_t len;
  int ret;
  bool finished;                  /* Set by the worker, last */
};

/* Settings (RFC 0025), a module of the system program.  The program fills
 * nothing in: settings_init() does.
 */

struct settings_s
{
  struct pnut_module_s module;    /* Must be first */

  /* The module's */

  struct settings_config_s config;
  struct settings_store_s store;
  struct pnut_settings_server_s server;
  FAR struct pnut_timer_s *timer; /* Until the next write */
  uint64_t timer_at;              /* When it expires */
  FAR uint8_t *buffer;            /* An owner's file, encoded */
  size_t size;
  struct settings_write_s write;
  char owner[SETTINGS_NAME_MAX + 1];  /* Whose file is being written */
  bool writing;
  bool serving;

  /* An answer, too large for the loop's stack */

  union
  {
    pnut_setting_value_t value;
    pnut_settings_values_t values;
    pnut_settings_schema_t schema;
  } out;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: settings_init
 *
 * Description:
 *   Make the module, to be added to the program's loop.  It takes its
 *   pools when the loop starts, and serves "settings".
 *
 ****************************************************************************/

void settings_init(FAR struct settings_s *settings,
                   FAR const struct settings_config_s *config);

/****************************************************************************
 * Name: settings_deinit
 *
 * Description:
 *   Give its pools back, once the loop is destroyed: a write may run on a
 *   worker until then.
 *
 ****************************************************************************/

void settings_deinit(FAR struct settings_s *settings);

#endif /* __PNUT_OS_SYSTEM_SETTINGS_H */
