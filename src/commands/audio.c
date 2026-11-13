/****************************************************************************
 * pnut-os/src/commands/audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The audio command: what plays, a built-in sound played, a sound
 * stopped (RFC 0032).  The volumes are settings: settings set audio
 * volume.ring 5.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/audio.pnut.h"
#include "pnut/audio.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_SERVICE   "audio"
#define AUDIO_WAIT_MS   3000
#define AUDIO_USES      (int)(sizeof(g_audio_uses) / sizeof(g_audio_uses[0]))

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum audio_op_e
{
  AUDIO_STATE = 0,
  AUDIO_PLAY,
  AUDIO_STOP
};

struct audio_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_audio_client_s client;
  struct pnut_audio_info_reader_s reader;
  bool open;
  bool reading;
  FAR struct pnut_timer_s *timer;
  uint8_t op;                     /* enum audio_op_e */
  pnut_audio_play_request_t play;
  pnut_audio_stop_request_t stop;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_audio_uses[] =
{
  "media", "interface", "notification", "ring", "alarm"
};

static FAR const char * const g_audio_failures[] =
{
  "", "no speaker", "no built-in sound of that name",
  "as many sounds play as can", "no sound of that handle plays"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void audio_usage(void)
{
  fprintf(stderr,
          "Usage: audio\n"
          "       audio play <sound> [<use>] [loop]\n"
          "       audio stop <handle>\n"
          "  sounds: ls /usr/share/sounds (click error notification "
          "ring alarm)\n"
          "  uses: media interface notification ring alarm\n"
          "  volumes, 0-10: settings set audio volume.ring|volume.media "
          "<n>\n");
}

static int audio_find(FAR const char *name)
{
  int i;

  for (i = 0; i < AUDIO_USES; i++)
    {
      if (strcmp(g_audio_uses[i], name) == 0)
        {
          return i;
        }
    }

  return -1;
}

static void audio_end(FAR struct audio_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static void audio_failure(int status, FAR const pnut_error_t *err)
{
  if (err != NULL && err->code != 0 &&
      err->code < sizeof(g_audio_failures) / sizeof(g_audio_failures[0]))
    {
      fprintf(stderr, "audio: %s\n", g_audio_failures[err->code]);
    }
  else
    {
      fprintf(stderr, "audio: %s\n",
              status == PNUT_STATUS_TIMEOUT ? "Audio did not answer" :
              status == PNUT_STATUS_INVALID ? "not understood" :
              "Audio cannot answer now");
    }
}

static void audio_played(FAR struct pnut_audio_client_s *client,
                         int status, FAR const pnut_audio_play_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct audio_cmd_s *cmd = arg;

  if (status == PNUT_STATUS_OK && out != NULL)
    {
      printf("%" PRIu32 "\n", out->handle);
      audio_end(cmd, OK);
      return;
    }

  audio_failure(status, err);
  audio_end(cmd, -EIO);
}

static void audio_stopped(FAR struct pnut_audio_client_s *client,
                          int status, FAR const pnut_audio_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct audio_cmd_s *cmd = arg;

  if (status == PNUT_STATUS_OK)
    {
      audio_end(cmd, OK);
      return;
    }

  audio_failure(status, err);
  audio_end(cmd, -EIO);
}

/* Connected: the request */

static void audio_state(FAR struct pnut_audio_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct audio_cmd_s *cmd = arg;
  int ret;

  if (!connected)
    {
      return;
    }

  if (cmd->op == AUDIO_PLAY)
    {
      ret = pnut_audio_play(client, &cmd->play, 0, audio_played, cmd);
    }
  else
    {
      ret = pnut_audio_stop(client, &cmd->stop, 0, audio_stopped, cmd);
    }

  if (ret < 0)
    {
      fprintf(stderr, "audio: cannot ask Audio: %d\n", ret);
      audio_end(cmd, ret);
    }
}

/* The topic's last state, which a new reader gets at once */

static void audio_info(FAR struct pnut_audio_info_reader_s *reader,
                       FAR const pnut_audio_info_t *msg, FAR void *arg)
{
  FAR struct audio_cmd_s *cmd = arg;
  bool any = false;
  int i;

  for (i = 0; i < AUDIO_USES; i++)
    {
      if (msg->uses & (1u << i))
        {
          printf("%s%s", any ? " " : "playing: ", g_audio_uses[i]);
          any = true;
        }
    }

  printf("%s\n", any ? "" : "silent");
  audio_end(cmd, OK);
}

static void audio_timeout(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct audio_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "audio: Audio does not answer (is the media program "
          "running?)\n");
  audio_end(cmd, -ETIMEDOUT);
}

static int audio_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct audio_cmd_s *cmd = (FAR struct audio_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, AUDIO_WAIT_MS, 0, audio_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "audio: no timer: %d\n", ret);
      return ret;
    }

  if (cmd->op == AUDIO_STATE)
    {
      ret = pnut_audio_info_subscribe(loop, audio_info, cmd, &cmd->reader);
      cmd->reading = ret >= 0;
      if (ret < 0)
        {
          fprintf(stderr, "audio: no audio topic: %d\n", ret);
        }
    }
  else
    {
      ret = pnut_audio_connect(loop, AUDIO_SERVICE, audio_state, cmd,
                               &cmd->client);
      cmd->open = ret >= 0;
      if (ret < 0)
        {
          fprintf(stderr, "audio: cannot reach Audio: %d\n", ret);
        }
    }

  if (ret < 0)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  return OK;
}

static void audio_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct audio_cmd_s *cmd = (FAR struct audio_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_audio_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->open)
    {
      pnut_audio_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/* A sound's use, unless given: by its name */

static int audio_use(FAR const char *name)
{
  return strcmp(name, "ring") == 0 ? PNUT_AUDIO_USE_AUDIO_USE_RING :
         strcmp(name, "alarm") == 0 ? PNUT_AUDIO_USE_AUDIO_USE_ALARM :
         strcmp(name, "notification") == 0 ?
         PNUT_AUDIO_USE_AUDIO_USE_NOTIFICATION :
         strcmp(name, "click") == 0 || strcmp(name, "error") == 0 ?
         PNUT_AUDIO_USE_AUDIO_USE_INTERFACE :
         PNUT_AUDIO_USE_AUDIO_USE_MEDIA;
}

static int audio_args(FAR struct audio_cmd_s *cmd, int argc,
                      FAR char *argv[])
{
  FAR char *end;
  unsigned long handle;
  int use = -1;
  int i;

  if (argc == 1)
    {
      cmd->op = AUDIO_STATE;
      return OK;
    }

  if (strcmp(argv[1], "play") == 0 && argc >= 3 && argc <= 5)
    {
      if (strlen(argv[2]) >= sizeof(cmd->play.name))
        {
          return -EINVAL;
        }

      for (i = 3; i < argc; i++)
        {
          if (strcmp(argv[i], "loop") == 0 && !cmd->play.loop)
            {
              cmd->play.loop = true;
            }
          else if (use < 0 && (use = audio_find(argv[i])) >= 0)
            {
              continue;
            }
          else
            {
              return -EINVAL;
            }
        }

      strlcpy(cmd->play.name, argv[2], sizeof(cmd->play.name));
      cmd->play.use = use >= 0 ? use : audio_use(argv[2]);
      cmd->op       = AUDIO_PLAY;
      return OK;
    }

  if (strcmp(argv[1], "stop") == 0 && argc == 3)
    {
      handle = strtoul(argv[2], &end, 10);
      if (*end != '\0' || handle == 0 || handle > UINT32_MAX)
        {
          return -EINVAL;
        }

      cmd->stop.handle = (uint32_t)handle;
      cmd->op          = AUDIO_STOP;
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
  FAR struct audio_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "audio: no memory\n");
      return EXIT_FAILURE;
    }

  if (audio_args(cmd, argc, argv) < 0)
    {
      audio_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "audio";
  cmd->module.start = audio_start;
  cmd->module.stop  = audio_stop;

  pnut_loop_defaults(&config);
  config.name    = "audio";
  config.fds     = 3;
  config.timers  = 2;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "audio: no loop: %d\n", ret);
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
