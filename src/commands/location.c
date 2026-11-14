/****************************************************************************
 * pnut-os/src/commands/location.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* location: where the device is, from the shell (RFC 0035), for
 * developers.
 *   location                                the receiver's state
 *   location now [<m>] [<s>] [approximate]  a position at least as
 *                                           accurate (any), within a time
 *                                           (60 s)
 *   location follow <s> [<for s>]           positions at an interval, for
 *                                           a time (60 s; 0 for ever)
 * Positions are precise unless asked approximate.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/location.pnut.h"
#include "pnut/location.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOCATION_SERVICE   "location"
#define LOCATION_WAIT_MS   3000
#define LOCATION_FOR_MAX   86400

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum location_op_e
{
  LOCATION_STATE = 0,
  LOCATION_NOW,
  LOCATION_FOLLOW
};

struct location_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_location_client_s client;
  struct pnut_location_info_reader_s reader;
  bool open;
  bool reading;
  bool streaming;
  bool opened;
  uint16_t stream;
  FAR struct pnut_timer_s *timer;
  uint8_t op;
  uint32_t seconds;
  uint32_t got;
  pnut_location_current_request_t now;
  pnut_location_follow_request_t follow;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_location_failures[] =
{
  "",
  "no receiver",
  "no position in time, and none known",
  "too many callers waiting"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void location_usage(void)
{
  fprintf(stderr,
          "usage: location\n"
          "       location now [<metres>] [<seconds>] [approximate]\n"
          "       location follow <seconds> [<for seconds>]\n");
}

static void location_end(FAR struct location_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  if (cmd->streaming)
    {
      pnut_location_follow_cancel(&cmd->client, cmd->stream);
      cmd->streaming = false;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static void location_failed(FAR struct location_cmd_s *cmd, int status,
                            FAR const pnut_error_t *err)
{
  if (err != NULL && err->code != 0 &&
      err->code < sizeof(g_location_failures) /
                  sizeof(g_location_failures[0]))
    {
      fprintf(stderr, "location: %s\n", g_location_failures[err->code]);
    }
  else
    {
      fprintf(stderr, "location: %s\n",
              status == PNUT_STATUS_TIMEOUT ? "Location did not answer" :
              "Location cannot answer now");
    }

  location_end(cmd, -EIO);
}

/* Degrees from 1e-7 units, with their sign */

static void location_print(FAR const pnut_location_position_t *p)
{
  long lat = p->lat;
  long lon = p->lon;

  printf("%s%ld.%07ld %s%ld.%07ld, %lu.%03lu m%s",
         lat < 0 ? "-" : "", labs(lat) / 10000000, labs(lat) % 10000000,
         lon < 0 ? "-" : "", labs(lon) / 10000000, labs(lon) % 10000000,
         (unsigned long)(p->accuracy_mm / 1000),
         (unsigned long)(p->accuracy_mm % 1000),
         p->approximate ? ", approximate" : "");
  if (!p->approximate)
    {
      printf(", %ld m high, %lu.%02lu m/s, %lu satellites",
             (long)(p->alt_mm / 1000), (unsigned long)(p->speed_mms / 1000),
             (unsigned long)(p->speed_mms % 1000 / 10),
             (unsigned long)p->satellites);
    }

  printf(", %lld UTC\n", (long long)p->utc);
}

static void location_answered(FAR struct pnut_location_client_s *client,
                              int status,
                              FAR const pnut_location_current_reply_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      location_failed(cmd, status, err);
      return;
    }

  printf("%s", out->fresh ? "" : "(not as asked, the best known) ");
  location_print(&out->position);
  location_end(cmd, OK);
}

static void location_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg);

static void location_opened(FAR struct pnut_location_client_s *client,
                            int status, FAR const pnut_error_t *err,
                            FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      cmd->streaming = false;
      location_failed(cmd, status, err);
      return;
    }

  cmd->opened = true;
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  if (cmd->seconds > 0 &&
      pnut_timer_start(cmd->module.loop, cmd->seconds * 1000, 0,
                       location_timeout, cmd, &cmd->timer) < 0)
    {
      cmd->timer = NULL;
    }

  printf("Following\n");
}

static void location_item(FAR struct pnut_location_client_s *client,
                          int status,
                          FAR const pnut_location_position_t *item,
                          FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;

  if (item == NULL)
    {
      cmd->streaming = false;
      location_end(cmd, status == PNUT_STATUS_ENDED ? OK : -EIO);
      return;
    }

  cmd->got++;
  location_print(item);
  pnut_location_follow_credit(client, cmd->stream, 1);
}

static void location_state(FAR struct pnut_location_client_s *client,
                           bool connected, FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;
  int ret;

  if (!connected || cmd->streaming || cmd->opened)
    {
      return;
    }

  if (cmd->op == LOCATION_NOW)
    {
      cmd->opened = true;
      ret = pnut_location_current(client, &cmd->now,
                                  (cmd->now.timeout_s + 10) * 1000,
                                  location_answered, cmd);
    }
  else
    {
      ret = pnut_location_follow(client, &cmd->follow, 1, 0,
                                 location_opened, location_item, cmd,
                                 &cmd->stream);
      cmd->streaming = ret >= 0;
    }

  if (ret < 0)
    {
      fprintf(stderr, "location: cannot ask Location: %d\n", ret);
      location_end(cmd, ret);
      return;
    }

  /* A Current answers in its own time */

  if (cmd->op == LOCATION_NOW && cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }
}

static void location_info(FAR struct pnut_location_info_reader_s *reader,
                          FAR const pnut_location_info_t *msg, FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;

  if (!msg->on)
    {
      printf("The receiver is off\n");
    }
  else
    {
      printf("The receiver is on: %s, %lu satellites\n",
             msg->fix ? "a fix" : "no fix", (unsigned long)msg->satellites);
    }

  location_end(cmd, OK);
}

static void location_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct location_cmd_s *cmd = arg;

  cmd->timer = NULL;
  if (cmd->op == LOCATION_FOLLOW && cmd->opened)
    {
      printf("%lu positions\n", (unsigned long)cmd->got);
      location_end(cmd, OK);
      return;
    }

  fprintf(stderr, "location: Location does not answer (is the sensors "
          "program running?)\n");
  location_end(cmd, -ETIMEDOUT);
}

static int location_start(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct location_cmd_s *cmd = (FAR struct location_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, LOCATION_WAIT_MS, 0, location_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "location: no timer: %d\n", ret);
      return ret;
    }

  if (cmd->op == LOCATION_STATE)
    {
      ret = pnut_location_info_subscribe(loop, location_info, cmd,
                                         &cmd->reader);
      cmd->reading = ret >= 0;
    }
  else
    {
      ret = pnut_location_connect(loop, LOCATION_SERVICE, location_state,
                                  cmd, &cmd->client);
      cmd->open = ret >= 0;
    }

  if (ret < 0)
    {
      fprintf(stderr, "location: cannot reach Location: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  return OK;
}

static void location_stop(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct location_cmd_s *cmd = (FAR struct location_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_location_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->open)
    {
      pnut_location_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

static int location_number(FAR const char *s, uint32_t max,
                           FAR uint32_t *value)
{
  FAR char *end;
  long n = strtol(s, &end, 10);

  if (end == s || *end != '\0' || n < 0 || (unsigned long)n > max)
    {
      return -EINVAL;
    }

  *value = (uint32_t)n;
  return OK;
}

static int location_args(FAR struct location_cmd_s *cmd, int argc,
                         FAR char *argv[])
{
  int i;

  if (argc == 1)
    {
      cmd->op = LOCATION_STATE;
      return OK;
    }

  if (strcmp(argv[1], "now") == 0)
    {
      cmd->op = LOCATION_NOW;
      cmd->now.precise   = true;
      cmd->now.timeout_s = 60;
      for (i = 2; i < argc; i++)
        {
          if (strcmp(argv[i], "approximate") == 0)
            {
              cmd->now.precise = false;
            }
          else if (location_number(argv[i], i == 2 ? 100000 : 3600,
                                   i == 2 ? &cmd->now.accuracy_m :
                                   &cmd->now.timeout_s) < 0 || i > 3)
            {
              return -EINVAL;
            }
        }

      return OK;
    }

  if (strcmp(argv[1], "follow") == 0 && argc >= 3 && argc <= 4)
    {
      cmd->op = LOCATION_FOLLOW;
      cmd->follow.precise = true;
      cmd->seconds        = 60;
      if (location_number(argv[2], 3600, &cmd->follow.interval_s) < 0 ||
          cmd->follow.interval_s == 0 ||
          (argc == 4 && location_number(argv[3], LOCATION_FOR_MAX,
                                        &cmd->seconds) < 0))
        {
          return -EINVAL;
        }

      return OK;
    }

  return -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct location_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "location: no memory\n");
      return EXIT_FAILURE;
    }

  if (location_args(cmd, argc, argv) < 0)
    {
      location_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "location";
  cmd->module.start = location_start;
  cmd->module.stop  = location_stop;

  pnut_loop_defaults(&config);
  config.name    = "location";
  config.fds     = 3;
  config.timers  = 2;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "location: no loop: %d\n", ret);
      free(cmd);
      return EXIT_FAILURE;
    }

  ret = pnut_module_add(loop, &cmd->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  free(cmd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
