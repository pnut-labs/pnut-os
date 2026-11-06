/****************************************************************************
 * pnut-os/src/data/storage.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_DATA_STORAGE_H
#define __PNUT_OS_DATA_STORAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/module.h>
#include <pnut/topic.h>
#include <pnut/worker.h>

#include "pnut/board.pnut.h"
#include "pnut/storage.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STORAGE_SLOTS     2       /* As many as the storage topic holds */
#define STORAGE_DEVICE    24      /* A slot's device file, as Board's */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Mount a slot's card at path, if it is FAT, and give its size; one
 * mounted there already is taken as it is.  Zero, or a negated errno
 * value: -ENODEV (or -ENOENT, -ENXIO, -ENOMEDIUM) for no card, -EINVAL
 * for a card that is not FAT.  It never formats.
 */

typedef CODE int (*storage_mount_t)(FAR void *arg, FAR const char *device,
                                    FAR const char *path,
                                    FAR uint32_t *size_mb);

struct storage_config_s
{
  /* Board's service: the slots are the description's storage slots
   * (RFC 0024).  NULL: device, the one slot, or none when NULL too.
   */

  FAR const char *board;
  FAR const char *device;

  FAR const char *media;          /* Where storage is mounted: /media */

  /* The system's mount, for FAT, when NULL (the tests give their own) */

  storage_mount_t mount;
  FAR void *mount_arg;
};

/* A slot, and its card's state */

struct storage_slot_s
{
  char device[STORAGE_DEVICE];
  pnut_storage_volume_t volume;
};

/* The mounts, on a worker */

struct storage_work_s
{
  storage_mount_t mount;
  FAR void *mount_arg;
  uint8_t count;
  struct storage_slot_s slots[STORAGE_SLOTS];
};

/* Storage (RFC 0011), so far its first part: a module of the data program
 * that mounts the device's external storage under /media as it starts
 * (RFC 0010), FAT only, and announces it on the storage topic.  The
 * slots come from Board's description.  It writes nothing on a card.
 *
 * A card is looked for once, at the start: the T-Deck Max's slot has no
 * card-detect line, so a card put in later is found at the next start.
 * Storage never unmounts: Log, in the system program, which stops after
 * this one, may still be writing its copy there, and each of its writes
 * closes the file, which leaves FAT whole.
 */

struct storage_s
{
  struct pnut_module_s module;    /* First: the module is the storage */
  struct storage_config_s config;

  struct pnut_board_client_s board;
  bool board_open;                /* Asking Board for the description */
  FAR struct pnut_publisher_s *topic;

  uint8_t count;                  /* Slots */
  struct storage_slot_s slots[STORAGE_SLOTS];

  struct storage_work_s work;
  bool working;
  bool stopping;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: storage_init
 *
 * Description:
 *   Set Storage up from config, whose strings it keeps pointing to; add
 *   its module to a loop to run it.
 *
 ****************************************************************************/

void storage_init(FAR struct storage_s *storage,
                  FAR const struct storage_config_s *config);

/****************************************************************************
 * Name: storage_mount_fat
 *
 * Description:
 *   The system's mount: a card at device mounted at path as FAT, unless
 *   FAT is mounted there already.  A storage_mount_t.
 *
 ****************************************************************************/

int storage_mount_fat(FAR void *arg, FAR const char *device,
                      FAR const char *path, FAR uint32_t *size_mb);

#ifdef __cplusplus
}
#endif

#endif /* __PNUT_OS_DATA_STORAGE_H */
