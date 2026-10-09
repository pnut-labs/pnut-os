/****************************************************************************
 * pnut-os/src/system/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system program (RFC 0007): the services every other program waits
 * on.  Settings is its first module.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "settings.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  FAR struct settings_s *settings;
  FAR struct pnut_loop_s *loop;
  int ret;

  settings = calloc(1, sizeof(*settings));
  if (settings == NULL)
    {
      fprintf(stderr, "pnut_system: no memory\n");
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name = "system";

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_system: no loop: %d\n", ret);
      free(settings);
      return EXIT_FAILURE;
    }

  sconfig.dir             = CONFIG_PNUT_SETTINGS_DIR;
  sconfig.limits.owners   = CONFIG_PNUT_SETTINGS_OWNERS;
  sconfig.limits.settings = CONFIG_PNUT_SETTINGS_MAX;
  sconfig.limits.texts    = CONFIG_PNUT_SETTINGS_TEXTS;
  sconfig.limits.choices  = CONFIG_PNUT_SETTINGS_CHOICES;
  sconfig.delay           = CONFIG_PNUT_SETTINGS_DELAY;
  settings_init(settings, &sconfig);

  ret = pnut_module_add(loop, &settings->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  /* A write may run on a worker until the loop is destroyed */

  pnut_loop_destroy(loop);
  settings_deinit(settings);
  free(settings);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
