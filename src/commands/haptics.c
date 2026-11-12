/****************************************************************************
 * pnut-os/src/commands/haptics.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* haptics: the vibration motor from the shell (RFC 0033), for developers.
 *
 *   haptics                           what plays
 *   haptics effect <name> [<use>]     a named effect
 *   haptics pattern <ms>:<0-100> ...  steps of a time and a strength
 *   haptics stop [<use>]              stop what plays, or a use's
 *
 * Effects: tick click double notification ring alarm success error.
 * Uses: app interface notification ring alarm; an effect's own by default
 * (ring, alarm and notification theirs, the others app), a pattern app.
 * The strength is a setting: `settings set haptics strength weak`.
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

#include "pnut/haptics.pnut.h"
#include "pnut/haptics.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HAPTICS_SERVICE   "haptics"
#define HAPTICS_WAIT_MS   3000

#define HAPTICS_EFFECTS   (int)(sizeof(g_haptics_effects) / \
                                sizeof(g_haptics_effects[0]))
#define HAPTICS_USES      (int)(sizeof(g_haptics_uses) / \
                                sizeof(g_haptics_uses[0]))

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum haptics_op_e
{
  HAPTICS_STATE = 0,
  HAPTICS_EFFECT,
  HAPTICS_PATTERN,
  HAPTICS_STOP
};

struct haptics_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_haptics_client_s client;
  struct pnut_haptics_info_reader_s reader;
  bool open;
  bool reading;
  FAR struct pnut_timer_s *timer;
  uint8_t op;                     /* enum haptics_op_e */
  int use;                        /* -1: from the topic (stop) */
  pnut_haptics_effect_request_t effect;
  pnut_haptics_pattern_request_t pattern;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_haptics_effects[] =
{
  "tick", "click", "double", "notification", "ring", "alarm", "success",
  "error"
};

static FAR const char * const g_haptics_uses[] =
{
  "app", "interface", "notification", "ring", "alarm"
};

static FAR const char * const g_haptics_failures[] =
{
  "", "no vibration motor", "a higher use plays", "a pattern is 1 to 16 "
  "steps, up to 5 s in all, strengths up to 100", "an app's: one a second",
  "the interface's are off (haptics.interface)"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void haptics_usage(void)
{
  fprintf(stderr,
          "Usage: haptics\n"
          "       haptics effect <name> [<use>]\n"
          "       haptics pattern <ms>:<strength 0-100> ...\n"
          "       haptics stop [<use>]\n"
          "  effects: tick click double notification ring alarm "
          "success error\n"
          "  uses: app interface notification ring alarm\n"
          "  strength: settings set haptics strength weak|medium|strong\n");
}

static int haptics_find(FAR const char * const *names, int n,
                        FAR const char *name)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(names[i], name) == 0)
        {
          return i;
        }
    }

  return -1;
}

static void haptics_end(FAR struct haptics_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static void haptics_replied(FAR struct pnut_haptics_client_s *client,
                            int status, FAR const pnut_haptics_reply_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct haptics_cmd_s *cmd = arg;

  if (status == PNUT_STATUS_OK)
    {
      haptics_end(cmd, OK);
      return;
    }

  if (err != NULL && err->code != 0 &&
      err->code < sizeof(g_haptics_failures) / sizeof(g_haptics_failures[0]))
    {
      fprintf(stderr, "haptics: %s\n", g_haptics_failures[err->code]);
    }
  else
    {
      fprintf(stderr, "haptics: %s\n",
              status == PNUT_STATUS_TIMEOUT ? "Haptics did not answer" :
              status == PNUT_STATUS_INVALID ? "not understood" :
              "Haptics cannot answer now");
    }

  haptics_end(cmd, -EIO);
}

/* Connected: the request */

static void haptics_state(FAR struct pnut_haptics_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct haptics_cmd_s *cmd = arg;
  pnut_haptics_stop_request_t stop;
  int ret = OK;

  if (!connected)
    {
      return;
    }

  switch (cmd->op)
    {
      case HAPTICS_EFFECT:
        ret = pnut_haptics_effect(client, &cmd->effect, 0, haptics_replied,
                                  cmd);
        break;

      case HAPTICS_PATTERN:
        ret = pnut_haptics_pattern(client, &cmd->pattern, 0,
                                   haptics_replied, cmd);
        break;

      case HAPTICS_STOP:
        memset(&stop, 0, sizeof(stop));
        stop.use = cmd->use;
        ret = pnut_haptics_stop(client, &stop, 0, haptics_replied, cmd);
        break;

      default:
        break;
    }

  if (ret < 0)
    {
      fprintf(stderr, "haptics: cannot ask Haptics: %d\n", ret);
      haptics_end(cmd, ret);
    }
}

static int haptics_connect(FAR struct haptics_cmd_s *cmd)
{
  int ret;

  ret = pnut_haptics_connect(cmd->module.loop, HAPTICS_SERVICE,
                             haptics_state, cmd, &cmd->client);
  cmd->open = ret >= 0;
  if (ret < 0)
    {
      fprintf(stderr, "haptics: cannot reach Haptics: %d\n", ret);
    }

  return ret;
}

/* The topic's last state, which a new reader gets at once: printed, or
 * the use a stop asks for
 */

static void haptics_info(FAR struct pnut_haptics_info_reader_s *reader,
                         FAR const pnut_haptics_info_t *msg, FAR void *arg)
{
  FAR struct haptics_cmd_s *cmd = arg;

  if (cmd->op == HAPTICS_STOP)
    {
      if (cmd->use >= 0 || cmd->open)
        {
          return;
        }

      if (!msg->playing)
        {
          printf("Nothing plays\n");
          haptics_end(cmd, OK);
          return;
        }

      cmd->use = msg->use;
      if (haptics_connect(cmd) < 0)
        {
          haptics_end(cmd, -EIO);
        }

      return;
    }

  if (!msg->playing)
    {
      printf("still\n");
    }
  else if (msg->pattern)
    {
      printf("playing a pattern, use %s\n",
             msg->use < HAPTICS_USES ? g_haptics_uses[msg->use] : "?");
    }
  else
    {
      printf("playing %s, use %s\n",
             msg->effect < HAPTICS_EFFECTS ?
             g_haptics_effects[msg->effect] : "?",
             msg->use < HAPTICS_USES ? g_haptics_uses[msg->use] : "?");
    }

  haptics_end(cmd, OK);
}

static void haptics_timeout(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct haptics_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "haptics: Haptics does not answer (is the media "
          "program running?)\n");
  haptics_end(cmd, -ETIMEDOUT);
}

static int haptics_start(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct haptics_cmd_s *cmd = (FAR struct haptics_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, HAPTICS_WAIT_MS, 0, haptics_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "haptics: no timer: %d\n", ret);
      return ret;
    }

  if (cmd->op == HAPTICS_STATE || (cmd->op == HAPTICS_STOP && cmd->use < 0))
    {
      ret = pnut_haptics_info_subscribe(loop, haptics_info, cmd,
                                        &cmd->reader);
      cmd->reading = ret >= 0;
      if (ret < 0)
        {
          fprintf(stderr, "haptics: no haptics topic: %d\n", ret);
        }
    }
  else
    {
      ret = haptics_connect(cmd);
    }

  if (ret < 0)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  return OK;
}

static void haptics_stop(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct haptics_cmd_s *cmd = (FAR struct haptics_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_haptics_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->open)
    {
      pnut_haptics_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

static int haptics_args(FAR struct haptics_cmd_s *cmd, int argc,
                        FAR char *argv[])
{
  FAR char *end;
  long ms;
  long strength;
  int effect;
  int i;

  cmd->use = -1;
  if (argc == 1)
    {
      cmd->op = HAPTICS_STATE;
      return OK;
    }

  if (strcmp(argv[1], "effect") == 0 && (argc == 3 || argc == 4))
    {
      effect = haptics_find(g_haptics_effects, HAPTICS_EFFECTS, argv[2]);
      if (effect < 0)
        {
          return -EINVAL;
        }

      if (argc == 4)
        {
          cmd->use = haptics_find(g_haptics_uses, HAPTICS_USES, argv[3]);
          if (cmd->use < 0)
            {
              return -EINVAL;
            }
        }

      cmd->op            = HAPTICS_EFFECT;
      cmd->effect.effect = effect;
      cmd->effect.use    = cmd->use >= 0 ? cmd->use :
        effect == PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_RING ?
        PNUT_HAPTICS_USE_HAPTICS_USE_RING :
        effect == PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_ALARM ?
        PNUT_HAPTICS_USE_HAPTICS_USE_ALARM :
        effect == PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_NOTIFICATION ?
        PNUT_HAPTICS_USE_HAPTICS_USE_NOTIFICATION :
        PNUT_HAPTICS_USE_HAPTICS_USE_APP;
      return OK;
    }

  if (strcmp(argv[1], "stop") == 0 && (argc == 2 || argc == 3))
    {
      if (argc == 3)
        {
          cmd->use = haptics_find(g_haptics_uses, HAPTICS_USES, argv[2]);
          if (cmd->use < 0)
            {
              return -EINVAL;
            }
        }

      cmd->op = HAPTICS_STOP;
      return OK;
    }

  if (strcmp(argv[1], "pattern") == 0 && argc >= 3 &&
      argc - 2 <= (int)(sizeof(cmd->pattern.steps) /
                        sizeof(cmd->pattern.steps[0])))
    {
      for (i = 2; i < argc; i++)
        {
          ms = strtol(argv[i], &end, 10);
          if (*end != ':' || ms <= 0 || ms > 5000)
            {
              return -EINVAL;
            }

          strength = strtol(end + 1, &end, 10);
          if (*end != '\0' || strength < 0 || strength > 100)
            {
              return -EINVAL;
            }

          cmd->pattern.steps[i - 2].ms       = (uint32_t)ms;
          cmd->pattern.steps[i - 2].strength = (uint32_t)strength;
        }

      cmd->op                  = HAPTICS_PATTERN;
      cmd->pattern.steps_count = argc - 2;
      cmd->pattern.use         = PNUT_HAPTICS_USE_HAPTICS_USE_APP;
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
  FAR struct haptics_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "haptics: no memory\n");
      return EXIT_FAILURE;
    }

  if (haptics_args(cmd, argc, argv) < 0)
    {
      haptics_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "haptics";
  cmd->module.start = haptics_start;
  cmd->module.stop  = haptics_stop;

  pnut_loop_defaults(&config);
  config.name    = "haptics";
  config.fds     = 3;
  config.timers  = 2;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "haptics: no loop: %d\n", ret);
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
