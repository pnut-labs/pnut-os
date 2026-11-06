/****************************************************************************
 * pnut-os/src/commands/wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* wifi: Wi-Fi from the shell (RFC 0031), for developers.  Wi-Fi is turned
 * on and off with its setting: `settings set wifi enabled true`.
 *
 *   wifi                          the state
 *   wifi scan                     the networks in reach
 *   wifi join <name> [<pass>]     join one, open or with its passphrase
 *   wifi leave                    leave it
 *
 * A passphrase typed here may stay in the shell's history.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/wifi.pnut.h"
#include "pnut/wifi.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_SERVICE      "wifi"
#define WIFI_WAIT_MS      60000   /* A join: association and DHCP */
#define WIFI_STATE_MS     3000    /* The topic's last state comes at once */

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum wifi_op_e
{
  WIFI_STATE = 0,
  WIFI_SCAN,
  WIFI_JOIN,
  WIFI_LEAVE
};

struct wifi_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_wifi_client_s client;
  struct pnut_wifi_info_reader_s reader;
  bool open;
  bool reading;
  FAR struct pnut_timer_s *timer;
  uint8_t op;                     /* enum wifi_op_e */
  int found;
  pnut_wifi_join_request_t join;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_wifi_states[] =
{
  "off", "on", "joining", "joined"
};

static FAR const char * const g_wifi_failures[] =
{
  "", "not allowed", "Wi-Fi is off", "the radio would not come up",
  "no such network in reach",
  "the network did not take the device (a wrong passphrase, or out of "
  "reach)",
  "no address from the network", "left, or Wi-Fi turned off, first"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A network's name, printed: any access point names itself, so control
 * bytes (a terminal's escapes) are shown as '?'.  The log cleans its own
 * lines (log.c); printf() to the console does not.
 */

static void wifi_print_name(FAR const char *name, size_t size)
{
  size_t i;

  for (i = 0; i < size && name[i] != '\0'; i++)
    {
      putchar((uint8_t)name[i] < 0x20 || name[i] == 0x7f ? '?' : name[i]);
    }
}

static void wifi_usage(void)
{
  fprintf(stderr,
          "Usage: wifi\n"
          "       wifi scan\n"
          "       wifi join <name> [<passphrase>]\n"
          "       wifi leave\n"
          "  on and off: settings set wifi enabled true|false\n");
}

static void wifi_end(FAR struct wifi_cmd_s *cmd, int ret)
{
  explicit_bzero(&cmd->join, sizeof(cmd->join));
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static FAR const char *wifi_failure(uint32_t code)
{
  return code < sizeof(g_wifi_failures) / sizeof(g_wifi_failures[0]) ?
         g_wifi_failures[code] : "";
}

static bool wifi_failed(FAR struct wifi_cmd_s *cmd, int status,
                        FAR const pnut_error_t *err)
{
  if (status == PNUT_STATUS_OK)
    {
      return false;
    }

  if (err != NULL && err->code != 0)
    {
      fprintf(stderr, "wifi: %s\n", wifi_failure(err->code));
    }
  else
    {
      fprintf(stderr, "wifi: %s\n",
              status == PNUT_STATUS_DENIED ? "not allowed" :
              status == PNUT_STATUS_BUSY ? "busy: try again" :
              status == PNUT_STATUS_TIMEOUT ? "Wi-Fi did not answer" :
              status == PNUT_STATUS_INVALID ?
              "a name, and a passphrase of 8 to 63 characters or none" :
              "Wi-Fi cannot answer now");
    }

  wifi_end(cmd, -EIO);
  return true;
}

/****************************************************************************
 * Name: wifi_info
 *
 * Description:
 *   The topic's last state, which a new reader gets at once.
 *
 ****************************************************************************/

static void wifi_info(FAR struct pnut_wifi_info_reader_s *reader,
                      FAR const pnut_wifi_info_t *msg, FAR void *arg)
{
  FAR struct wifi_cmd_s *cmd = arg;
  struct in_addr addr;

  if (cmd->op != WIFI_STATE)
    {
      return;
    }

  printf("state     %s\n", msg->state < 4 ? g_wifi_states[msg->state] :
         "?");
  if (msg->name[0] != '\0')
    {
      printf("network   ");
      wifi_print_name(msg->name, sizeof(msg->name));
      putchar('\n');
    }

  if (msg->state == PNUT_WIFI_INFO_STATE_JOINED)
    {
      addr.s_addr = msg->address;
      printf("signal    %d dBm\n", (int)msg->signal);
      printf("address   %s\n", inet_ntoa(addr));
    }

  if (msg->failure != PNUT_WIFI_ERROR_CODE_NONE)
    {
      printf("last join %s\n", wifi_failure(msg->failure));
    }

  wifi_end(cmd, OK);
}

/****************************************************************************
 * Name: wifi_opened, wifi_item, wifi_replied
 ****************************************************************************/

static void wifi_opened(FAR struct pnut_wifi_client_s *client, int status,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  wifi_failed(arg, status, err);
}

static void wifi_item(FAR struct pnut_wifi_client_s *client, int status,
                      FAR const pnut_wifi_network_t *item, FAR void *arg)
{
  FAR struct wifi_cmd_s *cmd = arg;

  if (status == PNUT_STATUS_OK)
    {
      printf("%4d dBm  ch %-3u %-6s  ", (int)item->signal,
             (unsigned int)item->channel,
             item->security == PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN ?
             "open" : "secure");
      wifi_print_name(item->name, sizeof(item->name));
      putchar('\n');
      cmd->found++;
      return;
    }

  if (status != PNUT_STATUS_ENDED)
    {
      wifi_failed(cmd, status, NULL);
      return;
    }

  if (cmd->found == 0)
    {
      printf("No networks in reach\n");
    }

  wifi_end(cmd, OK);
}

static void wifi_replied(FAR struct pnut_wifi_client_s *client, int status,
                         FAR const pnut_wifi_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifi_cmd_s *cmd = arg;

  if (wifi_failed(cmd, status, err))
    {
      return;
    }

  printf("%s\n", cmd->op == WIFI_JOIN ? "Joined" : "Left");
  wifi_end(cmd, OK);
}

/****************************************************************************
 * Name: wifi_state
 ****************************************************************************/

static void wifi_state(FAR struct pnut_wifi_client_s *client,
                       bool connected, FAR void *arg)
{
  FAR struct wifi_cmd_s *cmd = arg;
  pnut_wifi_scan_request_t scan;
  pnut_wifi_leave_request_t leave;
  uint16_t sno;
  int ret = OK;

  if (!connected)
    {
      return;
    }

  switch (cmd->op)
    {
      case WIFI_SCAN:
        memset(&scan, 0, sizeof(scan));
        ret = pnut_wifi_scan(client, &scan, 32, 0, wifi_opened, wifi_item,
                             cmd, &sno);
        break;

      case WIFI_JOIN:
        ret = pnut_wifi_join(client, &cmd->join, WIFI_WAIT_MS, wifi_replied,
                             cmd);
        explicit_bzero(&cmd->join, sizeof(cmd->join));
        break;

      case WIFI_LEAVE:
        memset(&leave, 0, sizeof(leave));
        ret = pnut_wifi_leave(client, &leave, 0, wifi_replied, cmd);
        break;

      default:
        break;
    }

  if (ret < 0)
    {
      fprintf(stderr, "wifi: cannot ask Wi-Fi: %d\n", ret);
      wifi_end(cmd, ret);
    }
}

static void wifi_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "wifi: Wi-Fi does not answer%s\n",
          cmd->op == WIFI_STATE ? " (is the network program running?)" :
          "");
  wifi_end(cmd, -ETIMEDOUT);
}

static int wifi_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_cmd_s *cmd = (FAR struct wifi_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, cmd->op == WIFI_STATE ? WIFI_STATE_MS :
                         WIFI_WAIT_MS, 0, wifi_timeout, cmd, &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "wifi: no timer: %d\n", ret);
      return ret;
    }

  if (cmd->op == WIFI_STATE)
    {
      ret = pnut_wifi_info_subscribe(loop, wifi_info, cmd, &cmd->reader);
      cmd->reading = ret >= 0;
    }
  else
    {
      ret = pnut_wifi_connect(loop, WIFI_SERVICE, wifi_state, cmd,
                              &cmd->client);
      cmd->open = ret >= 0;
    }

  if (ret < 0)
    {
      fprintf(stderr, "wifi: cannot reach Wi-Fi: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  return OK;
}

static void wifi_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_cmd_s *cmd = (FAR struct wifi_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_wifi_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->open)
    {
      pnut_wifi_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

static int wifi_args(FAR struct wifi_cmd_s *cmd, int argc, FAR char *argv[])
{
  if (argc == 1)
    {
      cmd->op = WIFI_STATE;
      return OK;
    }

  if (strcmp(argv[1], "scan") == 0 && argc == 2)
    {
      cmd->op = WIFI_SCAN;
      return OK;
    }

  if (strcmp(argv[1], "leave") == 0 && argc == 2)
    {
      cmd->op = WIFI_LEAVE;
      return OK;
    }

  if (strcmp(argv[1], "join") == 0 && (argc == 3 || argc == 4) &&
      strlen(argv[2]) < sizeof(cmd->join.name) &&
      (argc == 3 || strlen(argv[3]) < sizeof(cmd->join.passphrase)))
    {
      cmd->op = WIFI_JOIN;
      strlcpy(cmd->join.name, argv[2], sizeof(cmd->join.name));
      if (argc == 4)
        {
          strlcpy(cmd->join.passphrase, argv[3],
                  sizeof(cmd->join.passphrase));
          explicit_bzero(argv[3], strlen(argv[3]));
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
  FAR struct wifi_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "wifi: no memory\n");
      return EXIT_FAILURE;
    }

  if (wifi_args(cmd, argc, argv) < 0)
    {
      wifi_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "wifi";
  cmd->module.start = wifi_start;
  cmd->module.stop  = wifi_stop;

  pnut_loop_defaults(&config);
  config.name    = "wifi";
  config.fds     = 3;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "wifi: no loop: %d\n", ret);
      explicit_bzero(&cmd->join, sizeof(cmd->join));
      free(cmd);
      return EXIT_FAILURE;
    }

  ret = pnut_module_add(loop, &cmd->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  explicit_bzero(&cmd->join, sizeof(cmd->join));
  free(cmd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
