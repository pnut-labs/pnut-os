/****************************************************************************
 * pnut-os/tests/unit/test_telephony.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony (RFC 0030, step 1) against a fake modem: Board's description
 * names the modem and its lines; the modem answers once its rail is on
 * and its key pressed, takes AT+CMUX, opens 27.010's channels, answers the
 * commands on the AT channel, reports the network's time, and stops
 * answering after AT+CPOF.  The program powers it up, announces its state
 * and the time, and powers it down in order as it is asked to stop.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pb_encode.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "board.h"
#include "pnut_internal.h"
#include "telephony.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define T_UTC   1793363696          /* 2026-10-30 12:34:56 UTC */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The fake modem, and what the test saw */

struct tel_test_s
{
  FAR struct pnut_loop_s *loop;
  struct board_s board;
  struct telephony_s tel;
  struct pnut_module_s module;
  struct pnut_telephony_info_reader_s infos;
  struct pnut_network_time_reader_s times;
  char dir[64];
  char path[96];

  /* The modem */

  int uart;                       /* Its end of the UART */
  int lines[2];                   /* The rail's and the key's files */
  bool rail;
  bool key;
  bool on;                        /* Booted, answering */
  bool mux;                       /* In 27.010 */
  bool silent;                    /* Asked to stop: answers nothing */
  bool no_sim;
  bool left_on;                   /* On, in a multiplexer, at start */
  bool timed;                     /* The network's time sent */
  int cfun;                       /* 1 on the air, 4 off it */
  bool sim_air;                   /* Starts off the air, and goes on once
                                   * its SIM is ready, as the A7682E */
  uint64_t ready_at;              /* When its SIM answered ready */
  bool airplane;                  /* Airplane mode on, from the start */
  FAR struct pnut_publisher_s *airplanes;
  FAR const char *clock;          /* Its clock, as +CCLK gives it */
  uint64_t clock_at;              /* When its clock was read */
  FAR const char *ignore;         /* A command left unanswered, once */
  bool no_ua;                     /* Opens no channel */
  bool off_seen;                  /* The rail went off with the modem off */
  int rails_off;                  /* Times the rail went off */
  char line[256];
  size_t llen;
  uint8_t frame[300];
  size_t flen;
  char commands[2048];            /* Every command, ';' after each */
  int kicks;                      /* Close-downs seen */

  /* What the program announced */

  pnut_telephony_info_t info;
  int infos_seen;
  bool was_on;
  pnut_network_time_t time;
  int times_seen;
  uint64_t stop_at;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 27.010's FCS over a header */

static uint8_t tel_test_fcs(FAR const uint8_t *p, size_t n)
{
  uint8_t crc = 0xff;
  int i;

  while (n-- > 0)
    {
      crc ^= *p++;
      for (i = 0; i < 8; i++)
        {
          crc = (crc & 1) ? (crc >> 1) ^ 0xe0 : crc >> 1;
        }
    }

  return 0xff - crc;
}

/* The modem's bytes to the program */

static void tel_test_send(FAR struct tel_test_s *t, FAR const void *buf,
                          size_t len)
{
  assert_int_equal(write(t->uart, buf, len), (ssize_t)len);
}

static void tel_test_frame(FAR struct tel_test_s *t, uint8_t dlci,
                           uint8_t ctrl, FAR const void *info, size_t n)
{
  uint8_t buf[140];

  assert_true(n <= 127);
  buf[0] = 0xf9;
  buf[1] = (dlci << 2) | 0x01;          /* The modem's answers: no C/R */
  buf[2] = ctrl;
  buf[3] = (n << 1) | 0x01;
  memcpy(buf + 4, info, n);
  buf[4 + n] = tel_test_fcs(buf + 1, 3);
  buf[5 + n] = 0xf9;
  tel_test_send(t, buf, n + 6);
}

/* A line of the modem's, plain or on the AT channel */

static void tel_test_say(FAR struct tel_test_s *t, FAR const char *text)
{
  char buf[200];
  int n;

  n = snprintf(buf, sizeof(buf), "\r\n%s\r\n", text);
  if (t->mux)
    {
      tel_test_frame(t, 1, 0xef, buf, n);
    }
  else
    {
      tel_test_send(t, buf, n);
    }
}

/* A command, as the modem answers it */

static void tel_test_command(FAR struct tel_test_s *t, FAR const char *cmd)
{
  strlcat(t->commands, cmd, sizeof(t->commands));
  strlcat(t->commands, ";", sizeof(t->commands));

  if (t->silent || !t->on)
    {
      return;
    }

  if (t->ignore != NULL && strcmp(cmd, t->ignore) == 0)
    {
      t->ignore = NULL;
      return;
    }

  if (strcmp(cmd, "AT+CPOF") == 0)
    {
      tel_test_say(t, "OK");
      t->silent = true;
      return;
    }

  if (strncmp(cmd, "AT+CMUX=", 8) == 0)
    {
      tel_test_say(t, "OK");
      t->mux  = true;
      t->flen = 0;
      return;
    }

  if (strcmp(cmd, "AT+CPIN?") == 0)
    {
      tel_test_say(t, t->no_sim ? "+CME ERROR: SIM not inserted" :
                   "+CPIN: READY");
      if (!t->no_sim)
        {
          tel_test_say(t, "OK");
          if (t->ready_at == 0)
            {
              t->ready_at = pnut_now();
              if (t->sim_air)
                {
                  t->cfun = 1;    /* On the air with its SIM */
                }
            }
        }

      return;
    }

  if (strcmp(cmd, "AT+CFUN=4") == 0 || strcmp(cmd, "AT+CFUN=1") == 0)
    {
      t->cfun = cmd[8] - '0';
      tel_test_say(t, "OK");
      return;
    }

  if (strcmp(cmd, "AT+CSQ") == 0)
    {
      tel_test_say(t, "+CSQ: 20,99");
    }
  else if (strcmp(cmd, "AT+CEREG?") == 0)
    {
      tel_test_say(t, t->cfun == 4 ? "+CEREG: 2,0" :
                   "+CEREG: 2,1,\"1A2B\",\"01A2B3C4\",7");
    }
  else if (strcmp(cmd, "AT+CREG?") == 0)
    {
      tel_test_say(t, "+CREG: 2,0");
    }
  else if (strcmp(cmd, "AT+COPS?") == 0)
    {
      tel_test_say(t, t->cfun == 4 ? "+COPS: 0" : "+COPS: 0,0,\"Test\",7");
    }
  else if (strcmp(cmd, "AT+CNBP?") == 0)
    {
      tel_test_say(t, "+CNBP: 0x0000000000000180,0x00000000000800C5");
    }
  else if (strcmp(cmd, "AT+CNMP?") == 0)
    {
      tel_test_say(t, "+CNMP: 2");
    }
  else if (strcmp(cmd, "AT+CFUN?") == 0)
    {
      tel_test_say(t, t->cfun == 4 ? "+CFUN: 4" : "+CFUN: 1");
    }
  else if (strcmp(cmd, "AT+CCLK?") == 0)
    {
      tel_test_say(t, t->clock != NULL ? t->clock :
                   "+CCLK: \"26/10/30,13:34:56+04\"");
      t->clock_at = pnut_now();
    }

  tel_test_say(t, "OK");

  /* Once registered, the network's time, once */

  if (strcmp(cmd, "AT+COPS?") == 0 && !t->timed && t->cfun != 4)
    {
      t->timed = true;
      tel_test_say(t, "+CTZE: \"+04\",0,\"2026/10/30,13:34:56\"");
    }
}

/* Bytes of a command line, plain or from the AT channel */

static void tel_test_text(FAR struct tel_test_s *t, FAR const uint8_t *p,
                          size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (p[i] == '\r')
        {
          t->line[t->llen] = '\0';
          if (t->llen > 0)
            {
              tel_test_command(t, t->line);
            }

          t->llen = 0;
        }
      else if (p[i] != '\n' && t->llen < sizeof(t->line) - 1)
        {
          t->line[t->llen++] = p[i];
        }
    }
}

/* A whole frame from the program */

static void tel_test_on_frame(FAR struct tel_test_s *t)
{
  uint8_t dlci = t->frame[1] >> 2;
  uint8_t ctrl = t->frame[2] & ~0x10;
  size_t n = t->frame[3] >> 1;

  if (ctrl == 0x2f)
    {
      if (!t->no_ua)
        {
          tel_test_frame(t, dlci, 0x73, NULL, 0); /* SABM: UA */
        }
    }
  else if (ctrl == 0xef && dlci == 0)
    {
      if ((t->frame[4] & ~0x02) == 0xc1)          /* Close-down */
        {
          t->kicks++;
          t->mux = false;
        }
    }
  else if (ctrl == 0xef && dlci == 1)
    {
      tel_test_text(t, t->frame + 4, n);
    }
}

/* The modem reads its UART */

static void tel_test_uart(FAR struct pnut_loop_s *loop, int fd,
                          uint32_t events, FAR void *arg)
{
  FAR struct tel_test_s *t = arg;
  uint8_t buf[256];
  ssize_t n;
  ssize_t i;

  n = read(fd, buf, sizeof(buf));
  if (n <= 0 || !t->on)
    {
      return;                     /* Off: its UART is not powered */
    }

  if (!t->mux)
    {
      tel_test_text(t, buf, n);
      return;
    }

  for (i = 0; i < n; i++)
    {
      if (buf[i] == 0xf9)
        {
          if (t->flen >= 5)
            {
              t->frame[t->flen++] = 0xf9;
              tel_test_on_frame(t);
              t->flen = 0;
              if (!t->mux)
                {
                  /* Plain AT from the next byte on: a CR ends the
                   * close-down as a line
                   */

                  tel_test_text(t, buf + i + 1, n - i - 1);
                  return;
                }

              continue;
            }

          t->flen = 0;
          t->frame[t->flen++] = 0xf9;
          continue;
        }

      if (t->flen > 0 && t->flen < sizeof(t->frame) - 1)
        {
          t->frame[t->flen++] = buf[i];
        }
    }
}

/* The modem's lines: the rail, and the key, which starts it when pressed
 * and released with the rail on
 */

static int tel_test_open_uart(FAR void *arg, FAR const char *path)
{
  FAR struct tel_test_s *t = arg;
  int sv[2];

  assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  t->uart = sv[1];
  assert_int_equal(pnut_loop_watch(t->loop, t->uart, EPOLLIN,
                                   tel_test_uart, t), 0);
  return sv[0];
}

static int tel_test_open_line(FAR void *arg, FAR const char *path)
{
  FAR struct tel_test_s *t = arg;
  int fd = open("/dev/null", O_RDWR);

  assert_true(fd >= 0);
  if (strcmp(path, "/dev/test_pwr") == 0)
    {
      t->lines[0] = fd;
    }
  else
    {
      assert_string_equal(path, "/dev/test_key");
      t->lines[1] = fd;
    }

  return fd;
}

static int tel_test_set_line(FAR void *arg, int fd, bool high)
{
  FAR struct tel_test_s *t = arg;

  if (fd == t->lines[0])
    {
      if (t->rail && !high)
        {
          /* Cut only once the modem has shut down, never while it runs */

          t->rails_off++;
          t->off_seen = !t->on || t->silent;
          if (t->no_ua)
            {
              pnut_loop_stop(t->loop, 0);
            }
        }

      t->rail = high;
      if (!high)
        {
          t->on     = false;
          t->mux    = false;
          t->silent = false;
        }
    }
  else
    {
      assert_int_equal(fd, t->lines[1]);
      if (t->key && !high && t->rail && !t->on)
        {
          t->on   = true;         /* Booted at once */
          t->cfun = t->sim_air ? 4 : 1;
        }

      t->key = high;
    }

  return 0;
}

static int tel_test_get_line(FAR void *arg, int fd, FAR bool *high)
{
  FAR struct tel_test_s *t = arg;

  *high = fd == t->lines[0] ? t->rail : t->key;
  return 0;
}

/* What the program announces */

static void tel_test_info(FAR struct pnut_telephony_info_reader_s *reader,
                          FAR const pnut_telephony_info_t *msg,
                          FAR void *arg)
{
  FAR struct tel_test_s *t = arg;

  t->info = *msg;
  t->infos_seen++;
  if (msg->modem == PNUT_TELEPHONY_INFO_MODEM_ON &&
      msg->registration == PNUT_TELEPHONY_INFO_REGISTRATION_HOME &&
      msg->operator_name[0] != '\0')
    {
      t->was_on = true;
    }
}

static void tel_test_time(FAR struct pnut_network_time_reader_s *reader,
                          FAR const pnut_network_time_t *msg,
                          FAR void *arg)
{
  FAR struct tel_test_s *t = arg;

  if (msg->taken != 0)
    {
      t->time = *msg;
      t->times_seen++;
    }
}

/* Once the modem is on and the time is in, the program is asked to stop,
 * as NxInit does
 */

static void tel_test_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct tel_test_s *t = arg;

  if (t->stop_at == 0 &&
      ((t->was_on && t->times_seen > 0) ||
       (t->airplane && t->ready_at != 0 &&
        pnut_now() - t->ready_at > 500) ||
       (t->was_on && t->clock_at != 0 && pnut_now() - t->clock_at > 200) ||
       (t->no_sim && t->info.reason ==
        PNUT_TELEPHONY_INFO_REASON_NO_SIM &&
        t->info.modem == PNUT_TELEPHONY_INFO_MODEM_OFF)))
    {
      t->stop_at = pnut_now();
      pnut_loop_stop(loop, 0);
    }
}

static void tel_test_stop(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

static int tel_test_start(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct tel_test_s *t = (FAR struct tel_test_s *)
    ((FAR uint8_t *)module - offsetof(struct tel_test_s, module));

  assert_int_equal(pnut_telephony_info_subscribe(loop, tel_test_info, t,
                                                 &t->infos), 0);
  assert_int_equal(pnut_network_time_subscribe(loop, tel_test_time, t,
                                               &t->times), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, tel_test_tick, t, NULL),
                   0);
  if (t->airplane)
    {
      pnut_airplane_mode_t msg;

      memset(&msg, 0, sizeof(msg));
      msg.on = true;
      assert_int_equal(pnut_airplane_mode_advertise(loop, &t->airplanes), 0);
      assert_int_equal(pnut_airplane_mode_publish(t->airplanes, &msg), 0);
    }

  pnut_module_ready(module);
  return 0;
}

static void tel_test_stop_module(FAR struct pnut_module_s *module,
                                 FAR struct pnut_loop_s *loop)
{
  FAR struct tel_test_s *t = (FAR struct tel_test_s *)
    ((FAR uint8_t *)module - offsetof(struct tel_test_s, module));

  pnut_telephony_info_unsubscribe(&t->infos);
  pnut_network_time_unsubscribe(&t->times);
  if (t->airplanes != NULL)
    {
      pnut_topic_unadvertise(t->airplanes);
      t->airplanes = NULL;
    }
}

/* Board's description: the modem on its UART, with its rail and key */

static void tel_test_describe(FAR struct tel_test_s *t)
{
  FAR pnut_board_description_t *d = calloc(1, sizeof(*d));
  FAR pnut_board_part_t *part;
  uint8_t buffer[PNUT_BOARD_DESCRIPTION_SIZE];
  pb_ostream_t stream;
  FILE *f;

  assert_non_null(d);
  part = &d->parts[d->parts_count++];
  part->kind = PNUT_BOARD_PART_KIND_MODEM;
  strlcpy(part->name, "modem", sizeof(part->name));
  strlcpy(part->chip, "A7682E", sizeof(part->chip));
  strlcpy(part->device, "/dev/test_uart", sizeof(part->device));
  part->which_detail                    = PNUT_BOARD_PART_MODEM_TAG;
  part->detail.modem.lte_bands.size     = 3;
  part->detail.modem.lte_bands.bytes[0] = 0xc5;   /* 1, 3, 7, 8 */
  part->detail.modem.lte_bands.bytes[1] = 0x00;
  part->detail.modem.lte_bands.bytes[2] = 0x08;   /* 20 */
  part->detail.modem.gsm_bands_count    = 2;
  part->detail.modem.gsm_bands[0]       = PNUT_BOARD_MODEM_GSM_GSM_900;
  part->detail.modem.gsm_bands[1]       = PNUT_BOARD_MODEM_GSM_DCS_1800;

  d->lines_count = 2;
  d->lines[0].kind = PNUT_BOARD_PART_KIND_MODEM;
  strlcpy(d->lines[0].part, "modem", sizeof(d->lines[0].part));
  d->lines[0].role = PNUT_BOARD_LINE_ROLE_POWER;
  strlcpy(d->lines[0].device, "/dev/test_pwr", sizeof(d->lines[0].device));
  d->lines[1].kind = PNUT_BOARD_PART_KIND_MODEM;
  strlcpy(d->lines[1].part, "modem", sizeof(d->lines[1].part));
  d->lines[1].role = PNUT_BOARD_LINE_ROLE_POWER_KEY;
  strlcpy(d->lines[1].device, "/dev/test_key", sizeof(d->lines[1].device));

  stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
  assert_true(pb_encode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, d));
  f = fopen(t->path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buffer, 1, stream.bytes_written, f),
                   stream.bytes_written);
  fclose(f);
  free(d);
}

static FAR struct tel_test_s *tel_test_new(void)
{
  static int n;
  FAR struct tel_test_s *t = calloc(1, sizeof(*t));

  /* A directory of each test's own: one that fails leaves its own */

  assert_non_null(t);
  t->uart     = -1;
  t->lines[0] = -1;
  t->lines[1] = -1;
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-tel-%d-%d", (int)getpid(),
           n++);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->path, sizeof(t->path), "%s/board.pb", t->dir);
  tel_test_describe(t);
  return t;
}

static void tel_test_free(FAR struct tel_test_s *t)
{
  static FAR const char * const topics[] =
  {
    "telephony", "network_time", "airplane_mode", "settings"
  };

  char path[128];
  size_t i;

  for (i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    {
      snprintf(path, sizeof(path), "%s/topic.%s/last", t->dir, topics[i]);
      unlink(path);
      snprintf(path, sizeof(path), "%s/topic.%s", t->dir, topics[i]);
      rmdir(path);
    }

  unlink(t->path);
  assert_int_equal(rmdir(t->dir), 0);
  free(t);
}

/* Run the program, Board and the test's module, for 15 s at most */

static int tel_test_run(FAR struct tel_test_s *t)
{
  struct pnut_loop_config_s config;
  struct board_config_s bconfig;
  struct telephony_config_s tconfig;
  int ret;

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  config.finish = 10000;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  bconfig.path = t->path;
  board_init(&t->board, &bconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->board.module), 0);

  memset(&tconfig, 0, sizeof(tconfig));
  tconfig.board     = BOARD_SERVICE;
  tconfig.open_uart = tel_test_open_uart;
  tconfig.open_line = tel_test_open_line;
  tconfig.set_line  = tel_test_set_line;
  tconfig.get_line  = tel_test_get_line;
  tconfig.arg       = t;
  tconfig.settle    = 50;
  tconfig.key_off   = 50;
  tconfig.ready     = 20;
  tconfig.wait      = 50;
  tconfig.open      = 50;
  telephony_init(&t->tel, &tconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->tel.module), 0);
  if (getenv("TEL_DEBUG") != NULL)
    {
      t->tel.module.level = PNUT_LOG_DEBUG;
    }

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = tel_test_start;
  t->module.stop  = tel_test_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 15000, 0, tel_test_stop, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  if (t->uart >= 0)
    {
      close(t->uart);
    }

  return ret;
}

/****************************************************************************
 * Name: test_telephony_life
 *
 * Description:
 *   Powered up from off; the multiplexer's channels opened; the settings
 *   given; the SIM ready and the network found, announced; the network's
 *   time announced; and, asked to stop, powered down in order: AT+CPOF,
 *   then the rail once it stopped answering.
 *
 ****************************************************************************/

static void test_telephony_life(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  assert_int_equal(tel_test_run(t), 0);

  /* Up, through the multiplexer, its settings, the SIM and the network */

  assert_true(t->was_on);
  assert_non_null(strstr(t->commands, "AT+CMUX=0,0,5,127;"));
  assert_non_null(strstr(t->commands, "AT+CEMODE=2;"));
  assert_non_null(strstr(t->commands, "AT+CTZU=1;"));

  /* The bands were the description's; the mode was automatic: LTE only */

  assert_null(strstr(t->commands, "AT+CNBP=0x"));
  assert_non_null(strstr(t->commands, "AT+CNMP=38;"));

  /* The network's time, as the message came; no clock read, the modem
   * started here
   */

  assert_null(strstr(t->commands, "AT+CCLK?;"));

  assert_int_equal(t->times_seen, 1);
  assert_int_equal(t->time.utc, T_UTC);
  assert_true(t->time.has_offset);
  assert_int_equal(t->time.offset, 3600);
  assert_int_equal(t->time.uncertainty, 1000);
  assert_int_equal(t->time.kind, PNUT_NETWORK_TIME_KIND_MESSAGE);

  /* Down in order: AT+CPOF, the rail after; within the finish time */

  assert_non_null(strstr(t->commands, "AT+CPOF;"));
  assert_false(t->rail);
  assert_false(t->key);
  assert_true(pnut_now() - t->stop_at < 5000);

  /* The last state goes out as the loop ends: read it in the program */

  assert_int_equal(t->tel.state.modem, PNUT_TELEPHONY_INFO_MODEM_OFF);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_left_on
 *
 * Description:
 *   A modem found running at start, in a multiplexer a program left: closed
 *   down blindly, asked in plain AT, and taken as it is; without a SIM, it
 *   goes off.
 *
 ****************************************************************************/

static void test_telephony_left_on(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->rail   = true;
  t->on     = true;
  t->mux    = true;
  t->no_sim = true;
  assert_int_equal(tel_test_run(t), 0);

  assert_true(t->kicks >= 1);
  assert_true(strncmp(t->commands, "AT;", 3) == 0);
  assert_non_null(strstr(t->commands, "AT+CPIN?;"));
  assert_non_null(strstr(t->commands, "AT+CPOF;"));
  assert_int_equal(t->info.reason, PNUT_TELEPHONY_INFO_REASON_NO_SIM);
  assert_false(t->rail);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_airplane
 *
 * Description:
 *   Powered up in airplane mode, by a modem that starts off the air and
 *   goes on once its SIM is ready, as the A7682E does: airplane mode is
 *   given again, and the modem ends off the air.
 *
 ****************************************************************************/

static void test_telephony_airplane(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR const char *ready;

  t->airplane = true;
  t->sim_air  = true;
  assert_int_equal(tel_test_run(t), 0);

  ready = strstr(t->commands, "AT+CPIN?;");
  assert_non_null(ready);
  assert_non_null(strstr(ready, "AT+CFUN=4;"));
  assert_int_equal(t->cfun, 4);
  assert_false(t->was_on);
  assert_int_equal(t->times_seen, 0);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_clock_before
 *
 * Description:
 *   A modem found running, whose network sent its time before this start
 *   and does not again: its clock is read once, as the modem's, the
 *   network's time from an unknown time before.
 *
 ****************************************************************************/

static void test_telephony_clock_before(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->rail  = true;
  t->on    = true;
  t->mux   = true;
  t->timed = true;                /* Sent before: not again */
  assert_int_equal(tel_test_run(t), 0);

  assert_true(t->was_on);
  assert_non_null(strstr(t->commands, "AT+CCLK?;"));
  assert_int_equal(t->times_seen, 1);
  assert_int_equal(t->time.utc, T_UTC);
  assert_true(t->time.has_offset);
  assert_int_equal(t->time.offset, 3600);
  assert_false(t->time.dst);
  assert_int_equal(t->time.uncertainty, 60000);
  assert_int_equal(t->time.kind, PNUT_NETWORK_TIME_KIND_MODEM_CLOCK);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_clock_unset
 *
 * Description:
 *   A modem found running whose clock is in +00, as the A7682E's is until
 *   the network sends its time: read, and left aside.
 *
 ****************************************************************************/

static void test_telephony_clock_unset(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->rail  = true;
  t->on    = true;
  t->mux   = true;
  t->timed = true;
  t->clock = "+CCLK: \"26/10/30,13:34:56+00\"";
  assert_int_equal(tel_test_run(t), 0);

  assert_true(t->was_on);
  assert_non_null(strstr(t->commands, "AT+CCLK?;"));
  assert_int_equal(t->times_seen, 0);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_slow
 *
 * Description:
 *   A setting left unanswered is a failed one: the settings go on, and the
 *   modem comes up.
 *
 ****************************************************************************/

static void test_telephony_slow(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->ignore = "AT+CFGRI=1";
  assert_int_equal(tel_test_run(t), 0);
  assert_true(t->was_on);
  assert_null(t->ignore);
  assert_non_null(strstr(t->commands, "AT+CFGRI=1;AT+CSCLK=0;"));
  assert_true(t->off_seen);

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_no_channels
 *
 * Description:
 *   A modem that takes AT+CMUX and opens no channel: tried three times,
 *   then powered down in order, out of the multiplexer first, so that
 *   AT+CPOF reaches it; its rail cut only once it has shut down.
 *
 ****************************************************************************/

static void test_telephony_no_channels(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->no_ua = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_true(t->kicks >= 1);
  assert_non_null(strstr(t->commands, "AT+CMUX=0,0,5,127;AT+CPOF;"));
  assert_int_equal(t->rails_off, 1);
  assert_true(t->off_seen);

  tel_test_free(t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_telephony_life),
    cmocka_unit_test(test_telephony_left_on),
    cmocka_unit_test(test_telephony_airplane),
    cmocka_unit_test(test_telephony_clock_before),
    cmocka_unit_test(test_telephony_clock_unset),
    cmocka_unit_test(test_telephony_slow),
    cmocka_unit_test(test_telephony_no_channels),
  };

  return cmocka_run_group_tests_name("telephony", tests, NULL, NULL);
}
