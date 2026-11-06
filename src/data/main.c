/****************************************************************************
 * pnut-os/src/data/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "messages.h"
#include "storage.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Service states' and Board's services, in the system program */

#define DATA_SERVICES     "services"
#define DATA_BOARD        "board"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The data program (RFC 0007): Messages, and Storage's first part; later
 * Contacts and Packages.  It starts once the system program is ready.
 */

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct messages_config_s mconfig;
  struct storage_config_s sconfig;
  FAR struct messages_s *messages;
  FAR struct storage_s *storage;
  FAR struct pnut_loop_s *loop;
  int ret;

  /* Large (the store's work holds a page of messages): on the heap */

  messages = calloc(1, sizeof(*messages));
  storage  = calloc(1, sizeof(*storage));
  if (messages == NULL || storage == NULL)
    {
      fprintf(stderr, "pnut_data: no memory\n");
      free(messages);
      free(storage);
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name      = "data";
  config.levels    = true;
  config.stacksize = CONFIG_PNUT_DATA_WORKER_STACKSIZE;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_data: no loop: %d\n", ret);
      free(messages);
      free(storage);
      return EXIT_FAILURE;
    }

  memset(&mconfig, 0, sizeof(mconfig));
  mconfig.dir      = CONFIG_PNUT_MESSAGES_DIR;
  mconfig.limit    = (uint64_t)CONFIG_PNUT_MESSAGES_LIMIT * 1024;
  mconfig.segmax   = STORE_SEGMENT_MAX;
  mconfig.services = DATA_SERVICES;
  messages_init(messages, &mconfig);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.board = DATA_BOARD;
  sconfig.media = CONFIG_PNUT_STORAGE_MEDIA;
  storage_init(storage, &sconfig);

  ret = pnut_module_add(loop, &messages->module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &storage->module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  messages_deinit(messages);
  free(messages);
  free(storage);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
