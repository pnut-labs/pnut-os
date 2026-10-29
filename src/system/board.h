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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's socket, in the run directory */

#define BOARD_SERVICE  "board"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board (RFC 0024), so far the description: a module of the system
 * program that reads the target's description, which the build compiled
 * from configs/<target>/board.toml, as it starts, and serves it.  Device
 * data, the backlights, the lights and the USB port's mode come later; no
 * unit is provisioned yet.  Without a description it serves none, and the
 * program goes on.
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
