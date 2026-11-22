/****************************************************************************
 * pnut-os/src/ui/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The ui program (RFC 0007): the system UI (RFC 0019) and Input
 * (RFC 0027).  NxInit starts it once the system program is ready.
 * Services know it by its name, "ui", and let it do what the owner may.
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

#include "fb.h"
#include "input.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's service, in the system program: the screen, the input devices */

#define UI_BOARD  "board"

/* Apps (RFC 0037): their manifests, and Service states' service, which
 * names them
 */

#define UI_APPS      "/etc/apps"
#define UI_SERVICES  "services"

/* A handler's budget: drawing a screen on the loop takes tens of ms on
 * the ESP32-S3 (25-60 ms measured), building one as long; over this the
 * library logs it
 */

#define UI_BUDGET_MS  250

/* Timers at once: the UI's own, its clients' and services', and up to
 * four for each of the apps' HOST_APPS slots (its Busy, a deadline, an
 * answer owed, events waiting); from the heap
 */

#define UI_TIMERS     48

/* Descriptors: four apps' connections, services topic, screens' clients */

#define UI_FDS        24

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  FAR struct pnut_loop_s *loop;
  FAR struct input_s *input;
  FAR struct ui_s *ui;
  int ret;

  pnut_loop_defaults(&config);
  config.name    = "ui";
  config.levels  = true;
  config.workers = 0;
  config.jobs    = 0;
  config.budget  = UI_BUDGET_MS;
  config.timers  = UI_TIMERS;
  config.fds     = UI_FDS;

  /* The program's state is large (the apps' requests among it): from the
   * heap, in external memory, not in the internal memory statics take
   */

  ui    = calloc(1, sizeof(*ui));
  input = calloc(1, sizeof(*input));
  if (ui == NULL || input == NULL)
    {
      fprintf(stderr, "pnut_ui: no memory\n");
      free(ui);
      free(input);
      return EXIT_FAILURE;
    }

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_ui: no loop: %d\n", ret);
      free(ui);
      free(input);
      return EXIT_FAILURE;
    }

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &ui->display;
  input_init(input, &iconfig);

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.board       = UI_BOARD;
  uconfig.input       = input;
  uconfig.open_screen = fb_open;
  uconfig.open_input  = input_dev_open;
  uconfig.apps        = UI_APPS;
  uconfig.services    = UI_SERVICES;
  ui_init(ui, &uconfig);

  /* The UI first: it starts LVGL, which Input's devices belong to */

  ret = pnut_module_add(loop, &ui->module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &input->module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  free(input);
  free(ui);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
