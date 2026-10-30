/****************************************************************************
 * pnut-os/src/system/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_BOARD_H
#define __PNUT_OS_SYSTEM_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

#include <pnut/module.h>

#include "pnut/board.pnut.h"
#include "pnut/board.settings.h"
#include "pnut/board.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's socket, in the run directory */

#define BOARD_SERVICE  "board"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board (RFC 0024), so far the description and airplane mode: a module
 * of the system program that reads the target's description, which the
 * build compiled from configs/<target>/board.toml, as it starts, and
 * serves it.  It keeps airplane mode (RFC 0030) as a setting of its own,
 * which the system UI switches, and announces it on the airplane_mode
 * topic.  Device data, the backlights, the lights, the privacy switches
 * and the USB port's mode come later; no unit is provisioned yet.
 * Without a description it serves none, and the program goes on.
 */

struct board_config_s
{
  FAR const char *path;           /* The description, or NULL */
};

struct board_s
{
  struct pnut_module_s module;    /* First: the module is the board */
  struct board_config_s config;
  struct pnut_board_server_s server;
  pnut_board_description_t desc;
  bool loaded;                    /* desc holds the description */
  bool serving;

  /* Airplane mode: the setting, and the topic it is announced on */

  struct pnut_board_settings_s settings;
  bool settings_open;
  FAR struct pnut_publisher_s *airplanes;
  bool announced;                 /* airplane has been announced */
  bool airplane;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop.
 *
 ****************************************************************************/

void board_init(FAR struct board_s *board,
                FAR const struct board_config_s *config);

#endif /* __PNUT_OS_SYSTEM_BOARD_H */
