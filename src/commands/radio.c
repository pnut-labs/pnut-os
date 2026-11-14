/****************************************************************************
 * pnut-os/src/commands/radio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* radio: the LoRa radio from the shell (RFC 0034), for developers.
 *   radio                                      the radios' state
 *   radio listen <MHz> <kHz> <sf> [<opt>...]   print the packets heard
 *   radio send <MHz> <kHz> <sf> <text> [<opt>...]  send one packet
 * Options: cr=<5-8> (5), sync=<byte> (0x12), pre=<symbols> (16),
 * power=<dBm> (14), crc=on|off (on), iq=normal|inverted,
 * duty=<listen ms>:<sleep ms> (always listening), time=<s> (listen: 60,
 * 0 for ever).  The radio is the first Board names; Radio checks a send
 * against the region's band plan.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/radio.pnut.h"
#include "pnut/radio.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_SERVICE    "radio"
#define RADIO_WAIT_MS    3000     /* For Radio's first answer */
#define RADIO_SEND_MS    30000    /* For a send's: SF12 takes seconds */
#define RADIO_CREDIT     4
#define RADIO_DUTY_MS    262143   /* The driver's longest listen or sleep */
#define RADIO_TIME_MAX   86400    /* Listening, at most a day */

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum radio_op_e
{
  RADIO_STATE = 0,
  RADIO_LISTEN,
  RADIO_SEND
};

struct radio_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_radio_client_s client;
  struct pnut_radio_info_reader_s reader;
  bool open;
  bool reading;
  bool streaming;                 /* Asked for, answered or not */
  bool opened;                    /* Answered */
  uint16_t stream;
  FAR struct pnut_timer_s *timer;
  uint8_t op;                     /* enum radio_op_e */
  uint32_t seconds;               /* Listening, 0 for ever */
  uint32_t heard;
  pnut_radio_open_request_t req;
  pnut_radio_send_request_t send;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* pnut.RadioError's codes */

static FAR const char * const g_radio_failures[] =
{
  "",
  "no such radio",
  "another caller has the radio",
  "not opened",
  "not in a sub-band of the region's plan, or the radio's band",
  "over the power limit",
  "the sub-band's air time for the hour is used up",
  "airplane mode",
  "no band plan for the region: set radio region",
  "a send not finished",
  "the radio cannot take that configuration",
  "the radio failed"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void radio_usage(void)
{
  fprintf(stderr,
          "usage: radio\n"
          "       radio listen <MHz> <kHz> <sf> [<option>...]\n"
          "       radio send <MHz> <kHz> <sf> <text> [<option>...]\n"
          "options: cr=<5-8> sync=<byte> pre=<symbols> power=<dBm>\n"
          "         crc=on|off iq=normal|inverted\n"
          "         duty=<listen ms>:<sleep ms> time=<s>\n");
}

/* A decimal number scaled by 1000 (MHz to kHz, kHz to Hz): "869.525",
 * "62.5"
 */

static int radio_thousandths(FAR const char *s, FAR uint32_t *value)
{
  uint64_t v = 0;
  int frac = -1;

  if (*s == '\0')
    {
      return -EINVAL;
    }

  for (; *s != '\0'; s++)
    {
      if (*s == '.' && frac < 0)
        {
          frac = 0;
          continue;
        }

      if (!isdigit((unsigned char)*s) || frac >= 3)
        {
          return -EINVAL;
        }

      v = v * 10 + (*s - '0');
      if (frac >= 0)
        {
          frac++;
        }

      if (v > UINT32_MAX)
        {
          return -EINVAL;
        }
    }

  for (frac = frac < 0 ? 0 : frac; frac < 3; frac++)
    {
      v *= 10;
    }

  if (v > UINT32_MAX)
    {
      return -EINVAL;
    }

  *value = (uint32_t)v;
  return OK;
}

static void radio_end(FAR struct radio_cmd_s *cmd, int ret)
{
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  if (cmd->streaming)
    {
      pnut_radio_open_cancel(&cmd->client, cmd->stream);
      cmd->streaming = false;
    }

  pnut_loop_stop(cmd->module.loop, ret);
}

static void radio_failed(FAR struct radio_cmd_s *cmd, int status,
                         FAR const pnut_error_t *err)
{
  if (err != NULL && err->code != 0 &&
      err->code < sizeof(g_radio_failures) / sizeof(g_radio_failures[0]))
    {
      if (err->text[0] != '\0')
        {
          fprintf(stderr, "radio: %s (%s)\n", g_radio_failures[err->code],
                  err->text);
        }
      else
        {
          fprintf(stderr, "radio: %s\n", g_radio_failures[err->code]);
        }
    }
  else
    {
      fprintf(stderr, "radio: %s\n",
              status == PNUT_STATUS_TIMEOUT ? "Radio did not answer" :
              status == PNUT_STATUS_INVALID ? "not understood" :
              "Radio cannot answer now");
    }

  radio_end(cmd, -EIO);
}

static void radio_sent(FAR struct pnut_radio_client_s *client, int status,
                       FAR const pnut_radio_send_reply_t *out,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      radio_failed(cmd, status, err);
      return;
    }

  printf("Sent %u bytes, %lu.%03lu ms on the air\n",
         (unsigned)cmd->send.data.size, (unsigned long)(out->air_us / 1000),
         (unsigned long)(out->air_us % 1000));
  radio_end(cmd, OK);
}

/* The stream opened: a send goes now; listening waits for packets */

static void radio_timeout(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);

static void radio_opened(FAR struct pnut_radio_client_s *client, int status,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;
  uint32_t ms;
  int ret;

  if (status != PNUT_STATUS_OK)
    {
      cmd->streaming = false;
      radio_failed(cmd, status, err);
      return;
    }

  /* Radio answered: listening lasts its time, a send gets its own */

  cmd->opened = true;
  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(cmd->module.loop, cmd->timer);
      cmd->timer = NULL;
    }

  ms = cmd->op == RADIO_LISTEN ? cmd->seconds * 1000 : RADIO_SEND_MS + 5000;
  if (ms > 0 && pnut_timer_start(cmd->module.loop, ms, 0, radio_timeout,
                                 cmd, &cmd->timer) < 0)
    {
      cmd->timer = NULL;
    }

  if (cmd->op == RADIO_LISTEN)
    {
      printf("Listening%s\n", cmd->seconds > 0 ? "" : " until stopped");
      return;
    }

  ret = pnut_radio_send(client, &cmd->send, RADIO_SEND_MS, radio_sent,
                        cmd);
  if (ret < 0)
    {
      fprintf(stderr, "radio: cannot send: %d\n", ret);
      radio_end(cmd, ret);
    }
}

static void radio_item(FAR struct pnut_radio_client_s *client, int status,
                       FAR const pnut_radio_packet_t *item, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;
  int snr;
  int i;

  if (item == NULL)
    {
      cmd->streaming = false;
      if (status != PNUT_STATUS_ENDED)
        {
          fprintf(stderr, "radio: the radio was taken back (%d)\n", status);
        }

      radio_end(cmd, status == PNUT_STATUS_ENDED ? OK : -EIO);
      return;
    }

  cmd->heard++;
  snr = item->snr_cdb;
  printf("%lu ms: %u bytes, %ld dBm, SNR %s%d.%02d dB%s:",
         (unsigned long)item->ended_ms, (unsigned)item->data.size,
         (long)item->rssi_dbm, snr < 0 ? "-" : "", abs(snr) / 100,
         abs(snr) % 100, item->crc_error ? ", CRC wrong" : "");
  for (i = 0; i < item->data.size; i++)
    {
      printf(" %02x", item->data.bytes[i]);
    }

  printf("\n");
  pnut_radio_open_credit(client, cmd->stream, 1);
}

static void radio_state(FAR struct pnut_radio_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;
  int ret;

  if (!connected || cmd->streaming)
    {
      return;
    }

  ret = pnut_radio_open(client, &cmd->req, RADIO_CREDIT, 0, radio_opened,
                        radio_item, cmd, &cmd->stream);
  if (ret < 0)
    {
      fprintf(stderr, "radio: cannot ask Radio: %d\n", ret);
      radio_end(cmd, ret);
      return;
    }

  cmd->streaming = true;
}

static void radio_info(FAR struct pnut_radio_info_reader_s *reader,
                       FAR const pnut_radio_info_t *msg, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;
  pb_size_t i;

  if (msg->radios_count == 0)
    {
      printf("No radio\n");
    }

  for (i = 0; i < msg->radios_count; i++)
    {
      printf("%s: ", msg->radios[i].name);
      if (msg->radios[i].open)
        {
          printf("open by %lu%s, ", (unsigned long)msg->radios[i].pid,
                 msg->radios[i].listen ==
                 PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE ?
                 " (duty cycle)" : "");
        }
      else
        {
          printf("free, ");
        }

      printf("%lu ms sent in the last hour\n",
             (unsigned long)msg->radios[i].air_ms);
    }

  printf("Region: %s%s\n", msg->region[0] != '\0' ? msg->region :
         "none (nothing is sent)", msg->airplane ? "; airplane mode" : "");
  radio_end(cmd, OK);
}

static void radio_timeout(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct radio_cmd_s *cmd = arg;

  cmd->timer = NULL;
  if (cmd->op == RADIO_LISTEN && cmd->opened)
    {
      printf("%lu packets heard\n", (unsigned long)cmd->heard);
      radio_end(cmd, OK);
      return;
    }

  fprintf(stderr, "radio: %s\n", cmd->opened ?
          "the send was not answered" :
          "Radio does not answer (is the radios program running?)");
  radio_end(cmd, -ETIMEDOUT);
}

static int radio_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct radio_cmd_s *cmd = (FAR struct radio_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, RADIO_WAIT_MS, 0, radio_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "radio: no timer: %d\n", ret);
      return ret;
    }

  if (cmd->op == RADIO_STATE)
    {
      ret = pnut_radio_info_subscribe(loop, radio_info, cmd, &cmd->reader);
      cmd->reading = ret >= 0;
    }
  else
    {
      ret = pnut_radio_connect(loop, RADIO_SERVICE, radio_state, cmd,
                               &cmd->client);
      cmd->open = ret >= 0;
    }

  if (ret < 0)
    {
      fprintf(stderr, "radio: cannot reach Radio: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  return OK;
}

static void radio_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct radio_cmd_s *cmd = (FAR struct radio_cmd_s *)module;

  if (cmd->reading)
    {
      pnut_radio_info_unsubscribe(&cmd->reader);
      cmd->reading = false;
    }

  if (cmd->open)
    {
      pnut_radio_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/* The configuration's words: <MHz> <kHz> <sf>, then options */

static int radio_option(FAR struct radio_cmd_s *cmd, FAR const char *arg)
{
  FAR pnut_radio_lora_t *lora = &cmd->req.lora;
  FAR const char *v = strchr(arg, '=');
  FAR char *end;
  long n;

  if (v == NULL)
    {
      return -EINVAL;
    }

  v++;
  if (strncmp(arg, "crc=", 4) == 0 || strncmp(arg, "iq=", 3) == 0)
    {
      if (arg[0] == 'c')
        {
          lora->crc = strcmp(v, "on") == 0;
          return lora->crc || strcmp(v, "off") == 0 ? OK : -EINVAL;
        }

      lora->invert_iq = strcmp(v, "inverted") == 0;
      return lora->invert_iq || strcmp(v, "normal") == 0 ? OK : -EINVAL;
    }

  if (strncmp(arg, "duty=", 5) == 0)
    {
      n = strtol(v, &end, 10);
      if (end == v || *end != ':' || n <= 0 || n > RADIO_DUTY_MS)
        {
          return -EINVAL;
        }

      cmd->req.listen    = PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE;
      cmd->req.listen_us = (uint32_t)n * 1000;
      v = end + 1;
      n = strtol(v, &end, 10);
      if (end == v || *end != '\0' || n <= 0 || n > RADIO_DUTY_MS)
        {
          return -EINVAL;
        }

      cmd->req.sleep_us = (uint32_t)n * 1000;
      return OK;
    }

  n = strtol(v, &end, 0);
  if (end == v || *end != '\0')
    {
      return -EINVAL;
    }

  if (strncmp(arg, "cr=", 3) == 0)
    {
      lora->coding_rate = (uint32_t)n;
    }
  else if (strncmp(arg, "sync=", 5) == 0)
    {
      lora->sync_word = (uint32_t)n;
    }
  else if (strncmp(arg, "pre=", 4) == 0)
    {
      lora->preamble = (uint32_t)n;
    }
  else if (strncmp(arg, "power=", 6) == 0)
    {
      lora->power_dbm = (int32_t)n;
    }
  else if (strncmp(arg, "time=", 5) == 0 && n >= 0 && n <= RADIO_TIME_MAX)
    {
      cmd->seconds = (uint32_t)n;
    }
  else
    {
      return -EINVAL;
    }

  return OK;
}

static int radio_args(FAR struct radio_cmd_s *cmd, int argc,
                      FAR char *argv[])
{
  FAR pnut_radio_lora_t *lora = &cmd->req.lora;
  uint32_t khz;
  int first;
  int i;

  if (argc == 1)
    {
      cmd->op = RADIO_STATE;
      return OK;
    }

  if (strcmp(argv[1], "listen") == 0)
    {
      cmd->op = RADIO_LISTEN;
      first   = 5;
    }
  else if (strcmp(argv[1], "send") == 0)
    {
      cmd->op = RADIO_SEND;
      first   = 6;
    }
  else
    {
      return -EINVAL;
    }

  if (argc < first)
    {
      return -EINVAL;
    }

  /* The first radio Board names, by the name Board gives the T-Deck's */

  strlcpy(cmd->req.radio, "lora", sizeof(cmd->req.radio));
  strlcpy(cmd->send.radio, "lora", sizeof(cmd->send.radio));
  cmd->req.has_lora = true;
  cmd->seconds      = 60;
  lora->coding_rate = 5;
  lora->sync_word   = 0x12;
  lora->preamble    = 16;
  lora->power_dbm   = 14;
  lora->crc         = true;

  if (radio_thousandths(argv[2], &khz) < 0 ||
      radio_thousandths(argv[3], &lora->bandwidth_hz) < 0)
    {
      return -EINVAL;
    }

  if (khz > UINT32_MAX / 1000)
    {
      return -EINVAL;
    }

  lora->frequency_hz     = khz * 1000;
  lora->spreading_factor = (uint32_t)atoi(argv[4]);

  if (cmd->op == RADIO_SEND)
    {
      cmd->send.data.size = strlen(argv[5]);
      if (cmd->send.data.size == 0 ||
          cmd->send.data.size > sizeof(cmd->send.data.bytes))
        {
          return -EINVAL;
        }

      memcpy(cmd->send.data.bytes, argv[5], cmd->send.data.size);
    }

  for (i = first; i < argc; i++)
    {
      if (radio_option(cmd, argv[i]) < 0)
        {
          return -EINVAL;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct radio_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "radio: no memory\n");
      return EXIT_FAILURE;
    }

  if (radio_args(cmd, argc, argv) < 0)
    {
      radio_usage();
      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "radio";
  cmd->module.start = radio_start;
  cmd->module.stop  = radio_stop;

  pnut_loop_defaults(&config);
  config.name    = "radio";
  config.fds     = 3;
  config.timers  = 2;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "radio: no loop: %d\n", ret);
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
