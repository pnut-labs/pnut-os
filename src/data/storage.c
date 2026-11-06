/****************************************************************************
 * pnut-os/src/data/storage.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <sys/mount.h>
#include <sys/statfs.h>

#include <pnut/log.h>
#include <pnut/loop.h>

#include "storage.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A card slot's storage is named sd<n> (RFC 0010) */

#define STORAGE_KIND      "sd"

#ifndef MSDOS_SUPER_MAGIC
#  define MSDOS_SUPER_MAGIC 0x4d44
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void storage_board(FAR struct pnut_board_client_s *client,
                          bool connected, FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: storage_mount_run
 *
 * Description:
 *   Each slot's card mounted, on a worker: a card's first reads, over SPI,
 *   take a while.
 *
 ****************************************************************************/

static void storage_mount_run(FAR void *arg)
{
  FAR struct storage_work_s *work = arg;
  FAR pnut_storage_volume_t *volume;
  uint32_t size_mb;
  uint8_t i;
  int ret;

  for (i = 0; i < work->count; i++)
    {
      volume  = &work->slots[i].volume;
      size_mb = 0;
      ret     = work->mount(work->mount_arg, work->slots[i].device,
                            volume->path, &size_mb);
      switch (ret)
        {
          case OK:
            volume->state   = PNUT_STORAGE_VOLUME_STATE_MOUNTED;
            volume->size_mb = size_mb;
            break;

          case -ENODEV:
          case -ENOENT:
          case -ENXIO:
#ifdef ENOMEDIUM
          case -ENOMEDIUM:
#endif
            volume->state = PNUT_STORAGE_VOLUME_STATE_ABSENT;
            break;

          case -EINVAL:
            volume->state = PNUT_STORAGE_VOLUME_STATE_UNSUPPORTED;
            break;

          default:
            volume->state = PNUT_STORAGE_VOLUME_STATE_FAILED;
            break;
        }

      /* The error, for the log, in the size's place until done */

      if (ret < 0)
        {
          volume->size_mb = (uint32_t)-ret;
        }
    }
}

/****************************************************************************
 * Name: storage_publish
 ****************************************************************************/

static void storage_publish(FAR struct storage_s *storage)
{
  pnut_storage_info_t info;
  uint8_t i;
  int ret;

  if (storage->topic == NULL)
    {
      return;
    }

  memset(&info, 0, sizeof(info));
  info.volumes_count = storage->count;
  for (i = 0; i < storage->count; i++)
    {
      info.volumes[i] = storage->slots[i].volume;
    }

  ret = pnut_storage_info_publish(storage->topic, &info);
  if (ret < 0)
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "Cannot announce the storage: %d", ret);
    }
}

/****************************************************************************
 * Name: storage_mount_done
 ****************************************************************************/

static void storage_mount_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct storage_s *storage =
    (FAR struct storage_s *)((FAR char *)arg -
                             offsetof(struct storage_s, work));
  FAR pnut_storage_volume_t *volume;
  uint8_t i;

  storage->working = false;
  if (storage->stopping)
    {
      return;
    }

  for (i = 0; i < storage->count; i++)
    {
      volume = &storage->work.slots[i].volume;
      switch (volume->state)
        {
          case PNUT_STORAGE_VOLUME_STATE_MOUNTED:
            pnut_log(&storage->module, PNUT_LOG_INFO,
                     "%s: mounted at %s, %" PRIu32 " MB", volume->name,
                     volume->path, volume->size_mb);
            break;

          case PNUT_STORAGE_VOLUME_STATE_ABSENT:
            pnut_log(&storage->module, PNUT_LOG_INFO, "%s: no card: %d",
                     volume->name, -(int)volume->size_mb);
            volume->size_mb = 0;
            break;

          case PNUT_STORAGE_VOLUME_STATE_UNSUPPORTED:
            pnut_log(&storage->module, PNUT_LOG_WARNING,
                     "%s: not FAT; left as it is", volume->name);
            volume->size_mb = 0;
            break;

          default:
            pnut_log(&storage->module, PNUT_LOG_WARNING,
                     "%s: cannot be mounted: %d", volume->name,
                     -(int)volume->size_mb);
            volume->size_mb = 0;
            break;
        }

      storage->slots[i].volume = *volume;
    }

  storage_publish(storage);
}

/****************************************************************************
 * Name: storage_mount
 *
 * Description:
 *   The slots known: their cards mounted on a worker, then announced.
 *
 ****************************************************************************/

static void storage_mount(FAR struct storage_s *storage)
{
  FAR struct storage_work_s *work = &storage->work;
  int ret;

  /* Every slot announced at once, not yet looked into */

  storage_publish(storage);
  if (storage->count == 0 || storage->working)
    {
      return;
    }

  memset(work, 0, sizeof(*work));
  work->mount     = storage->config.mount != NULL ?
                    storage->config.mount : storage_mount_fat;
  work->mount_arg = storage->config.mount_arg;
  work->count     = storage->count;
  memcpy(work->slots, storage->slots, sizeof(work->slots));

  ret = pnut_job_submit(storage->module.loop, storage_mount_run,
                        storage_mount_done, work);
  if (ret < 0)
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "Cannot mount the storage: %d", ret);
      return;
    }

  storage->working = true;
}

/****************************************************************************
 * Name: storage_slot
 *
 * Description:
 *   A slot added, as sd<n>, at <media>/sd<n>.
 *
 ****************************************************************************/

static bool storage_slot(FAR struct storage_s *storage,
                         FAR const char *device)
{
  FAR struct storage_slot_s *slot;
  int n;

  if (storage->count >= STORAGE_SLOTS)
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "More than %d slots: %s left out", STORAGE_SLOTS, device);
      return false;
    }

  slot = &storage->slots[storage->count];
  memset(slot, 0, sizeof(*slot));
  n = snprintf(slot->volume.name, sizeof(slot->volume.name), "%s%u",
               STORAGE_KIND, (unsigned)storage->count);
  if (n < 0 || n >= (int)sizeof(slot->volume.name) ||
      strlcpy(slot->device, device, sizeof(slot->device)) >=
      sizeof(slot->device))
    {
      return false;
    }

  n = snprintf(slot->volume.path, sizeof(slot->volume.path), "%s/%s%u",
               storage->config.media, STORAGE_KIND,
               (unsigned)storage->count);
  if (n < 0 || n >= (int)sizeof(slot->volume.path))
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "No room for %s's path under %s", slot->volume.name,
               storage->config.media);
      return false;
    }

  slot->volume.state = PNUT_STORAGE_VOLUME_STATE_ABSENT;
  storage->count++;
  return true;
}

/****************************************************************************
 * Name: storage_described
 *
 * Description:
 *   Board's description came: its storage slots, in its order.  Board is
 *   asked once: the description does not change while the device runs.
 *
 ****************************************************************************/

static void storage_described(FAR struct pnut_board_client_s *client,
                              int status,
                              FAR const pnut_board_description_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct storage_s *storage = arg;
  pb_size_t i;

  /* No answer: the connection was lost, and the library connects again,
   * when Board is asked again; or Board did not answer in time
   */

  if (status == PNUT_STATUS_TIMEOUT ||
      (status == PNUT_STATUS_UNAVAILABLE && err == NULL))
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "No answer from Board: status %d; asking again", status);
      if (status == PNUT_STATUS_TIMEOUT)
        {
          storage_board(client, true, storage);
        }

      return;
    }

  pnut_board_disconnect(&storage->board);
  storage->board_open = false;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      pnut_log(&storage->module, status == PNUT_STATUS_UNAVAILABLE ?
               PNUT_LOG_INFO : PNUT_LOG_WARNING,
               "No description from Board: status %d; no storage",
               status);
      storage_publish(storage);
      return;
    }

  for (i = 0; i < out->parts_count; i++)
    {
      if (out->parts[i].kind == PNUT_BOARD_PART_KIND_STORAGE_SLOT &&
          out->parts[i].device[0] != '\0')
        {
          storage_slot(storage, out->parts[i].device);
        }
    }

  storage_mount(storage);
}

/****************************************************************************
 * Name: storage_board
 ****************************************************************************/

static void storage_board(FAR struct pnut_board_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct storage_s *storage = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, storage_described, storage);
  if (ret < 0)
    {
      pnut_log(&storage->module, PNUT_LOG_WARNING,
               "Cannot ask Board: %d; no storage", ret);
    }
}

/****************************************************************************
 * Name: storage_start
 ****************************************************************************/

static int storage_start(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct storage_s *storage = (FAR struct storage_s *)module;
  int ret;

  storage->stopping = false;
  storage->count    = 0;

  ret = pnut_storage_info_advertise(loop, &storage->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No storage topic: %d", ret);
      storage->topic = NULL;
    }

  /* Nothing to wait for: Storage serves nothing so far */

  pnut_module_ready(module);

  if (storage->config.board == NULL)
    {
      if (storage->config.device != NULL)
        {
          storage_slot(storage, storage->config.device);
        }

      storage_mount(storage);
      return OK;
    }

  ret = pnut_board_connect(loop, storage->config.board, storage_board,
                           storage, &storage->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No Board: %d; no storage", ret);
      storage_publish(storage);
      return OK;
    }

  storage->board_open = true;
  return OK;
}

/****************************************************************************
 * Name: storage_stop
 *
 * Description:
 *   What is mounted stays mounted (see storage.h).  A mount under way
 *   finishes on its worker; the loop waits for it as it ends.
 *
 ****************************************************************************/

static void storage_stop(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct storage_s *storage = (FAR struct storage_s *)module;

  storage->stopping = true;

  if (storage->board_open)
    {
      pnut_board_disconnect(&storage->board);
      storage->board_open = false;
    }

  if (storage->topic != NULL)
    {
      pnut_topic_unadvertise(storage->topic);
      storage->topic = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: storage_mount_fat
 ****************************************************************************/

int storage_mount_fat(FAR void *arg, FAR const char *device,
                      FAR const char *path, FAR uint32_t *size_mb)
{
  struct statfs st;

  /* Mounted already, as when the program starts again */

  if (statfs(path, &st) < 0 || st.f_type != MSDOS_SUPER_MAGIC)
    {
      if (mount(device, path, "vfat", 0, NULL) < 0)
        {
          return -errno;
        }

      if (statfs(path, &st) < 0)
        {
          *size_mb = 0;
          return OK;
        }
    }

  *size_mb = (uint32_t)(((uint64_t)st.f_blocks * st.f_bsize) >> 20);
  return OK;
}

/****************************************************************************
 * Name: storage_init
 ****************************************************************************/

void storage_init(FAR struct storage_s *storage,
                  FAR const struct storage_config_s *config)
{
  memset(storage, 0, sizeof(*storage));
  storage->config       = *config;
  storage->module.name  = "storage";
  storage->module.start = storage_start;
  storage->module.stop  = storage_stop;

  if (storage->config.media == NULL)
    {
      storage->config.media = "/media";
    }
}
