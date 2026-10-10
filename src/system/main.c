/****************************************************************************
 * pnut-os/src/system/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system program (RFC 0007): the services every other program waits
 * on.  Service states comes first, since the others ask it who calls;
 * then Settings.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "settings.h"
#include "states.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Settings asks Service states who calls (RFC 0006) */

static int system_who(FAR void *arg, pid_t pid,
                      FAR const char **programp)
{
  return states_who(arg, pid, programp);
}

static bool system_runs(FAR void *arg, FAR const char *program,
                        FAR const char *service)
{
  return states_runs(arg, program, service);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct states_config_s tconfig;
  FAR struct settings_s *settings;
  FAR struct states_s *states;
  FAR struct pnut_loop_s *loop;
  int ret;

  settings = calloc(1, sizeof(*settings));
  states   = calloc(1, sizeof(*states));
  if (settings == NULL || states == NULL)
    {
      fprintf(stderr, "pnut_system: no memory\n");
      free(settings);
      free(states);
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name = "system";

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_system: no loop: %d\n", ret);
      free(settings);
      free(states);
      return EXIT_FAILURE;
    }

  tconfig.initctl  = config.initctl;
  tconfig.programs = CONFIG_PNUT_PROGRAMS_PATH;
  tconfig.services = CONFIG_PNUT_STATES_SERVICES;
  states_init(states, &tconfig);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir             = CONFIG_PNUT_SETTINGS_DIR;
  sconfig.limits.owners   = CONFIG_PNUT_SETTINGS_OWNERS;
  sconfig.limits.settings = CONFIG_PNUT_SETTINGS_MAX;
  sconfig.limits.texts    = CONFIG_PNUT_SETTINGS_TEXTS;
  sconfig.limits.choices  = CONFIG_PNUT_SETTINGS_CHOICES;
  sconfig.delay           = CONFIG_PNUT_SETTINGS_DELAY;
  sconfig.who             = system_who;
  sconfig.runs            = system_runs;
  sconfig.identity        = states;
  settings_init(settings, &sconfig);

  ret = pnut_module_add(loop, &states->module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &settings->module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  /* A write may run on a worker until the loop is destroyed */

  pnut_loop_destroy(loop);
  settings_deinit(settings);
  states_deinit(states);
  free(settings);
  free(states);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
