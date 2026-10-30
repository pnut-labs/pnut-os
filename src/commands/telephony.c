/****************************************************************************
 * pnut-os/src/commands/telephony.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* telephony: the modem from the shell (RFC 0030), for developers.  A
 * command typed in the developer's shell may do what the system UI may:
 * hand the SIM its PIN, and pass an AT command through, for diagnosis.
 *   telephony                         the modem's and the network's state
 *   telephony pin <PIN>               the SIM's PIN
 *   telephony puk <PUK> <PIN>         its PUK, with a new PIN
 *   telephony at <command> [<ms>]     one AT command, and its answer
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

#include "pnut/telephony.pnut.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Telephony's service, and how long the command waits for it besides a
 * command's own time, in milliseconds
 */

#define TELEPHONY_SERVICE  "telephony"
#define TELEPHONY_WAIT_MS  5000
#define TELEPHONY_PIN_MS   20000

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum telephony_op_e
{
  TELEPHONY_STATUS = 0,
  TELEPHONY_PIN,
  TELEPHONY_PUK,
  TELEPHONY_AT
};

struct telephony_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_telephony_client_s client;
  bool open;                      /* The client is */
  bool asked;                     /* The question went out */
  FAR struct pnut_timer_s *timer; /* Until the command gives up */
  uint8_t op;                     /* enum telephony_op_e */
  uint32_t wait;                  /* Its time, in ms */
  union
  {
    pnut_telephony_pin_request_t pin;
    pnut_telephony_puk_request_t puk;
    pnut_telephony_command_request_t command;
  } in;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char *const g_telephony_status[] =
{
  "done", "busy", "denied", "not found", "invalid", "unavailable",
  "no answer in time", "failed"
};

static FAR const char *const g_telephony_modem[] =
{
  "off", "starting", "on", "stopping", "failed"
};

static FAR const char *const g_telephony_sim[] =
{
  "not known", "absent", "needs its PIN", "needs its PUK", "ready",
  "blocked", "failed"
};

static FAR const char *const g_telephony_reg[] =
{
  "none", "searching", "home", "roaming", "denied"
};

static FAR const char *const g_telephony_tech[] =
{
  "", "LTE", "GSM"
};

static FAR const char *const g_telephony_data[] =
{
  "off", "connecting", "on", "failed"
};

static FAR const char *const g_telephony_reason[] =
{
  "", "turned off (telephony.enabled)", "airplane mode", "no SIM",
  "no answer after power-up", "it stopped too often",
  "GSM turned off again", "the device has no modem"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#define TELEPHONY_NAME(table, i) \
  ((i) < sizeof(table) / sizeof(table[0]) ? table[i] : "?")

/****************************************************************************
 * Name: telephony_usage
 ****************************************************************************/

static void telephony_usage(void)
{
  fprintf(stderr,
          "usage: telephony                     the state\n"
          "       telephony pin <PIN>           the SIM's PIN\n"
          "       telephony puk <PUK> <PIN>     its PUK, and a new PIN\n"
          "       telephony at <command> [<ms>] one AT command\n");
}

/****************************************************************************
 * Name: telephony_end
 ****************************************************************************/

static void telephony_end(FAR struct telephony_cmd_s *cmd, int status)
{
  pnut_loop_stop(cmd->module.loop, status);
}

/****************************************************************************
 * Name: telephony_failed
 *
 * Description:
 *   An error answer: its status, and the service's reason.
 *
 ****************************************************************************/

static void telephony_failed(FAR struct telephony_cmd_s *cmd, int status,
                             FAR const pnut_error_t *err)
{
  fprintf(stderr, "telephony: %s%s%s\n",
          status >= 0 && status < (int)(sizeof(g_telephony_status) /
                                        sizeof(g_telephony_status[0])) ?
          g_telephony_status[status] : "failed",
          err != NULL && err->text[0] != '\0' ? ": " : "",
          err != NULL ? err->text : "");
  telephony_end(cmd, -EIO);
}

/****************************************************************************
 * Name: telephony_status_done
 ****************************************************************************/

static void telephony_status_done(FAR struct pnut_telephony_client_s *client,
                                  int status,
                                  FAR const pnut_telephony_info_t *out,
                                  FAR const pnut_error_t *err,
                                  FAR void *arg)
{
  FAR struct telephony_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      telephony_failed(cmd, status, err);
      return;
    }

  printf("modem         %s\n", TELEPHONY_NAME(g_telephony_modem,
                                                out->modem));
  if (out->reason != 0)
    {
      printf("because       %s\n", TELEPHONY_NAME(g_telephony_reason,
                                                    out->reason));
    }

  printf("SIM           %s", TELEPHONY_NAME(g_telephony_sim, out->sim));
  if (out->tries > 0)
    {
      printf(", %" PRIu32 " tries left", out->tries);
    }

  printf("\nregistration  %s\n", TELEPHONY_NAME(g_telephony_reg,
                                                  out->registration));
  if (out->operator_name[0] != '\0')
    {
      printf("operator      %s %s\n", out->operator_name,
             TELEPHONY_NAME(g_telephony_tech, out->tech));
    }

  if (out->dbm != 0)
    {
      printf("signal        %" PRIu32 " of 4 bars, %" PRId32 " dBm\n",
             out->bars, out->dbm);
    }

  printf("mobile data   %s\n", TELEPHONY_NAME(g_telephony_data,
                                                out->data));
  telephony_end(cmd, OK);
}

/****************************************************************************
 * Name: telephony_pin_done
 ****************************************************************************/

static void telephony_pin_done(FAR struct pnut_telephony_client_s *client,
                               int status,
                               FAR const pnut_telephony_reply_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct telephony_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      telephony_failed(cmd, status, err);
      return;
    }

  printf("The SIM took it\n");
  telephony_end(cmd, OK);
}

/****************************************************************************
 * Name: telephony_at_done
 ****************************************************************************/

static void telephony_at_done(FAR struct pnut_telephony_client_s *client,
                              int status,
                              FAR const pnut_telephony_command_reply_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct telephony_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      telephony_failed(cmd, status, err);
      return;
    }

  printf("%s%s\n", out->reply, out->ok ? "OK" : "");
  telephony_end(cmd, out->ok ? OK : -EIO);
}

/****************************************************************************
 * Name: telephony_ask
 ****************************************************************************/

static void telephony_ask(FAR struct telephony_cmd_s *cmd)
{
  pnut_telephony_status_request_t status;
  int ret;

  switch (cmd->op)
    {
      case TELEPHONY_PIN:
        ret = pnut_telephony_enter_pin(&cmd->client, &cmd->in.pin,
                                       TELEPHONY_PIN_MS, telephony_pin_done,
                                       cmd);
        break;

      case TELEPHONY_PUK:
        ret = pnut_telephony_enter_puk(&cmd->client, &cmd->in.puk,
                                       TELEPHONY_PIN_MS, telephony_pin_done,
                                       cmd);
        break;

      case TELEPHONY_AT:

        /* The service's own time out first, and tell */

        ret = pnut_telephony_command(&cmd->client, &cmd->in.command,
                                     cmd->wait + 2000, telephony_at_done,
                                     cmd);
        break;

      default:
        memset(&status, 0, sizeof(status));
        ret = pnut_telephony_status(&cmd->client, &status, 0,
                                    telephony_status_done, cmd);
        break;
    }

  /* Nothing of the PIN stays here once it is sent */

  memset(&cmd->in, 0, sizeof(cmd->in));
  if (ret < 0)
    {
      fprintf(stderr, "telephony: cannot ask: %d\n", ret);
      telephony_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: telephony_state
 ****************************************************************************/

static void telephony_state(FAR struct pnut_telephony_client_s *client,
                            bool connected, FAR void *arg)
{
  FAR struct telephony_cmd_s *cmd = arg;

  if (connected && !cmd->asked)
    {
      cmd->asked = true;
      telephony_ask(cmd);
    }
}

/****************************************************************************
 * Name: telephony_timeout
 ****************************************************************************/

static void telephony_timeout(FAR struct pnut_loop_s *loop,
                              FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "telephony: %s\n", cmd->asked ?
          "Telephony does not answer" : "Telephony is not running");
  telephony_end(cmd, -ETIMEDOUT);
}

/****************************************************************************
 * Name: telephony_start
 ****************************************************************************/

static int telephony_start(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct telephony_cmd_s *cmd = (FAR struct telephony_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, cmd->wait + TELEPHONY_WAIT_MS, 0,
                         telephony_timeout, cmd, &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "telephony: no timer: %d\n", ret);
      return ret;
    }

  ret = pnut_telephony_connect(loop, TELEPHONY_SERVICE, telephony_state,
                               cmd, &cmd->client);
  if (ret < 0)
    {
      fprintf(stderr, "telephony: cannot reach Telephony: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  cmd->open = true;
  return OK;
}

/****************************************************************************
 * Name: telephony_stop
 ****************************************************************************/

static void telephony_stop(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct telephony_cmd_s *cmd = (FAR struct telephony_cmd_s *)module;

  if (cmd->open)
    {
      pnut_telephony_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/****************************************************************************
 * Name: telephony_args
 ****************************************************************************/

static int telephony_args(FAR struct telephony_cmd_s *cmd, int argc,
                          FAR char *argv[])
{
  FAR char *end;
  long ms;

  if (argc == 1)
    {
      cmd->op = TELEPHONY_STATUS;
      return OK;
    }

  if (strcmp(argv[1], "pin") == 0 && argc == 3)
    {
      cmd->op   = TELEPHONY_PIN;
      cmd->wait = TELEPHONY_PIN_MS;
      strlcpy(cmd->in.pin.pin, argv[2], sizeof(cmd->in.pin.pin));
      return OK;
    }

  if (strcmp(argv[1], "puk") == 0 && argc == 4)
    {
      cmd->op   = TELEPHONY_PUK;
      cmd->wait = TELEPHONY_PIN_MS;
      strlcpy(cmd->in.puk.puk, argv[2], sizeof(cmd->in.puk.puk));
      strlcpy(cmd->in.puk.pin, argv[3], sizeof(cmd->in.puk.pin));
      return OK;
    }

  if (strcmp(argv[1], "at") == 0 && (argc == 3 || argc == 4))
    {
      cmd->op = TELEPHONY_AT;
      if (strlcpy(cmd->in.command.command, argv[2],
                  sizeof(cmd->in.command.command)) >=
          sizeof(cmd->in.command.command))
        {
          fprintf(stderr, "telephony: the command is too long\n");
          return -E2BIG;
        }

      ms = 10000;
      if (argc == 4)
        {
          ms = strtol(argv[3], &end, 10);
          if (*end != '\0' || ms <= 0 || ms > 180000)
            {
              fprintf(stderr, "telephony: a time of 1 to 180000 ms\n");
              return -EINVAL;
            }
        }

      cmd->in.command.timeout = (uint32_t)ms;
      cmd->wait               = (uint32_t)ms;
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
  FAR struct telephony_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  /* The client holds an answer of 1 KB: not for the stack */

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "telephony: no memory\n");
      return EXIT_FAILURE;
    }

  ret = telephony_args(cmd, argc, argv);
  if (ret < 0)
    {
      if (ret == -EINVAL)
        {
          telephony_usage();
        }

      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "telephony";
  cmd->module.start = telephony_start;
  cmd->module.stop  = telephony_stop;

  /* One connection, its timer and the command's, no workers, and nothing
   * for NxInit
   */

  pnut_loop_defaults(&config);
  config.name    = "telephony";
  config.fds     = 2;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "telephony: no loop: %d\n", ret);
      free(cmd);
      return EXIT_FAILURE;
    }

  ret = pnut_module_add(loop, &cmd->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  memset(cmd, 0, sizeof(*cmd));
  free(cmd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
