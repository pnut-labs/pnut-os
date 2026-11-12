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

#include <dirent.h>
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

#include <arpa/inet.h>

#include <cmocka.h>

#include <pb_encode.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "board.h"
#include "messages.h"
#include "pnut_internal.h"
#include "store.h"
#include "telephony.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define T_UTC   1793363696          /* 2026-10-30 12:34:56 UTC */

/* Wi-Fi carrying the traffic before mobile data's standby, in ms */

#define TEL_WIFI_HOLD 400

/* SMS in the modem's memory (the codec's own vectors, test_sms.c) */

#define T_SCTS    "62010321300040"
#define T_HELLO   "07917283010010F5040BC87238880900F1000099" \
                  "3092516195800AE8329BFD4697D9EC37"
#define T_LONG1   "00440B918421436587F90000" T_SCTS "090500037B0201D069"
#define T_LONG2   "00440B918421436587F90000" T_SCTS "090500037B0202D069"
#define T_STATUS  "000600"
#define T_DATA    "00040B918421436587F90004" T_SCTS "02CAFE"
#define T_ZIPPED  "00040B918421436587F90020" T_SCTS "02E834"
#define T_UCS2    "00040B918421436587F90008" T_SCTS "0401050119"

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
  bool air_flip;                  /* Then back on the air by itself, out
                                   * of coverage */
  bool air_stubborn;              /* and staying there, CFUN=4 or not */
  bool flipped;
  int air_given;                  /* CFUN=4 given after the flip */
  FAR struct pnut_publisher_s *airplanes;
  FAR const char *clock;          /* Its clock, as +CCLK gives it */
  uint64_t clock_at;              /* When its clock was read */
  FAR const char *ignore;         /* A command left unanswered, once */
  bool no_ua;                     /* Opens no channel */
  bool off_seen;                  /* The rail went off with the modem off */
  int rails_off;                  /* Times the rail went off */
  char line[512];
  size_t llen;
  uint8_t frame[300];
  size_t flen;
  char commands[2048];            /* Every command, ';' after each */
  int kicks;                      /* Close-downs seen */

  /* SMS: the modem's memory, and Messages to deliver to */

  bool sms;
  struct messages_s messages;
  char store[96];
  int sms_total;                  /* The memory's slots */
  uint64_t sms_limit;             /* Messages' store's; 0 for 64 KB */
  FAR const char *slots[32];      /* Each slot's PDU, or NULL */
  FAR const char *sim_slots[32];  /* The SIM's memory, alike */
  int sim_total;                  /* Its slots; 0 for 25 */
  bool on_sim;                    /* AT+CMGR and AT+CMGD read the SIM's */
  int deletes;                    /* AT+CMGD seen */
  int cmgd_fail;                  /* AT+CMGD refused, so many times */
  uint64_t deleted_at;
  int expect_deletes;             /* Then the run ends */
  int cmti_after;                 /* Deletes before an SMS comes */
  int cmti_slot;                  /* Where it is stored; -1 none */
  FAR const char *cmti_pdu;
  int cmgs;                       /* SMS parts sent */
  int expect_cmgs;                /* Then the run ends */
  uint64_t cmgs_at;
  bool cmgs_fail;                 /* Every part refused */
  bool texting;                   /* After the prompt, the PDU comes */

  /* Mobile data: channel 2 dials, PPP's bytes go both ways, and the
   * link is the test's
   */

  bool data;                      /* Wanted from the start */
  bool data_fail;                 /* ATD answered NO CARRIER */
  bool data_stop_up;              /* The program stops while data is up */
  bool data_left_on;              /* A call left in data mode at start */
  bool data_once;                 /* The run ends once data is on */
  FAR const char *apn;            /* The access point's name, or NULL */
  bool data_connected;            /* Past CONNECT, till the hang-up */
  bool d2;                        /* AT&D2 given: DTR dropped hangs up;
                                   * else ignored, as the A7682E starts */
  bool hung_up;                   /* A call ended by DTR */
  bool again;                     /* Data off, then on again */
  int escapes;                    /* "+++" taken in data mode */
  int probes2;                    /* AT&D2 on channel 2 */
  bool dialing;                   /* ATD taken, CONNECT not yet sent */
  uint64_t connect_at;            /* When CONNECT goes */
  int dials;
  int hangups;
  int at_during_dial;             /* AT channel commands while dialing */
  int at_after_dial;
  int link_starts;
  int link_stops;
  int link_tty;                   /* The link's end of the terminal */
  int link_fd;                    /* Its records to the program */
  bool data_sent;                 /* PPP's test frames sent */
  bool data_done;                 /* Both arrived */
  uint64_t done_at;
  int data_at_stop;               /* The data state when the run ended */
  bool data_wifi;                 /* Wi-Fi carries the traffic, a while */
  FAR struct pnut_publisher_s *links;
  uint8_t wifi_step;
  uint64_t wifi_at;
  char line2[64];                 /* Channel 2's command line */
  size_t l2len;
  uint8_t from_modem[64];         /* What the link read from its end */
  size_t fm_len;
  uint8_t to_modem[64];           /* What channel 2 carried after CONNECT */
  size_t tm_len;
  struct data_link_ops_s link;

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
  if (n > 0)
    {
      memcpy(buf + 4, info, n);
    }

  buf[4 + n] = tel_test_fcs(buf + 1, 3);
  buf[5 + n] = 0xf9;
  tel_test_send(t, buf, n + 6);
}

/* A part's PDU, after the prompt: sent, or refused */

static void tel_test_say(FAR struct tel_test_s *t, FAR const char *text);

static void tel_test_pdu(FAR struct tel_test_s *t, FAR const char *pdu)
{
  char line[32];

  strlcat(t->commands, "PDU;", sizeof(t->commands));
  assert_true(strlen(pdu) >= 2 * 8);
  if (t->cmgs_fail)
    {
      tel_test_say(t, "+CMS ERROR: 500");
    }
  else
    {
      snprintf(line, sizeof(line), "+CMGS: %d", ++t->cmgs);
      tel_test_say(t, line);
      tel_test_say(t, "OK");
    }

  t->cmgs_at = pnut_now();
}

/* A line of the modem's, plain or on the AT channel */

static void tel_test_say(FAR struct tel_test_s *t, FAR const char *text)
{
  char buf[600];
  int n;
  int i;

  n = snprintf(buf, sizeof(buf), "\r\n%s\r\n", text);
  assert_true(n < (int)sizeof(buf));
  if (t->mux)
    {
      /* A long line, as a PDU, in several frames */

      for (i = 0; i < n; i += 120)
        {
          tel_test_frame(t, 1, 0xef, buf + i, n - i < 120 ? n - i : 120);
        }
    }
  else
    {
      tel_test_send(t, buf, n);
    }
}

/* The modem's SMS commands: its memory chosen and counted, a slot read or
 * deleted; an SMS arriving after some deletes
 */

static bool tel_test_sms(FAR struct tel_test_s *t, FAR const char *cmd)
{
  char line[64];
  int used = 0;
  int n;
  int i;

  if (strcmp(cmd, "AT+CPMS=\"ME\",\"ME\",\"ME\"") == 0)
    {
      t->on_sim = false;
      for (i = 0; i < t->sms_total; i++)
        {
          used += t->slots[i] != NULL;
        }

      snprintf(line, sizeof(line), "+CPMS: %d,%d,%d,%d,%d,%d", used,
               t->sms_total, used, t->sms_total, used, t->sms_total);
      tel_test_say(t, line);
      tel_test_say(t, "OK");
      return true;
    }

  if (strcmp(cmd, "AT+CPMS=\"SM\",\"SM\",\"ME\"") == 0)
    {
      int total = t->sim_total != 0 ? t->sim_total : 25;
      int me    = 0;

      t->on_sim = true;
      for (i = 0; i < total; i++)
        {
          used += t->sim_slots[i] != NULL;
        }

      for (i = 0; i < t->sms_total; i++)
        {
          me += t->slots[i] != NULL;
        }

      snprintf(line, sizeof(line), "+CPMS: %d,%d,%d,%d,%d,%d", used, total,
               used, total, me, t->sms_total);
      tel_test_say(t, line);
      tel_test_say(t, "OK");
      return true;
    }

  if (strncmp(cmd, "AT+CMGR=", 8) == 0)
    {
      FAR const char **mem = t->on_sim ? t->sim_slots : t->slots;
      int total = t->on_sim ? (t->sim_total != 0 ? t->sim_total : 25) :
                  t->sms_total;

      n = atoi(cmd + 8);
      if (n >= 0 && n < total && mem[n] != NULL)
        {
          FAR const char *pdu = mem[n];
          unsigned int smsc;
          int len;

          /* The TPDU's octets: the SMSC's address not counted */

          assert_int_equal(sscanf(pdu, "%2x", &smsc), 1);
          len = (int)(strlen(pdu) / 2) - 1 - (int)smsc;
          snprintf(line, sizeof(line), "+CMGR: 0,,%d", len < 1 ? 1 : len);
          tel_test_say(t, line);
          tel_test_say(t, pdu);
        }

      tel_test_say(t, "OK");
      return true;
    }

  if (strncmp(cmd, "AT+CMGS=", 8) == 0)
    {
      static const char prompt[] = "\r\n> ";

      /* The prompt has no line end */

      t->texting = true;
      if (t->mux)
        {
          tel_test_frame(t, 1, 0xef, prompt, sizeof(prompt) - 1);
        }
      else
        {
          tel_test_send(t, prompt, sizeof(prompt) - 1);
        }

      return true;
    }

  if (strncmp(cmd, "AT+CMGD=", 8) == 0 && t->cmgd_fail > 0)
    {
      t->cmgd_fail--;
      tel_test_say(t, "+CMS ERROR: 500");
      return true;
    }

  if (strncmp(cmd, "AT+CMGD=", 8) == 0)
    {
      n = atoi(cmd + 8);
      if (t->on_sim && n >= 0 && n < 32)
        {
          t->sim_slots[n] = NULL;
        }
      else if (n >= 0 && n < t->sms_total)
        {
          t->slots[n] = NULL;
        }

      t->deletes++;
      t->deleted_at = pnut_now();
      tel_test_say(t, "OK");
      if (t->cmti_slot >= 0 && t->deletes == t->cmti_after)
        {
          t->slots[t->cmti_slot] = t->cmti_pdu;
          snprintf(line, sizeof(line), "+CMTI: \"ME\",%d", t->cmti_slot);
          t->cmti_slot = -1;
          tel_test_say(t, line);
        }

      return true;
    }

  return false;
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

  if (tel_test_sms(t, cmd))
    {
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
      if (t->flipped && cmd[8] == '4')
        {
          t->air_given++;
        }

      if (!t->flipped || !t->air_stubborn)
        {
          t->cfun = cmd[8] - '0';
        }

      tel_test_say(t, "OK");
      return;
    }

  if (strcmp(cmd, "AT+CSQ") == 0)
    {
      tel_test_say(t, "+CSQ: 20,99");
    }
  else if (strcmp(cmd, "AT+CEREG?") == 0)
    {
      tel_test_say(t, t->cfun == 4 || t->flipped ? "+CEREG: 2,0" :
                   "+CEREG: 2,1,\"1A2B\",\"01A2B3C4\",7");
    }
  else if (strcmp(cmd, "AT+CREG?") == 0)
    {
      tel_test_say(t, "+CREG: 2,0");
    }
  else if (strcmp(cmd, "AT+COPS?") == 0)
    {
      tel_test_say(t, t->cfun == 4 || t->flipped ? "+COPS: 0" :
                   "+COPS: 0,0,\"Test\",7");
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
      if (t->texting && (p[i] == 0x1a || p[i] == 0x1b))
        {
          /* The PDU ends with Ctrl-Z; ESC drops it */

          t->line[t->llen] = '\0';
          t->texting = false;
          if (p[i] == 0x1a)
            {
              tel_test_pdu(t, t->line);
            }

          t->llen = 0;
        }
      else if (p[i] == '\r')
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

/* Channel 2: "AT" and the dial, then PPP's bytes */

static void tel_test_data(FAR struct tel_test_s *t, FAR const uint8_t *p,
                          size_t n)
{
  size_t i;

  if (t->data_connected)
    {
      if (n == 3 && memcmp(p, "+++", 3) == 0)
        {
          t->escapes++;
          t->data_connected = false;
          tel_test_frame(t, 2, 0xef, "\r\nOK\r\n", 6);
          return;
        }

      for (i = 0; i < n && t->tm_len < sizeof(t->to_modem); i++)
        {
          t->to_modem[t->tm_len++] = p[i];
        }

      return;
    }

  for (i = 0; i < n; i++)
    {
      if (p[i] != '\r')
        {
          if (t->l2len < sizeof(t->line2) - 1)
            {
              t->line2[t->l2len++] = (char)p[i];
            }

          continue;
        }

      t->line2[t->l2len] = '\0';
      t->l2len = 0;
      if (strcmp(t->line2, "AT") == 0)
        {
          tel_test_frame(t, 2, 0xef, "\r\nOK\r\n", 6);
        }
      else if (strcmp(t->line2, "AT&D2") == 0 ||
               strcmp(t->line2, "ATH&D2") == 0)
        {
          t->probes2 += t->line2[2] == '&';
          t->d2 = true;
          tel_test_frame(t, 2, 0xef, "\r\nOK\r\n", 6);
        }
      else if (strcmp(t->line2, "ATD*99***1#") == 0)
        {
          t->dials++;
          if (t->data_fail)
            {
              tel_test_frame(t, 2, 0xef, "\r\nNO CARRIER\r\n", 14);
              continue;
            }

          /* CONNECT a moment later; meanwhile a report on the AT channel
           * asks for a refresh, whose commands must wait
           */

          t->dialing    = true;
          t->connect_at = pnut_now() + 300;
          tel_test_say(t, "+CEREG: 1");
        }
    }
}

/* The test's link, in place of pppd: its end of the terminal read, and
 * its records written as the test goes
 */

static void tel_test_link_read(FAR struct pnut_loop_s *loop, int fd,
                               uint32_t events, FAR void *arg)
{
  FAR struct tel_test_s *t = arg;
  ssize_t n;

  n = read(fd, t->from_modem + t->fm_len,
           sizeof(t->from_modem) - t->fm_len);
  if (n > 0)
    {
      t->fm_len += n;
    }
}

static void tel_test_link_tell(FAR struct tel_test_s *t, uint8_t event)
{
  struct data_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.event = event;
  if (event == DATA_LINK_UP)
    {
      strlcpy(ev.ifname, "ppp0", sizeof(ev.ifname));
      ev.local.s_addr  = htonl(0x0a000001);
      ev.dns[1].s_addr = htonl(0x0a000035);   /* The first not given */
    }

  assert_int_equal(write(t->link_fd, &ev, sizeof(ev)), sizeof(ev));
}

static int tel_test_link_start(FAR void *arg, FAR const char *tty, int fd)
{
  FAR struct tel_test_s *t = arg;

  t->link_starts++;
  t->link_tty = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
  assert_true(t->link_tty >= 0);
  assert_int_equal(pnut_loop_watch(t->loop, t->link_tty, EPOLLIN,
                                   tel_test_link_read, t), 0);
  t->link_fd = fd;
  tel_test_link_tell(t, DATA_LINK_UP);
  return 0;
}

static void tel_test_link_stop(FAR void *arg)
{
  FAR struct tel_test_s *t = arg;

  t->link_stops++;
  pnut_loop_unwatch(t->loop, t->link_tty);
  close(t->link_tty);
  t->link_tty = -1;
  tel_test_link_tell(t, DATA_LINK_ENDED);
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
      else if ((t->frame[4] & ~0x02) == 0xe1 && n >= 4 &&
               (t->frame[6] >> 2) == 2 && t->frame[7] == 0x89)
        {
          t->hangups++;                           /* MSC: DTR dropped */
          if (t->d2 && (t->data_connected || t->dialing))
            {
              t->hung_up        = true;
              t->data_connected = false;
              t->dialing        = false;
              tel_test_frame(t, 2, 0xef, "\r\nNO CARRIER\r\n", 14);
            }
        }
    }
  else if (ctrl == 0xef && dlci == 1)
    {
      if (t->dialing)
        {
          t->at_during_dial++;
        }
      else if (t->data_connected)
        {
          t->at_after_dial++;
        }

      tel_test_text(t, t->frame + 4, n);
    }
  else if (ctrl == 0xef && dlci == 2)
    {
      tel_test_data(t, t->frame + 4, n);
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
      /* A flag, unless the length says the frame goes on: the length
       * octet itself is 0xf9 for 124 bytes
       */

      if (buf[i] == 0xf9 &&
          (t->flen <= 2 ||
           (t->flen >= 4 && t->flen >= 5 + (size_t)(t->frame[3] >> 1))))
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

/* Network's link in use, as its topic says */

static void tel_test_link(FAR struct tel_test_s *t,
                          pnut_network_info_link_t link)
{
  pnut_network_info_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.link = link;
  if (link != PNUT_NETWORK_INFO_LINK_NO_LINK)
    {
      msg.metered = link == PNUT_NETWORK_INFO_LINK_MOBILE;
      msg.address = htonl(0xc0a80102);
    }

  assert_int_equal(pnut_network_info_publish(t->links, &msg), 0);
  t->wifi_at = pnut_now();
  t->wifi_step++;
}

/* Data up; Wi-Fi in use for less than the hold, then gone: the call
 * kept; Wi-Fi in use again: standby after the hold, the call hung up;
 * Wi-Fi gone, the hang-up long over: dialled again, up
 */

static void tel_test_wifi_tick(FAR struct tel_test_s *t,
                               FAR struct pnut_loop_s *loop)
{
  uint64_t since = pnut_now() - t->wifi_at;

  switch (t->wifi_step)
    {
      case 0:
        if (t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_ON)
          {
            tel_test_link(t, PNUT_NETWORK_INFO_LINK_WIFI);
          }
        break;

      case 1:
        if (since >= TEL_WIFI_HOLD / 4)
          {
            tel_test_link(t, PNUT_NETWORK_INFO_LINK_NO_LINK);
          }
        break;

      case 2:
        if (since >= TEL_WIFI_HOLD * 3 / 2)
          {
            assert_int_equal(t->hangups, 0);
            assert_int_equal(t->dials, 1);
            assert_int_equal(t->info.data, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
            tel_test_link(t, PNUT_NETWORK_INFO_LINK_WIFI);
          }
        break;

      case 3:
        if (t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_STANDBY &&
            t->hung_up && t->link_stops > 0)
          {
            assert_true(since >= TEL_WIFI_HOLD);
            assert_int_equal(t->info.data_address, 0);
            t->wifi_at = pnut_now();
            t->wifi_step++;
          }
        break;

      case 4:                     /* The hang-up over: standby, idle */
        if (since >= DATA_DTR_MS + 500)
          {
            assert_int_equal(t->info.data,
                             PNUT_TELEPHONY_INFO_DATA_DATA_STANDBY);
            assert_int_equal(t->dials, 1);
            tel_test_link(t, PNUT_NETWORK_INFO_LINK_NO_LINK);
          }
        break;

      case 5:
        if (t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_ON &&
            t->stop_at == 0)
          {
            t->data_at_stop = t->info.data;
            t->stop_at      = pnut_now();
            pnut_loop_stop(loop, 0);
          }
        break;
    }
}

static void tel_test_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct tel_test_s *t = arg;

  if (t->air_flip && !t->flipped && t->ready_at != 0 &&
      pnut_now() - t->ready_at > 300)
    {
      t->flipped = true;
      t->cfun    = 1;
      tel_test_say(t, "+CEREG: 0");     /* Not registered: no change */
    }

  if (t->dialing && pnut_now() >= t->connect_at)
    {
      t->dialing        = false;
      t->data_connected = true;
      tel_test_frame(t, 2, 0xef, "\r\nCONNECT 150000000\r\n", 21);
    }

  if (t->data_wifi)
    {
      tel_test_wifi_tick(t, loop);
      return;
    }

  if (t->data)
    {
      /* Up: a PPP frame each way, then data off (or the program asked to
       * stop); a failed dial: its hang-up
       */

      if (t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_ON &&
          !t->data_sent && !t->data_left_on && !t->data_once)
        {
          t->data_sent = true;
          assert_int_equal(t->info.data_address, htonl(0x0a000001));
          assert_int_equal(t->info.data_dns_count, 1);
          assert_int_equal(t->info.data_dns[0], htonl(0x0a000035));
          tel_test_frame(t, 2, 0xef, "\x7e\xff\x03he\rl\nlo\x7e", 11);
          assert_int_equal(write(t->link_tty, "\x7e\xff\x03wo\rr\nld\x7e",
                                 11), 11);
          return;
        }

      if (t->data_sent && !t->data_done && t->fm_len >= 11 &&
          t->tm_len >= 11)
        {
          t->data_done = true;
          t->done_at   = pnut_now();
          if (t->data_stop_up)
            {
              t->data_at_stop = t->info.data;
              t->stop_at      = pnut_now();
              pnut_loop_stop(loop, 0);
            }
          else
            {
              data_want(&t->tel.data, false, "");
            }

          return;
        }

      /* Off: on again, which dials again */

      if (t->data_done && !t->data_stop_up && !t->again &&
          t->hangups > 0 &&
          t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_OFF)
        {
          assert_int_equal(t->info.data_address, 0);
          assert_int_equal(t->info.data_dns_count, 0);
          t->again = true;
          data_want(&t->tel.data, true, "");
          return;
        }

      if (t->stop_at == 0 &&
          (((t->again || t->data_left_on || t->data_once) &&
            t->info.data == PNUT_TELEPHONY_INFO_DATA_DATA_ON) ||
           (t->data_fail && t->hangups > 0 &&
            t->info.data == PNUT_TELEPHONY_INFO_DATA_CONNECTING) ||
           (t->apn != NULL && !t->data_once && t->tel.data.fails > 0)))
        {
          t->data_at_stop = t->info.data;
          t->stop_at      = pnut_now();
          pnut_loop_stop(loop, 0);
        }

      return;
    }

  if (t->sms)
    {
      /* Every SMS dealt with, and a moment for anything more */

      if (t->stop_at == 0 && t->deletes >= t->expect_deletes &&
          (t->expect_cmgs == 0 || t->cmgs >= t->expect_cmgs ||
           (t->cmgs_fail && t->cmgs_at != 0)) &&
          pnut_now() - t->deleted_at > 300 &&
          pnut_now() - t->cmgs_at > 300)
        {
          t->stop_at = pnut_now();
          pnut_loop_stop(loop, 0);
        }

      return;
    }

  if (t->stop_at == 0 &&
      ((t->was_on && t->times_seen > 0) ||
       (t->airplane && t->ready_at != 0 &&
        pnut_now() - t->ready_at > (t->air_flip ? 1000 : 500)) ||
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

  if (t->data)
    {
      data_want(&t->tel.data, true, t->apn != NULL ? t->apn : "");
    }

  if (t->data_wifi)
    {
      assert_int_equal(pnut_network_info_advertise(loop, &t->links), 0);
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

  if (t->links != NULL)
    {
      pnut_topic_unadvertise(t->links);
      t->links = NULL;
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
  t->uart      = -1;
  t->lines[0]  = -1;
  t->lines[1]  = -1;
  t->cmti_slot = -1;
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-tel-%d-%d", (int)getpid(),
           n++);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->path, sizeof(t->path), "%s/board.pb", t->dir);
  snprintf(t->store, sizeof(t->store), "%s/store", t->dir);
  tel_test_describe(t);
  return t;
}

static void tel_test_free(FAR struct tel_test_s *t)
{
  static FAR const char * const topics[] =
  {
    "telephony", "network_time", "airplane_mode", "settings", "messages",
    "network"
  };

  char path[128 + 256];
  struct dirent *de;
  size_t i;
  DIR *d;

  /* Messages' store */

  d = opendir(t->store);
  if (d != NULL)
    {
      while ((de = readdir(d)) != NULL)
        {
          if (de->d_name[0] != '.')
            {
              snprintf(path, sizeof(path), "%s/%s", t->store, de->d_name);
              unlink(path);
            }
        }

      closedir(d);
      rmdir(t->store);
    }

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
  if (t->data)
    {
      t->link.start = tel_test_link_start;
      t->link.stop  = tel_test_link_stop;
      t->link.arg   = t;
      t->link_tty   = -1;
      tconfig.link  = &t->link;
    }

  if (t->data_wifi)
    {
      tconfig.data_wifi_hold = TEL_WIFI_HOLD;
    }

  if (t->sms)
    {
      struct messages_config_s mconfig;

      memset(&mconfig, 0, sizeof(mconfig));
      mconfig.dir    = t->store;
      mconfig.limit  = t->sms_limit != 0 ? t->sms_limit : 64 * 1024;
      mconfig.segmax = STORE_SEGMENT_MAX;
      mconfig.retry  = 100;
      messages_init(&t->messages, &mconfig);
      assert_int_equal(pnut_module_add(t->loop, &t->messages.module), 0);
      tconfig.messages = MESSAGES_SERVICE;
      tconfig.sms_tick = 200;
    }

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
  telephony_deinit(&t->tel);
  if (t->sms)
    {
      messages_deinit(&t->messages);
    }

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
 * Name: test_telephony_airplane_backstop
 *
 * Description:
 *   In airplane mode, a modem that goes back on the air by itself, out of
 *   coverage, reports only that it is not registered: its state is read,
 *   and airplane mode given again.  One that stays on the air whatever it
 *   is told is told again once a minute at most, not at each refresh.
 *
 ****************************************************************************/

static void test_telephony_airplane_backstop(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->airplane = true;
  t->air_flip = true;
  assert_int_equal(tel_test_run(t), 0);

  assert_true(t->flipped);
  assert_int_equal(t->air_given, 1);
  assert_int_equal(t->cfun, 4);
  tel_test_free(t);

  t = tel_test_new();
  t->airplane     = true;
  t->air_flip     = true;
  t->air_stubborn = true;
  assert_int_equal(tel_test_run(t), 0);

  assert_true(t->flipped);
  assert_int_equal(t->air_given, 1);
  assert_int_equal(t->cfun, 1);
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

/* A conversation's newest page, read from Messages' store */

static void tel_test_page(FAR struct store_s *store, FAR const char *peer,
                          FAR pnut_messages_page_t *page)
{
  FAR struct store_read_s *r = calloc(1, sizeof(*r));
  FAR struct store_conv_s *conv;
  bool more;

  assert_non_null(r);
  conv = store_find_peer(store, "sms", peer);
  assert_non_null(conv);
  assert_int_equal(store_plan_page(store, conv->id, 0, r, &more), 0);
  store_read_run(r);
  assert_int_equal(r->ret, 0);
  memset(page, 0, sizeof(*page));
  assert_int_equal(store_page(store, r, page), 0);
  free(r);
}

/****************************************************************************
 * Name: test_telephony_sms
 *
 * Description:
 *   The modem's memory read through once Messages has sms registered: a
 *   whole SMS delivered, and a copy of it stored once; a long one's two
 *   parts joined; a status report and 8-bit data deleted unread; one
 *   that does not decode kept; then one announced as it arrives.  Each
 *   delivered one's slots deleted once Messages answered.
 *
 ****************************************************************************/

static void test_telephony_sms(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR pnut_messages_page_t *page = calloc(1, sizeof(*page));
  struct store_s store;
  int i;

  assert_non_null(page);
  t->sms        = true;
  t->sms_total  = 10;
  t->slots[0]   = T_HELLO;
  t->slots[1]   = T_LONG1;
  t->slots[2]   = T_STATUS;
  t->slots[3]   = T_DATA;
  t->slots[4]   = T_ZIPPED;
  t->slots[5]   = T_LONG2;
  t->slots[6]   = T_HELLO;
  t->cmti_after = 6;
  t->cmti_slot  = 7;
  t->cmti_pdu   = T_UCS2;
  t->expect_deletes = 7;
  assert_int_equal(tel_test_run(t), 0);

  assert_non_null(strstr(t->commands, "AT+CPMS=\"ME\",\"ME\",\"ME\";"));
  for (i = 0; i < t->sms_total; i++)
    {
      if (i == 4)
        {
          assert_non_null(t->slots[i]);
        }
      else
        {
          assert_null(t->slots[i]);
        }
    }

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  assert_int_equal(store.nconvs, 2);
  assert_int_equal(store_unread(&store), 3);

  tel_test_page(&store, "27838890001", page);
  assert_int_equal(page->messages_count, 1);
  assert_string_equal(page->messages[0].text, "hellohello");
  assert_false(page->messages[0].out);
  assert_true(page->messages[0].sent != 0);

  tel_test_page(&store, "+48123456789", page);
  assert_int_equal(page->messages_count, 2);
  assert_string_equal(page->messages[0].text, "ąę");
  assert_string_equal(page->messages[1].text, "hihi");

  store_free(&store);
  free(page);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_long
 *
 * Description:
 *   A long SMS of 24 UCS-2 parts, 1608 characters: more than a message
 *   holds, so delivered as two, in order, and its 24 slots deleted.
 *
 ****************************************************************************/

static void test_telephony_sms_long(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR pnut_messages_page_t *page = calloc(1, sizeof(*page));
  static char parts[24][2 * 170];
  struct store_s store;
  size_t n;
  int i;
  int j;

  assert_non_null(page);
  for (i = 0; i < 24; i++)
    {
      n = snprintf(parts[i], sizeof(parts[i]), "00440B918421436587F90008"
                   T_SCTS "%02X05000355%02X%02X", 6 + 2 * 67, 24, i + 1);
      for (j = 0; j < 67; j++)
        {
          n += snprintf(parts[i] + n, sizeof(parts[i]) - n, "0061");
        }

      assert_true(n < sizeof(parts[i]));
      t->slots[i] = parts[i];
    }

  t->sms        = true;
  t->sms_total  = 32;
  t->expect_deletes = 24;
  assert_int_equal(tel_test_run(t), 0);
  for (i = 0; i < 24; i++)
    {
      assert_null(t->slots[i]);
    }

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  tel_test_page(&store, "+48123456789", page);
  assert_int_equal(page->messages_count, 2);
  assert_int_equal(strlen(page->messages[1].text), 1530);
  assert_int_equal(strlen(page->messages[0].text), 24 * 67 - 1530);
  assert_int_equal(page->messages[0].text[0], 'a');

  store_free(&store);
  free(page);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_scan
 *
 * Description:
 *   Two SMS counted at start, in slots 0 and 5; the first part of a long
 *   one stored in slot 1 while the memory is read through: the scan still
 *   reaches slot 5, and the part waits for the rest.  Stored in slot 0,
 *   behind the scan, it counts all the same.
 *
 ****************************************************************************/

static void test_telephony_sms_scan(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->sms        = true;
  t->sms_total  = 10;
  t->slots[0]   = T_HELLO;
  t->slots[5]   = T_UCS2;
  t->cmti_after = 1;
  t->cmti_slot  = 1;
  t->cmti_pdu   = T_LONG1;
  t->expect_deletes = 2;
  assert_int_equal(tel_test_run(t), 0);

  assert_null(t->slots[0]);
  assert_null(t->slots[5]);
  assert_non_null(t->slots[1]);
  assert_null(strstr(t->commands, "AT+CMGD=1;"));
  tel_test_free(t);

  /* One stored behind the scan, in the slot just emptied: read from its
   * announcement, and counted, so the scan reads no further than it must
   */

  t = tel_test_new();
  t->sms        = true;
  t->sms_total  = 10;
  t->slots[0]   = T_HELLO;
  t->slots[5]   = T_UCS2;
  t->cmti_after = 1;
  t->cmti_slot  = 0;
  t->cmti_pdu   = T_LONG1;
  t->expect_deletes = 2;
  assert_int_equal(tel_test_run(t), 0);

  assert_non_null(t->slots[0]);
  assert_null(t->slots[5]);
  assert_null(strstr(t->commands, "AT+CMGR=6;"));

  tel_test_free(t);
}

/* A message queued in Messages' store before the run */

static void tel_test_queue(FAR struct tel_test_s *t, FAR const char *peer,
                           FAR const char *text)
{
  FAR struct store_write_s *w = calloc(1, sizeof(*w));
  FAR pnut_messages_message_record_t *m = calloc(1, sizeof(*m));
  struct store_s store;

  assert_non_null(w);
  assert_non_null(m);
  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  m->out    = true;
  m->status = PNUT_MESSAGES_MESSAGE_STATUS_QUEUED;
  strlcpy(m->text, text, sizeof(m->text));
  assert_int_equal(store_plan_message(&store, "sms", peer, m, w), 0);
  store_write_run(w);
  assert_int_equal(w->ret, 0);
  assert_int_equal(store_commit(&store, w), 0);
  store_free(&store);
  free(m);
  free(w);
}

/****************************************************************************
 * Name: test_telephony_sms_send
 *
 * Description:
 *   Messages' queue taken once the modem has the network: a message of
 *   one part and one of two sent, each part after the modem's prompt,
 *   and both reported sent.
 *
 ****************************************************************************/

static void test_telephony_sms_send(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR pnut_messages_page_t *page = calloc(1, sizeof(*page));
  char longer[201];
  struct store_s store;

  assert_non_null(page);
  memset(longer, 'a', 200);
  longer[200] = '\0';
  tel_test_queue(t, "+48123456789", "hello there");
  tel_test_queue(t, "+48123456789", longer);

  t->sms         = true;
  t->sms_total   = 10;
  t->expect_cmgs = 3;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->cmgs, 3);
  assert_non_null(strstr(t->commands, "AT+CMGS="));

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  tel_test_page(&store, "+48123456789", page);
  assert_int_equal(page->messages_count, 2);
  assert_int_equal(page->messages[0].status,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT);
  assert_int_equal(page->messages[1].status,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT);

  store_free(&store);
  free(page);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_send_fail
 *
 * Description:
 *   A part the network refuses: the message reported failed, with the
 *   modem's word for why.
 *
 ****************************************************************************/

static void test_telephony_sms_send_fail(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR pnut_messages_page_t *page = calloc(1, sizeof(*page));
  struct store_s store;

  assert_non_null(page);
  tel_test_queue(t, "+48123456789", "will fail");
  t->sms         = true;
  t->sms_total   = 10;
  t->cmgs_fail   = true;
  t->expect_cmgs = 1;
  assert_int_equal(tel_test_run(t), 0);

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  tel_test_page(&store, "+48123456789", page);
  assert_int_equal(page->messages[0].status,
                   PNUT_MESSAGES_MESSAGE_STATUS_FAILED);
  assert_string_equal(page->messages[0].reason, "+CMS ERROR: 500");

  store_free(&store);
  free(page);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_undeleted
 *
 * Description:
 *   A delete the modem refuses, for a status report and for a delivered
 *   SMS: each tried again when the memory is read through at the tick,
 *   the SMS stored once.
 *
 ****************************************************************************/

static void test_telephony_sms_undeleted(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  struct store_s store;

  t->sms        = true;
  t->sms_total  = 10;
  t->slots[0]   = T_STATUS;
  t->slots[1]   = T_HELLO;
  t->cmgd_fail  = 2;
  t->expect_deletes = 2;
  assert_int_equal(tel_test_run(t), 0);
  assert_null(t->slots[0]);
  assert_null(t->slots[1]);

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  assert_int_equal(store_unread(&store), 1);
  store_free(&store);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_sim
 *
 * Description:
 *   SMS left in the SIM's memory, a long one among them, each part twice:
 *   read from there first, delivered, the copies dropped, the SIM emptied;
 *   then the modem's own read as ever.
 *
 ****************************************************************************/

static void test_telephony_sms_sim(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();
  FAR pnut_messages_page_t *page = calloc(1, sizeof(*page));
  struct store_s store;
  FAR const char *sim;
  FAR const char *me;
  int i;

  assert_non_null(page);
  t->sms            = true;
  t->sms_total      = 10;
  t->slots[2]       = T_UCS2;
  t->sim_slots[1]   = T_LONG2;    /* As a SIM seen holds them: each twice */
  t->sim_slots[2]   = T_LONG2;
  t->sim_slots[3]   = T_LONG1;
  t->sim_slots[4]   = T_LONG1;
  t->sim_slots[5]   = T_HELLO;
  t->expect_deletes = 6;
  assert_int_equal(tel_test_run(t), 0);

  sim = strstr(t->commands, "AT+CPMS=\"SM\",\"SM\",\"ME\";");
  assert_non_null(sim);
  me = strstr(sim, "AT+CPMS=\"ME\",\"ME\",\"ME\";");
  assert_non_null(me);
  for (i = 0; i < 32; i++)
    {
      assert_null(t->sim_slots[i]);
    }

  assert_null(t->slots[2]);

  /* Two conversations: the long message once, the short one, the UCS2 */

  store_init(&store, t->store, 64 * 1024, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&store), 0);
  assert_int_equal(store.nconvs, 2);
  i = store.convs[0].nmsgs + store.convs[1].nmsgs;
  assert_int_equal(i, 3);
  store_free(&store);

  free(page);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_sms_full
 *
 * Description:
 *   Messages' store full of unread messages, which never go to make room:
 *   the first SMS goes in, the next is refused and stays in the modem,
 *   read and offered again at each tick, never deleted.
 *
 ****************************************************************************/

static void test_telephony_sms_full(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->sms        = true;
  t->sms_limit  = 1;
  t->sms_total  = 10;
  t->slots[0]   = T_HELLO;
  t->slots[1]   = T_UCS2;
  t->expect_deletes = 1;
  assert_int_equal(tel_test_run(t), 0);

  assert_null(t->slots[0]);
  assert_non_null(t->slots[1]);
  assert_int_equal(t->deletes, 1);
  assert_null(strstr(t->commands, "AT+CMGD=1;"));

  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data
 *
 * Description:
 *   Mobile data wanted: once the modem is registered, channel 2 answers
 *   "AT" and dials; the AT channel's commands wait until CONNECT; the link
 *   starts on a raw pseudo-terminal and comes up; a PPP frame goes each
 *   way, whole; data off stops the link and hangs the call up (the probe
 *   gave DTR that meaning, AT&D2); data on again dials again.
 *
 ****************************************************************************/

static void test_telephony_data(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->dials, 2);
  assert_int_equal(t->at_during_dial, 0);
  assert_true(t->at_after_dial > 0);
  assert_int_equal(t->link_starts, 2);
  assert_true(t->link_stops >= 1);
  assert_true(t->hangups >= 1);
  assert_true(t->hung_up);
  assert_int_equal(t->escapes, 0);
  assert_int_equal(t->fm_len, 11);
  assert_memory_equal(t->from_modem, "\x7e\xff\x03he\rl\nlo\x7e", 11);
  assert_int_equal(t->tm_len, 11);
  assert_memory_equal(t->to_modem, "\x7e\xff\x03wo\rr\nld\x7e", 11);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data_wifi
 *
 * Description:
 *   Mobile data up while Wi-Fi carries the traffic: kept when Wi-Fi goes
 *   within the hold; after it, standby, the call hung up, the setting
 *   still on; dialled again once Wi-Fi goes.
 *
 ****************************************************************************/

static void test_telephony_data_wifi(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data      = true;
  t->data_wifi = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->wifi_step, 5);
  assert_int_equal(t->dials, 2);
  assert_int_equal(t->link_starts, 2);
  assert_true(t->hangups >= 1);
  assert_true(t->tel.data.want);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data_escape
 *
 * Description:
 *   A call left in data mode on channel 2 (a modem that ignored DTR): the
 *   probes go unanswered, so "+++" takes the channel out of data mode and
 *   ATH&D2 hangs up; then it dials, and data comes up.
 *
 ****************************************************************************/

static void test_telephony_data_escape(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data           = true;
  t->data_left_on   = true;
  t->data_connected = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->escapes, 1);
  assert_true(t->d2);
  assert_int_equal(t->dials, 1);
  assert_int_equal(t->link_starts, 1);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data_apn
 *
 * Description:
 *   An access point's name: AT+CGDCONT on the AT channel before the dial.
 *   One with a character an operator's name has not: no command, no dial,
 *   a failure.
 *
 ****************************************************************************/

static void test_telephony_data_apn(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data      = true;
  t->data_once = true;
  t->apn       = "inter.net-1";
  assert_int_equal(tel_test_run(t), 0);
  assert_non_null(strstr(t->commands,
                         "AT+CGDCONT=1,\"IP\",\"inter.net-1\";"));
  assert_int_equal(t->dials, 1);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
  tel_test_free(t);

  t = tel_test_new();
  t->data = true;
  t->apn  = "bad\",\"x";
  assert_int_equal(tel_test_run(t), 0);
  assert_null(strstr(t->commands, "CGDCONT"));
  assert_int_equal(t->dials, 0);
  assert_int_equal(t->tel.data.fails, 1);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data_fail
 *
 * Description:
 *   A dial the network refuses (NO CARRIER): hung up, no link started,
 *   and "connecting" while it waits to try again.  The program then
 *   stops while data waits: the modem's power-down starts no attempt on
 *   the way (it once did, the state still saying "on" while data reset).
 *
 ****************************************************************************/

static void test_telephony_data_fail(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data      = true;
  t->data_fail = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->dials, 1);
  assert_int_equal(t->hangups, 1);
  assert_int_equal(t->link_starts, 0);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_CONNECTING);
  assert_int_equal(t->probes2, 1);
  assert_int_equal(t->tel.data.fails, 1);
  tel_test_free(t);
}

/****************************************************************************
 * Name: test_telephony_data_stop
 *
 * Description:
 *   The program asked to stop while data is up: the link is stopped, and
 *   the modem powered down as ever.
 *
 ****************************************************************************/

static void test_telephony_data_stop(void **state)
{
  FAR struct tel_test_s *t = tel_test_new();

  t->data         = true;
  t->data_stop_up = true;
  assert_int_equal(tel_test_run(t), 0);
  assert_int_equal(t->data_at_stop, PNUT_TELEPHONY_INFO_DATA_DATA_ON);
  assert_int_equal(t->link_starts, 1);
  assert_int_equal(t->link_stops, 1);
  assert_int_equal(t->hangups, 0);
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
    cmocka_unit_test(test_telephony_airplane_backstop),
    cmocka_unit_test(test_telephony_clock_before),
    cmocka_unit_test(test_telephony_clock_unset),
    cmocka_unit_test(test_telephony_slow),
    cmocka_unit_test(test_telephony_no_channels),
    cmocka_unit_test(test_telephony_sms),
    cmocka_unit_test(test_telephony_sms_long),
    cmocka_unit_test(test_telephony_sms_sim),
    cmocka_unit_test(test_telephony_sms_full),
    cmocka_unit_test(test_telephony_sms_scan),
    cmocka_unit_test(test_telephony_sms_undeleted),
    cmocka_unit_test(test_telephony_sms_send),
    cmocka_unit_test(test_telephony_sms_send_fail),
    cmocka_unit_test(test_telephony_data),
    cmocka_unit_test(test_telephony_data_wifi),
    cmocka_unit_test(test_telephony_data_escape),
    cmocka_unit_test(test_telephony_data_apn),
    cmocka_unit_test(test_telephony_data_fail),
    cmocka_unit_test(test_telephony_data_stop),
  };

  return cmocka_run_group_tests_name("telephony", tests, NULL, NULL);
}
