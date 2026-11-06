/****************************************************************************
 * pnut-os/src/commands/logs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* logs: the log kept in files, and the crash reports, from the shell
 * (RFC 0026), for developers.  dmesg shows the RAM log; this, what Log
 * kept across restarts.
 *
 *   logs [<level>] [<service>]   the lines, oldest first: those of a level
 *                                (debug, info, notice, warning, error,
 *                                crit, alert, emerg) and worse, of a
 *                                service or program as named in them
 *   logs crashes                 the crash reports, newest first
 *   logs crash <name>            one, whole
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

#include "pnut/log.pnut.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Log's service, and how long the command waits for an answer */

#define LOGS_SERVICE      "log"
#define LOGS_WAIT_MS      15000

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum logs_op_e
{
  LOGS_LINES = 0,
  LOGS_CRASHES,
  LOGS_CRASH
};

struct logs_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_log_client_s client;
  bool open;                      /* The client is */
  FAR struct pnut_timer_s *timer; /* Until the command gives up */
  uint8_t op;                     /* enum logs_op_e */
  pnut_log_read_request_t read;
  pnut_log_crash_request_t crash;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void logs_ask(FAR struct logs_cmd_s *cmd);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The levels by name, least first, as log.proto numbers them from 1 */

static FAR const char * const g_logs_levels[] =
{
  "debug", "info", "notice", "warning", "error", "crit", "alert", "emerg"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void logs_usage(void)
{
  fprintf(stderr,
          "Usage: logs [<level>] [<service>]\n"
          "       logs crashes\n"
          "       logs crash <name>\n"
          "  level: debug, info, notice, warning, error, crit, alert, "
          "emerg\n");
}

/****************************************************************************
 * Name: logs_end
 *
 * Description:
 *   The command is over: the loop stops, with ret for its result.
 *
 ****************************************************************************/

static void logs_end(FAR struct logs_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static bool logs_failed(FAR struct logs_cmd_s *cmd, int status,
                        FAR const pnut_error_t *err)
{
  if (status == PNUT_STATUS_OK)
    {
      return false;
    }

  fprintf(stderr, "logs: %s\n",
          status == PNUT_STATUS_DENIED ? "not allowed" :
          status == PNUT_STATUS_NOTFOUND ? "no such crash report" :
          status == PNUT_STATUS_BUSY ? "Log is busy: try again" :
          status == PNUT_STATUS_UNAVAILABLE ?
          "Log cannot answer now: try again" :
          status == PNUT_STATUS_TIMEOUT ? "Log did not answer" :
          status == PNUT_STATUS_INVALID ? "the answer does not decode" :
          "Log could not read its files");
  logs_end(cmd, -EIO);
  return true;
}

/****************************************************************************
 * Name: logs_page, logs_names, logs_part
 *
 * Description:
 *   An answer: shown, and the next asked for while there is more.
 *
 ****************************************************************************/

static void logs_page(FAR struct pnut_log_client_s *client, int status,
                      FAR const pnut_log_page_t *out,
                      FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct logs_cmd_s *cmd = arg;
  pb_size_t i;

  if (logs_failed(cmd, status, err))
    {
      return;
    }

  if (out->lost)
    {
      printf("---- (lines rotated away meanwhile) ----\n");
    }

  for (i = 0; i < out->lines_count; i++)
    {
      printf("%s\n", out->lines[i]);
    }

  if (!out->more)
    {
      logs_end(cmd, OK);
      return;
    }

  cmd->read.has_from = true;
  cmd->read.from     = out->next;
  logs_ask(cmd);
}

static void logs_names(FAR struct pnut_log_client_s *client, int status,
                       FAR const pnut_log_crashes_t *out,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct logs_cmd_s *cmd = arg;
  pb_size_t i;

  if (logs_failed(cmd, status, err))
    {
      return;
    }

  if (out->names_count == 0)
    {
      printf("No crash reports\n");
    }

  for (i = 0; i < out->names_count; i++)
    {
      printf("%s\n", out->names[i]);
    }

  logs_end(cmd, OK);
}

static void logs_part(FAR struct pnut_log_client_s *client, int status,
                      FAR const pnut_log_crash_t *out,
                      FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct logs_cmd_s *cmd = arg;

  if (logs_failed(cmd, status, err))
    {
      return;
    }

  fwrite(out->text.bytes, 1, out->text.size, stdout);
  if (!out->more || out->text.size == 0)
    {
      logs_end(cmd, OK);
      return;
    }

  cmd->crash.offset += out->text.size;
  logs_ask(cmd);
}

/****************************************************************************
 * Name: logs_ask
 ****************************************************************************/

static void logs_ask(FAR struct logs_cmd_s *cmd)
{
  pnut_log_crashes_request_t crashes;
  int ret;

  switch (cmd->op)
    {
      case LOGS_LINES:
        ret = pnut_log_read(&cmd->client, &cmd->read, 0, logs_page, cmd);
        break;

      case LOGS_CRASHES:
        memset(&crashes, 0, sizeof(crashes));
        ret = pnut_log_crashes(&cmd->client, &crashes, 0, logs_names, cmd);
        break;

      default:
        ret = pnut_log_read_crash(&cmd->client, &cmd->crash, 0, logs_part,
                                  cmd);
        break;
    }

  if (ret < 0)
    {
      fprintf(stderr, "logs: cannot ask Log: %d\n", ret);
      logs_end(cmd, ret);
    }
}

static void logs_state(FAR struct pnut_log_client_s *client,
                       bool connected, FAR void *arg)
{
  FAR struct logs_cmd_s *cmd = arg;

  if (connected)
    {
      logs_ask(cmd);
    }
}

static void logs_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct logs_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "logs: Log does not answer\n");
  logs_end(cmd, -ETIMEDOUT);
}

static int logs_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct logs_cmd_s *cmd = (FAR struct logs_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, LOGS_WAIT_MS, 0, logs_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "logs: no timer: %d\n", ret);
      return ret;
    }

  ret = pnut_log_connect(loop, LOGS_SERVICE, logs_state, cmd, &cmd->client);
  if (ret < 0)
    {
      fprintf(stderr, "logs: cannot reach Log: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  cmd->open = true;
  return OK;
}

static void logs_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct logs_cmd_s *cmd = (FAR struct logs_cmd_s *)module;

  if (cmd->open)
    {
      pnut_log_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/****************************************************************************
 * Name: logs_args
 ****************************************************************************/

static int logs_args(FAR struct logs_cmd_s *cmd, int argc,
                     FAR char *argv[])
{
  int i;
  int a = 1;

  if (argc >= 2 && strcmp(argv[1], "crashes") == 0)
    {
      cmd->op = LOGS_CRASHES;
      return argc == 2 ? OK : -EINVAL;
    }

  if (argc >= 2 && strcmp(argv[1], "crash") == 0)
    {
      if (argc != 3 || strlen(argv[2]) >= sizeof(cmd->crash.name))
        {
          return -EINVAL;
        }

      cmd->op = LOGS_CRASH;
      strlcpy(cmd->crash.name, argv[2], sizeof(cmd->crash.name));
      return OK;
    }

  cmd->op = LOGS_LINES;
  if (a < argc)
    {
      for (i = 0; i < 8; i++)
        {
          if (strcmp(argv[a], g_logs_levels[i]) == 0)
            {
              cmd->read.level = i + 1;
              a++;
              break;
            }
        }
    }

  if (a < argc)
    {
      if (strlen(argv[a]) >= sizeof(cmd->read.service))
        {
          return -EINVAL;
        }

      strlcpy(cmd->read.service, argv[a], sizeof(cmd->read.service));
      a++;
    }

  return a == argc ? OK : -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct logs_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  /* The client holds a page of lines, 2 KB: not for the stack */

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "logs: no memory\n");
      return EXIT_FAILURE;
    }

  if (logs_args(cmd, argc, argv) < 0)
    {
      logs_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "logs";
  cmd->module.start = logs_start;
  cmd->module.stop  = logs_stop;

  /* One connection, its timer and the command's, no workers, and nothing
   * for NxInit
   */

  pnut_loop_defaults(&config);
  config.name    = "logs";
  config.fds     = 2;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "logs: no loop: %d\n", ret);
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
