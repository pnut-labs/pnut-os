/****************************************************************************
 * pnut-os/src/system/board.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pb_decode.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>

#include "board.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void board_describe(FAR struct pnut_board_server_s *server,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_board_describe_request_t *in,
                           FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pnut_board_handlers_s g_board_handlers =
{
  .describe = board_describe,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_load
 *
 * Description:
 *   Read the target's description.  A unit's device data would be merged
 *   in here; none is provisioned yet.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value otherwise.
 *
 ****************************************************************************/

static int board_load(FAR struct board_s *board)
{
  FAR uint8_t *buffer;
  pb_istream_t stream;
  ssize_t len = 0;
  ssize_t n;
  int ret = OK;
  int fd;

  fd = open(board->config.path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  /* One byte more than the largest description, to tell one too long */

  buffer = malloc(PNUT_BOARD_DESCRIPTION_SIZE + 1);
  if (buffer == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (len <= PNUT_BOARD_DESCRIPTION_SIZE)
    {
      n = read(fd, buffer + len, PNUT_BOARD_DESCRIPTION_SIZE + 1 - len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          ret = n < 0 ? -errno : OK;
          break;
        }

      len += n;
    }

  close(fd);

  if (ret == OK && len > PNUT_BOARD_DESCRIPTION_SIZE)
    {
      ret = -EFBIG;
    }

  if (ret == OK)
    {
      memset(&board->desc, 0, sizeof(board->desc));
      stream = pb_istream_from_buffer(buffer, len);
      if (!pb_decode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, &board->desc))
        {
          ret = -EINVAL;
        }
    }

  free(buffer);

  if (ret < 0)
    {
      return ret;
    }

  /* What differs per unit comes from its device data alone */

  board->desc.has_identity = true;
  board->desc.identity.revision[0] = '\0';
  board->desc.identity.serial[0]   = '\0';
  board->desc.identity.provisioned = false;
  return OK;
}

/****************************************************************************
 * Name: board_describe
 ****************************************************************************/

static void board_describe(FAR struct pnut_board_server_s *server,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_board_describe_request_t *in,
                           FAR void *arg)
{
  FAR struct board_s *board = arg;

  if (!board->loaded)
    {
      pnut_board_fail(server, req, PNUT_STATUS_UNAVAILABLE, 0,
                      "No description");
      return;
    }

  pnut_board_describe_reply(server, req, PNUT_STATUS_OK, &board->desc);
}

/****************************************************************************
 * Name: board_settings
 *
 * Description:
 *   Board's settings were read, or one changed: announce airplane mode,
 *   once read, and at each change.
 *
 ****************************************************************************/

static void board_settings(FAR struct pnut_settings_cache_s *cache,
                           int index, FAR void *arg)
{
  FAR struct board_s *board = arg;
  pnut_airplane_mode_t msg;
  bool on = board->settings.values.airplane;
  int ret;

  if (board->airplanes == NULL ||
      (board->announced && on == board->airplane))
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.on = on;
  ret = pnut_airplane_mode_publish(board->airplanes, &msg);
  if (ret < 0)
    {
      pnut_log(&board->module, PNUT_LOG_WARNING,
               "Cannot announce airplane mode: %d", ret);
      return;
    }

  if (board->announced || on)
    {
      pnut_log(&board->module, PNUT_LOG_INFO, "Airplane mode %s",
               on ? "on" : "off");
    }

  board->announced = true;
  board->airplane  = on;
}

/****************************************************************************
 * Name: board_start
 ****************************************************************************/

static int board_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct board_s *board = (FAR struct board_s *)module;
  int ret;

  board->loaded = false;

  /* Without a description Board serves none: the program goes on */

  if (board->config.path == NULL)
    {
      pnut_log(module, PNUT_LOG_INFO, "No description");
    }
  else
    {
      ret = board_load(board);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_ERROR, "No description at %s: %d",
                   board->config.path, ret);
        }
      else
        {
          board->loaded = true;
          pnut_log(module, PNUT_LOG_INFO,
                   "%s %s, %u parts; unprovisioned",
                   board->desc.identity.maker, board->desc.identity.model,
                   (unsigned int)board->desc.parts_count);
        }
    }

  ret = pnut_board_serve(loop, BOARD_SERVICE, 0, &g_board_handlers, board,
                         &board->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  board->serving = true;

  /* Airplane mode is announced once Settings' value is read: until then
   * the radios' owners have none, and keep their radios as they are
   */

  ret = pnut_airplane_mode_advertise(loop, &board->airplanes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING,
               "No airplane_mode topic: %d; airplane mode not announced",
               ret);
      board->airplanes = NULL;
    }
  else
    {
      ret = pnut_board_settings_open(module, board_settings, board,
                                     &board->settings);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No settings: %d; airplane mode not announced", ret);
          pnut_topic_unadvertise(board->airplanes);
          board->airplanes = NULL;
        }
      else
        {
          board->settings_open = true;
        }
    }

  pnut_module_ready(module);
  return OK;
}

/****************************************************************************
 * Name: board_stop
 ****************************************************************************/

static void board_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct board_s *board = (FAR struct board_s *)module;

  if (board->settings_open)
    {
      pnut_board_settings_close(&board->settings);
      board->settings_open = false;
    }

  if (board->airplanes != NULL)
    {
      pnut_topic_unadvertise(board->airplanes);
      board->airplanes = NULL;
    }

  board->announced = false;

  if (board->serving)
    {
      pnut_board_close(&board->server);
      board->serving = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_init
 ****************************************************************************/

void board_init(FAR struct board_s *board,
                FAR const struct board_config_s *config)
{
  memset(board, 0, sizeof(*board));
  board->config       = *config;
  board->module.name  = "board";
  board->module.start = board_start;
  board->module.stop  = board_stop;
}
