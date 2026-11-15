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

/* A handler's budget: drawing a screen on the loop takes tens of ms on
 * the ESP32-S3 (25-60 ms measured), building one as long; over this the
 * library logs it
 */

#define UI_BUDGET_MS  250

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  FAR struct pnut_loop_s *loop;
  static struct input_s input;
  static struct ui_s ui;
  int ret;

  pnut_loop_defaults(&config);
  config.name    = "ui";
  config.levels  = true;
  config.workers = 0;
  config.jobs    = 0;
  config.budget  = UI_BUDGET_MS;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_ui: no loop: %d\n", ret);
      return EXIT_FAILURE;
    }

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &ui.display;
  input_init(&input, &iconfig);

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.board       = UI_BOARD;
  uconfig.input       = &input;
  uconfig.open_screen = fb_open;
  uconfig.open_input  = input_dev_open;
  ui_init(&ui, &uconfig);

  /* The UI first: it starts LVGL, which Input's devices belong to */

  ret = pnut_module_add(loop, &ui.module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &input.module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
