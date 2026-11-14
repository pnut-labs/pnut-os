/****************************************************************************
 * pnut-os/src/commands/motion.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* motion: what the motion sensors tell, from the shell (RFC 0036), for
 * developers.
 *   motion                    what Motion can tell, still or moving, the
 *                             facing, today's steps, the activity
 *   motion watch [<for s>]    their changes and the gestures as they come,
 *                             for a time (60 s; 0 for ever)
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

#include "pnut/motion.pnut.h"
#include "pnut/motion.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MOTION_SERVICE   "motion"
#define MOTION_WAIT_MS   10000    /* The hub's firmware upload, at start */
#define MOTION_FOR_MAX   86400

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct motion_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_motion_client_s client;
  struct pnut_motion_info_reader_s reader;
  struct pnut_motion_gesture_reader_s greader;
  bool open;
  bool reading;
  bool greading;
  bool asked;
  bool watch;
  bool streaming;
  uint16_t stream;
  FAR struct pnut_timer_s *timer;
  uint32_t seconds;
  pnut_motion_info_t info;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_movements[] =
{
  "unknown", "still", "moving"
};

static FAR const char * const g_facings[] =
{
  "unknown", "up", "down", "upright", "on its side"
};

static FAR const char * const g_activities[] =
{
  "unknown", "still", "walking", "running", "on a bicycle", "in a vehicle"
};

static FAR const char * const g_failures[] =
{
  "", "the device cannot tell", "turned off (motion.steps)",
  "too many following"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR const char *motion_name(FAR const char * const *names,
                                   size_t n, uint32_t i)
{
  return i < n ? names[i] : "?";
}

#define NAME(names, i) motion_name(names, sizeof(names) / sizeof(names[0]), i)

static void motion_end(FAR struct motion_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  if (cmd->streaming)
    {
      pnut_motion_follow_steps_cancel(&cmd->client, cmd->stream);
      cmd->streaming = false;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static void motion_failure(FAR const char *what, int status,
                           FAR const pnut_error_t *err)
{
  if (err != NULL && err->code != 0)
    {
      printf("%s: %s\n", what, NAME(g_failures, err->code));
    }
  else
    {
      printf("%s: no answer (%d)\n", what, status);
    }
}

static void motion_info_print(FAR const pnut_motion_info_t *msg)
{
  printf("%s, facing %s\n", NAME(g_movements, msg->movement),
         NAME(g_facings, msg->facing));
}

static void motion_activity_done(FAR struct pnut_motion_client_s *client,
                                 int status,
                                 FAR const pnut_motion_activity_t *out,
                                 FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      motion_failure("Activity", status, err);
    }
  else
    {
      printf("Activity: %s, since %lld UTC\n",
             NAME(g_activities, out->kind), (long long)out->since);
    }

  motion_info_print(&cmd->info);
  motion_end(cmd, OK);
}

static void motion_steps_done(FAR struct pnut_motion_client_s *client,
                              int status, FAR const pnut_motion_steps_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;
  pnut_motion_activity_request_t req;

  if (status != PNUT_STATUS_OK)
    {
      motion_failure("Steps", status, err);
    }
  else
    {
      printf("Steps today: %lu, changed %lld UTC\n",
             (unsigned long)out->count, (long long)out->changed);
    }

  memset(&req, 0, sizeof(req));
  if (pnut_motion_activity(client, &req, MOTION_WAIT_MS,
                           motion_activity_done, cmd) < 0)
    {
      motion_end(cmd, -EIO);
    }
}

static void motion_abilities_done(FAR struct pnut_motion_client_s *client,
                                  int status,
                                  FAR const pnut_motion_abilities_t *out,
                                  FAR const pnut_error_t *err,
                                  FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;
  pnut_motion_steps_request_t req;

  if (status != PNUT_STATUS_OK)
    {
      motion_failure("Abilities", status, err);
      motion_end(cmd, -EIO);
      return;
    }

  printf("Tells:%s%s%s%s%s%s\n", out->still ? " still/moving" : "",
         out->facing ? " facing" : "", out->steps ? " steps" : "",
         out->activity ? " activity" : "",
         out->picked_up ? " picked-up" : "", out->glance ? " glance" : "");

  memset(&req, 0, sizeof(req));
  if (pnut_motion_steps(client, &req, MOTION_WAIT_MS, motion_steps_done,
                        cmd) < 0)
    {
      motion_end(cmd, -EIO);
    }
}

static void motion_opened(FAR struct pnut_motion_client_s *client,
                          int status, FAR const pnut_error_t *err,
                          FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      cmd->streaming = false;
      motion_failure("Steps", status, err);
    }
}

static void motion_step_item(FAR struct pnut_motion_client_s *client,
                             int status, FAR const pnut_motion_steps_t *item,
                             FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;

  if (item == NULL)
    {
      cmd->streaming = false;
      return;
    }

  printf("Steps today: %lu\n", (unsigned long)item->count);
  pnut_motion_follow_steps_credit(client, cmd->stream, 1);
}

static void motion_state(FAR struct pnut_motion_client_s *client,
                         bool connected, FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;
  int ret;

  if (!connected || cmd->asked)
    {
      return;
    }

  cmd->asked = true;
  if (cmd->watch)
    {
      pnut_motion_steps_request_t req;

      memset(&req, 0, sizeof(req));
      ret = pnut_motion_follow_steps(client, &req, 1, 0, motion_opened,
                                     motion_step_item, cmd, &cmd->stream);
      cmd->streaming = ret >= 0;
    }
  else
    {
      pnut_motion_abilities_request_t req;

      memset(&req, 0, sizeof(req));
      ret = pnut_motion_abilities(client, &req, MOTION_WAIT_MS,
                                  motion_abilities_done, cmd);
    }

  if (ret < 0)
    {
      fprintf(stderr, "motion: cannot ask Motion: %d\n", ret);
      motion_end(cmd, ret);
    }
}

static void motion_info(FAR struct pnut_motion_info_reader_s *reader,
                        FAR const pnut_motion_info_t *msg, FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;

  cmd->info = *msg;
  if (cmd->watch)
    {
      motion_info_print(msg);
    }
}

static void motion_gesture(FAR struct pnut_motion_gesture_reader_s *reader,
                           FAR const pnut_motion_gesture_t *msg,
                           FAR void *arg)
{
  printf("Gesture: %s\n", msg->kind == PNUT_MOTION_GESTURE_KIND_GLANCE ?
         "lifted to be looked at" : "picked up");
}

static void motion_timeout(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct motion_cmd_s *cmd = arg;

  cmd->timer = NULL;
  if (cmd->watch)
    {
      motion_end(cmd, OK);
      return;
    }

  fprintf(stderr, "motion: Motion does not answer (is the sensors program "
          "running?)\n");
  motion_end(cmd, -ETIMEDOUT);
}

static int motion_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  FAR struct motion_cmd_s *cmd = (FAR struct motion_cmd_s *)module;
  uint32_t ms = cmd->watch ? cmd->seconds * 1000 : 3 * MOTION_WAIT_MS;
  int ret;

  if (ms > 0)
    {
      ret = pnut_timer_start(loop, ms, 0, motion_timeout, cmd, &cmd->timer);
      if (ret < 0)
        {
          fprintf(stderr, "motion: no timer: %d\n", ret);
          return ret;
        }
    }

  ret = pnut_motion_info_subscribe(loop, motion_info, cmd, &cmd->reader);
  cmd->reading = ret >= 0;
  if (ret >= 0 && cmd->watch)
    {
      ret = pnut_motion_gesture_subscribe(loop, motion_gesture, cmd,
                                          &cmd->greader);
      cmd->greading = ret >= 0;
    }

  if (ret >= 0)
    {
      ret = pnut_motion_connect(loop, MOTION_SERVICE, motion_state, cmd,
                                &cmd->client);
      cmd->open = ret >= 0;
    }

  if (ret < 0)
    {
      fprintf(stderr, "motion: cannot reach Motion: %d\n", ret);
      if (cmd->timer != NULL)
        {
          pnut_timer_cancel(loop, cmd->timer);
          cmd->timer = NULL;
        }

      return ret;
    }

  return OK;
}

static void motion_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  FAR struct motion_cmd_s *cmd = (FAR struct motion_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_motion_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->greading)
    {
      pnut_motion_gesture_unsubscribe(&cmd->greader);
      cmd->greading = false;
    }

  if (cmd->open)
    {
      pnut_motion_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

static int motion_args(FAR struct motion_cmd_s *cmd, int argc,
                       FAR char *argv[])
{
  FAR char *end;
  long n;

  if (argc == 1)
    {
      return OK;
    }

  if (strcmp(argv[1], "watch") != 0 || argc > 3)
    {
      return -EINVAL;
    }

  cmd->watch   = true;
  cmd->seconds = 60;
  if (argc == 3)
    {
      n = strtol(argv[2], &end, 10);
      if (end == argv[2] || *end != '\0' || n < 0 || n > MOTION_FOR_MAX)
        {
          return -EINVAL;
        }

      cmd->seconds = (uint32_t)n;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct motion_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "motion: no memory\n");
      return EXIT_FAILURE;
    }

  if (motion_args(cmd, argc, argv) < 0)
    {
      fprintf(stderr, "usage: motion\n"
                      "       motion watch [<for seconds>]\n");
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "motion";
  cmd->module.start = motion_start;
  cmd->module.stop  = motion_stop;

  pnut_loop_defaults(&config);
  config.name    = "motion";
  config.fds     = 4;
  config.timers  = 2;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "motion: no loop: %d\n", ret);
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
