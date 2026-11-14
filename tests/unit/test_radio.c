/****************************************************************************
 * pnut-os/tests/unit/test_radio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Radio (RFC 0034) against a fake LoRa radio, with Settings in the same
 * loop: one caller at a time, the packets heard as a stream (a slow
 * reader losing the oldest), every send checked against EU868, airplane
 * mode, and the antenna setting.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/settings.pnut.h"

#include "packetradio.h"
#include "settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_SERVICE    "radio"
#define FAKE_QUEUE       16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rad_s;
typedef CODE bool (*rad_step_t)(FAR struct rad_s *t);

struct rad_s
{
  FAR struct pnut_loop_s *loop;
  rad_step_t steps;
  int last;
  struct settings_s settings;
  struct radio_s radio;
  struct radio_part_s part;
  struct pnut_module_s module;
  struct pnut_radio_client_s client;
  struct pnut_radio_client_s other;
  struct pnut_settings_client_s set;
  struct pnut_radio_info_reader_s reader;
  FAR struct pnut_publisher_s *airplane;
  char dir[64];
  char sdir[80];
  int step;
  int ticks;
  bool waiting;
  int status;                     /* The last answer's */
  uint32_t code;
  uint32_t air_us;
  pnut_radio_info_t state;        /* The topic's last */

  /* The caller's stream */

  uint16_t stream;
  bool streaming;
  int ended;                      /* Its end's status, -1 while open */
  uint8_t got[32];                /* Each packet's first byte */
  int ngot;

  /* A send answered while the test goes on */

  bool bg_waiting;
  int bg_status;
  uint32_t bg_code;

  /* The fake radio.  open, setup and send run on the worker: they count
   * what goes wrong in bad rather than assert there.
   */

  int pipe[2];
  volatile int bad;
  volatile bool open_fails;
  volatile uint32_t setup_fails_hz;
  volatile bool block;            /* A send waits for gate */
  volatile bool in_send;
  sem_t gate;
  bool opened;
  int opens;
  int setups;
  struct radio_lora_s lora;       /* Its last setup's */
  uint8_t listen;
  uint32_t listen_us;
  uint32_t sleep_us;
  uint8_t queue[FAKE_QUEUE];
  int qhead;
  int qcount;
  pthread_mutex_t lock;
  int sends;
  uint8_t sent[RADIO_PAYLOAD];
  size_t sent_len;
  volatile int antennas;
  volatile bool internal;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint64_t g_now = 1000 * 60000;  /* Minute 1000 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t fake_now(void)
{
  return g_now;
}

/* The fake radio: packets heard are bytes queued, the pipe readable while
 * any waits
 */

static int fake_open(FAR void *arg, FAR const char *path)
{
  FAR struct rad_s *t = arg;

  if (strcmp(path, "/dev/fake_lora") != 0 || t->opened)
    {
      t->bad++;
    }

  if (t->open_fails)
    {
      return -EIO;
    }

  t->opened = true;
  t->opens++;
  return t->pipe[0];
}

static void fake_close(FAR void *arg)
{
  FAR struct rad_s *t = arg;
  char c;

  if (!t->opened)
    {
      t->bad++;
    }

  t->opened = false;
  t->qcount = 0;
  while (read(t->pipe[0], &c, 1) == 1);
}

static int fake_setup(FAR void *arg, FAR const struct radio_lora_s *lora,
                      uint8_t listen, uint32_t listen_us, uint32_t sleep_us)
{
  FAR struct rad_s *t = arg;

  if (!t->opened)
    {
      t->bad++;
    }

  t->setups++;
  if (lora->freq_hz == t->setup_fails_hz)
    {
      return -EINVAL;
    }

  t->lora      = *lora;
  t->listen    = listen;
  t->listen_us = listen_us;
  t->sleep_us  = sleep_us;
  return OK;
}

static int fake_receive(FAR void *arg, FAR struct radio_heard_s *heard)
{
  FAR struct rad_s *t = arg;
  char c;

  if (t->qcount == 0)
    {
      return 0;
    }

  assert_int_equal(read(t->pipe[0], &c, 1), 1);
  memset(heard, 0, sizeof(*heard));
  heard->len      = 3;
  heard->data[0]  = t->queue[t->qhead];
  heard->data[1]  = 0xaa;
  heard->data[2]  = 0xbb;
  heard->rssi_dbm = -90;
  heard->snr_cdb  = 725;
  t->qhead = (t->qhead + 1) % FAKE_QUEUE;
  t->qcount--;
  return 1;
}

static int fake_send(FAR void *arg, FAR const uint8_t *data, size_t len)
{
  FAR struct rad_s *t = arg;

  if (t->block)
    {
      t->in_send = true;
      while (sem_wait(&t->gate) < 0);
      t->in_send = false;
    }

  pthread_mutex_lock(&t->lock);
  memcpy(t->sent, data, len);
  t->sent_len = len;
  t->sends++;
  pthread_mutex_unlock(&t->lock);
  return OK;
}

static int fake_antenna(FAR void *arg, FAR const char *line, bool internal)
{
  FAR struct rad_s *t = arg;

  if (strcmp(line, "/dev/fake_ant") != 0)
    {
      t->bad++;
    }

  t->internal = internal;
  t->antennas++;
  return OK;
}

static const struct radio_dev_ops_s g_fake =
{
  fake_open,
  fake_close,
  fake_setup,
  fake_receive,
  fake_send,
  fake_antenna
};

/* A packet heard by the fake, its first byte n */

static void fake_hear(FAR struct rad_s *t, uint8_t n)
{
  assert_true(t->qcount < FAKE_QUEUE);
  t->queue[(t->qhead + t->qcount) % FAKE_QUEUE] = n;
  t->qcount++;
  assert_int_equal(write(t->pipe[1], "x", 1), 1);
}

/* Calls to Radio */

static void rad_lora(FAR pnut_radio_lora_t *lora, uint32_t hz,
                     uint32_t bw, uint32_t sf, int32_t dbm)
{
  memset(lora, 0, sizeof(*lora));
  lora->frequency_hz     = hz;
  lora->bandwidth_hz     = bw;
  lora->spreading_factor = sf;
  lora->coding_rate      = 5;
  lora->sync_word        = 0x12;
  lora->preamble         = 16;
  lora->power_dbm        = dbm;
  lora->crc              = true;
}

static void rad_opened(FAR struct pnut_radio_client_s *client, int status,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->waiting = false;
  if (status != PNUT_STATUS_OK && client == &t->client)
    {
      t->streaming = false;
    }
}

static void rad_item(FAR struct pnut_radio_client_s *client, int status,
                     FAR const pnut_radio_packet_t *item, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  if (item == NULL)
    {
      t->ended     = status;
      t->streaming = false;
      return;
    }

  assert_int_equal(item->data.size, 3);
  assert_int_equal(item->data.bytes[1], 0xaa);
  assert_int_equal(item->rssi_dbm, -90);
  assert_int_equal(item->snr_cdb, 725);
  assert_int_equal(item->ended_ms, g_now);
  assert_true(t->ngot < (int)sizeof(t->got));
  t->got[t->ngot++] = item->data.bytes[0];
}

static void rad_open(FAR struct rad_s *t,
                     FAR struct pnut_radio_client_s *client,
                     FAR const pnut_radio_lora_t *lora, uint16_t credit)
{
  pnut_radio_open_request_t req;
  uint16_t stream;

  memset(&req, 0, sizeof(req));
  strlcpy(req.radio, "lora", sizeof(req.radio));
  req.has_lora = true;
  req.lora     = *lora;
  t->waiting   = true;
  assert_int_equal(pnut_radio_open(client, &req, credit, 0, rad_opened,
                                   rad_item, t, &stream), 0);
  if (client == &t->client)
    {
      t->stream    = stream;
      t->streaming = true;
      t->ended     = -1;
    }
}

static void rad_sent(FAR struct pnut_radio_client_s *client, int status,
                     FAR const pnut_radio_send_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->air_us  = out != NULL ? out->air_us : 0;
  t->waiting = false;
}

static void rad_send(FAR struct rad_s *t,
                     FAR struct pnut_radio_client_s *client, size_t len)
{
  pnut_radio_send_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.radio, "lora", sizeof(req.radio));
  memset(req.data.bytes, 0x5a, len);
  req.data.size = len;
  t->waiting    = true;
  assert_int_equal(pnut_radio_send(client, &req, 0, rad_sent, t), 0);
}

/* A send answered later, the test going on meanwhile */

static void rad_sent_bg(FAR struct pnut_radio_client_s *client, int status,
                        FAR const pnut_radio_send_reply_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->bg_status  = status;
  t->bg_code    = err != NULL ? err->code : 0;
  t->bg_waiting = false;
}

static void rad_send_bg(FAR struct rad_s *t,
                        FAR struct pnut_radio_client_s *client, size_t len)
{
  pnut_radio_send_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.radio, "lora", sizeof(req.radio));
  memset(req.data.bytes, 0x3c, len);
  req.data.size = len;
  t->bg_waiting = true;
  assert_int_equal(pnut_radio_send(client, &req, 30000, rad_sent_bg, t),
                   0);
}

static void rad_configured(FAR struct pnut_radio_client_s *client,
                           int status, FAR const pnut_radio_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->waiting = false;
}

static void rad_configure_listen(FAR struct rad_s *t,
                                 FAR struct pnut_radio_client_s *client,
                                 FAR const pnut_radio_lora_t *lora,
                                 pnut_radio_listen_t listen,
                                 uint32_t listen_us, uint32_t sleep_us)
{
  pnut_radio_configure_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.radio, "lora", sizeof(req.radio));
  req.has_lora  = true;
  req.lora      = *lora;
  req.listen    = listen;
  req.listen_us = listen_us;
  req.sleep_us  = sleep_us;
  t->waiting    = true;
  assert_int_equal(pnut_radio_configure(client, &req, 0, rad_configured,
                                        t), 0);
}

static void rad_configure(FAR struct rad_s *t,
                          FAR struct pnut_radio_client_s *client,
                          FAR const pnut_radio_lora_t *lora)
{
  rad_configure_listen(t, client, lora,
                       PNUT_RADIO_LISTEN_RADIO_LISTEN_ALWAYS, 0, 0);
}

static void rad_set_done(FAR struct pnut_settings_client_s *client,
                         int status, FAR const pnut_settings_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  t->waiting = false;
}

static void rad_set(FAR struct rad_s *t, FAR const char *key,
                    FAR const char *text)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_RADIO_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, key, sizeof(req.key));
  req.has_value              = true;
  req.value.which_value      = PNUT_SETTING_VALUE_TEXT_TAG;
  strlcpy(req.value.value.text, text, sizeof(req.value.value.text));
  t->waiting = true;
  assert_int_equal(pnut_settings_set(&t->set, &req, 0, rad_set_done, t),
                   0);
}

static void rad_airplane(FAR struct rad_s *t, bool on)
{
  pnut_airplane_mode_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.on = on;
  assert_int_equal(pnut_airplane_mode_publish(t->airplane, &msg), 0);
}

/****************************************************************************
 * Name: rad_listen
 *
 * Description:
 *   Opened, the radio is set up as asked and listens; the packets heard
 *   come in order; the topic says who has it; the stream ended, the radio
 *   is closed and free.
 *
 ****************************************************************************/

static bool rad_listen(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_int_equal(t->state.radios_count, 1);
        assert_false(t->state.radios[0].open);
        assert_string_equal(t->state.region, "EU868");
        rad_lora(&lora, 869525000, 250000, 11, 22);
        rad_open(t, &t->client, &lora, 8);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (!t->state.radios[0].open)
          {
            return false;
          }

        assert_int_equal(t->state.radios[0].pid, getpid());
        assert_true(t->opened);
        assert_int_equal(t->lora.freq_hz, 869525000);
        assert_int_equal(t->lora.bw_hz, 250000);
        assert_int_equal(t->lora.bw_code, 0x05);
        assert_int_equal(t->lora.sf, 11);
        assert_false(t->lora.ldro);
        assert_int_equal(t->listen, PNUT_RADIO_LISTEN_RADIO_LISTEN_ALWAYS);
        fake_hear(t, 1);
        fake_hear(t, 2);
        break;

      case 2:
        if (t->ngot < 2)
          {
            return false;
          }

        assert_int_equal(t->got[0], 1);
        assert_int_equal(t->got[1], 2);
        assert_int_equal(pnut_radio_open_cancel(&t->client, t->stream), 0);
        break;

      case 3:                     /* Given back */
        if (t->opened || t->state.radios[0].open)
          {
            return false;
          }

        assert_int_equal(t->opens, 1);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_one_caller
 *
 * Description:
 *   While one caller has the radio, another is told it is busy, and may
 *   neither send nor configure; a radio of another name does not exist.
 *   The first gone, the other may open it.
 *
 ****************************************************************************/

static bool rad_one_caller(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;
  pnut_radio_open_request_t req;
  uint16_t stream;

  if (t->waiting)
    {
      return false;
    }

  rad_lora(&lora, 869525000, 250000, 11, 22);
  switch (t->step)
    {
      case 0:
        rad_open(t, &t->client, &lora, 4);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_open(t, &t->other, &lora, 4);
        break;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_BUSY);
        rad_send(t, &t->other, 10);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_NOT_OPEN);
        rad_configure(t, &t->other, &lora);
        break;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_NOT_OPEN);
        assert_int_equal(t->sends, 0);

        memset(&req, 0, sizeof(req));
        strlcpy(req.radio, "uwb", sizeof(req.radio));
        req.has_lora = true;
        req.lora     = lora;
        t->waiting   = true;
        assert_int_equal(pnut_radio_open(&t->other, &req, 1, 0, rad_opened,
                                         rad_item, t, &stream), 0);
        break;

      case 5:
        assert_int_equal(t->status, PNUT_STATUS_NOTFOUND);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_NO_RADIO);
        assert_int_equal(pnut_radio_open_cancel(&t->client, t->stream), 0);
        break;

      case 6:
        if (t->state.radios[0].open)
          {
            return false;
          }

        rad_open(t, &t->other, &lora, 4);
        break;

      case 7:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->opened);
        assert_int_equal(t->opens, 2);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_rules
 *
 * Description:
 *   At 869.525 MHz the chip's 22 dBm goes out, with its air time counted.
 *   At 868.1 MHz (25 mW) 22 dBm is refused, 14 dBm goes; 255 bytes at
 *   SF12 take 9.28 s, so the sub-band's 36 s an hour take three, not a
 *   fourth (16 symbols of preamble).  A channel across two sub-bands is
 *   refused; one outside the radio's band is not taken.  A duty cycle
 *   takes up to 262 s a turn.  The longest preamble at SF12 and 7.8 kHz
 *   is too long to count: refused, not counted as nothing.
 *
 ****************************************************************************/

static bool rad_rules(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        rad_lora(&lora, 869525000, 250000, 11, 22);
        rad_open(t, &t->client, &lora, 4);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_send(t, &t->client, 50);
        break;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->sends, 1);
        assert_int_equal(t->sent_len, 50);
        assert_int_equal(t->air_us, 641024);
        rad_lora(&lora, 868100000, 125000, 12, 22);
        rad_configure(t, &t->client, &lora);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->lora.ldro);
        rad_send(t, &t->client, 255);
        break;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_POWER);
        rad_lora(&lora, 868100000, 125000, 12, 14);
        rad_configure(t, &t->client, &lora);
        break;

      case 5:
      case 6:
      case 7:
        if (t->step > 5)
          {
            assert_int_equal(t->status, PNUT_STATUS_OK);
            assert_int_equal(t->air_us, 9281536);
          }

        rad_send(t, &t->client, 255);
        break;

      case 8:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_send(t, &t->client, 255);
        break;

      case 9:                     /* The fourth: over the hour's 36 s */
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_AIR_TIME);
        assert_int_equal(t->sends, 4);
        if (t->state.radios[0].air_ms < 3 * 9282)
          {
            return false;
          }

        rad_lora(&lora, 865000000, 250000, 9, 14);
        rad_configure(t, &t->client, &lora);
        break;

      case 10:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_send(t, &t->client, 10);
        break;

      case 11:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_BAND);
        rad_lora(&lora, 862990000, 125000, 9, 14);
        rad_configure(t, &t->client, &lora);
        break;

      case 12:                    /* A duty cycle: a sleep too long */
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_BAND);
        rad_lora(&lora, 868100000, 125000, 9, 14);
        rad_configure_listen(t, &t->client, &lora,
                             PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE,
                             10000, RADIO_DUTY_MAX + 1);
        break;

      case 13:                    /* The longest listen */
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_CONFIG);
        rad_lora(&lora, 868100000, 125000, 9, 14);
        rad_configure_listen(t, &t->client, &lora,
                             PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE,
                             RADIO_DUTY_MAX, 100000);
        break;

      case 14:                    /* Air time too long to count */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->listen,
                         PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE);
        assert_int_equal(t->listen_us, RADIO_DUTY_MAX);
        assert_int_equal(t->sleep_us, 100000);
        rad_lora(&lora, 868100000, 7812, 12, 14);
        lora.preamble = 65535;
        rad_configure(t, &t->client, &lora);
        break;

      case 15:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->lora.preamble, 65535);
        rad_send(t, &t->client, 10);
        break;

      case 16:
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_AIR_TIME);
        assert_int_equal(t->sends, 4);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_slow_reader
 *
 * Description:
 *   A caller with no credit: of 12 packets heard at once, Radio keeps the
 *   8 newest, gives the first of them for the one credit it had, and the
 *   rest once credit comes.
 *
 ****************************************************************************/

static bool rad_slow_reader(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;
  int i;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        rad_lora(&lora, 869525000, 250000, 11, 22);
        rad_open(t, &t->client, &lora, 1);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        for (i = 1; i <= 12; i++)
          {
            fake_hear(t, i);
          }

        break;

      case 2:
        if (t->qcount > 0 || t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->ngot, 1);
        assert_int_equal(t->got[0], 5);
        assert_int_equal(pnut_radio_open_credit(&t->client, t->stream, 20),
                         0);
        break;

      case 3:
        if (t->ngot < 8 || t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->ngot, 8);
        for (i = 0; i < 8; i++)
          {
            assert_int_equal(t->got[i], 5 + i);
          }

        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_airplane_mode
 *
 * Description:
 *   Airplane mode closes the radio, its caller keeping it; a send is
 *   refused; off again, the radio opens as it was set up.
 *
 ****************************************************************************/

static bool rad_airplane_mode(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        rad_lora(&lora, 869525000, 250000, 11, 22);
        rad_open(t, &t->client, &lora, 4);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->opened);
        rad_airplane(t, true);
        break;

      case 2:
        if (t->opened || !t->state.airplane)
          {
            return false;
          }

        assert_true(t->state.radios[0].open);
        rad_send(t, &t->client, 10);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_AIRPLANE);
        rad_airplane(t, false);
        break;

      case 4:
        if (!t->opened)
          {
            return false;
          }

        assert_int_equal(t->opens, 2);
        assert_int_equal(t->lora.freq_hz, 869525000);
        assert_true(t->streaming);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_antenna
 *
 * Description:
 *   The internal antenna at start; the external chosen, its line switched
 *   and its 3 dBi counted: 14 dBm at 868.1 MHz is then over 25 mW, 13 dBm
 *   is not.
 *
 ****************************************************************************/

static bool rad_antenna(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_true(t->antennas >= 1);
        assert_true(t->internal);
        rad_set(t, "lora.antenna", "external");
        break;

      case 1:
        if (t->internal)
          {
            return false;
          }

        rad_lora(&lora, 868100000, 125000, 7, 14);
        rad_open(t, &t->client, &lora, 4);
        break;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_send(t, &t->client, 10);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_DENIED);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_POWER);
        rad_lora(&lora, 868100000, 125000, 7, 13);
        rad_configure(t, &t->client, &lora);
        break;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        rad_send(t, &t->client, 10);
        break;

      case 5:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->air_us, 49408);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_mid_send
 *
 * Description:
 *   While a send is on the air nothing else reaches the radio: a Configure
 *   and another caller's Open are told it is busy.  Its caller gone, the
 *   radio stays open until the send ends, then closes; the next caller's
 *   Open sets the radio up with its own configuration.  Airplane mode
 *   during a send closes the radio once it ends, and its end opens it
 *   again.
 *
 ****************************************************************************/

static bool rad_mid_send(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  rad_lora(&lora, 868100000, 125000, 7, 14);
  switch (t->step)
    {
      case 0:
        rad_lora(&lora, 869525000, 250000, 11, 22);
        rad_open(t, &t->client, &lora, 4);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        t->block = true;
        rad_send_bg(t, &t->client, 50);
        break;

      case 2:                     /* On the air */
        if (!t->in_send)
          {
            return false;
          }

        rad_configure(t, &t->client, &lora);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_SENDING);
        rad_open(t, &t->other, &lora, 4);
        break;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_BUSY);
        assert_int_equal(pnut_radio_open_cancel(&t->client, t->stream), 0);
        break;

      case 5:                     /* Given back, still sending */
        if (t->state.radios[0].open)
          {
            return false;
          }

        assert_true(t->opened);
        rad_open(t, &t->other, &lora, 4);
        break;

      case 6:
        assert_int_equal(t->status, PNUT_STATUS_BUSY);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_SENDING);
        t->block = false;
        sem_post(&t->gate);
        break;

      case 7:                     /* Sent, then closed */
        if (t->opened || t->bg_waiting)
          {
            return false;
          }

        assert_int_equal(t->bg_status, PNUT_STATUS_OK);
        assert_int_equal(t->opens, 1);
        rad_open(t, &t->other, &lora, 4);
        break;

      case 8:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_true(t->opened);
        assert_int_equal(t->opens, 2);
        assert_int_equal(t->lora.freq_hz, 868100000);
        assert_int_equal(t->lora.power_dbm, 14);
        t->block = true;
        rad_send_bg(t, &t->other, 10);
        break;

      case 9:
        if (!t->in_send)
          {
            return false;
          }

        rad_airplane(t, true);
        break;

      case 10:                    /* Open until the send ends */
        if (!t->state.airplane || t->ticks < 2)
          {
            return false;
          }

        assert_true(t->opened);
        t->block = false;
        sem_post(&t->gate);
        break;

      case 11:
        if (t->opened || t->bg_waiting)
          {
            return false;
          }

        assert_int_equal(t->bg_status, PNUT_STATUS_OK);
        rad_airplane(t, false);
        break;

      case 12:
        if (!t->opened)
          {
            return false;
          }

        assert_int_equal(t->opens, 3);
        assert_int_equal(t->lora.freq_hz, 868100000);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_failures
 *
 * Description:
 *   A radio that does not open fails the Open, and nobody holds it.  A
 *   configuration the radio does not take fails the Configure with the
 *   one before set up again, and the radio still sends.
 *
 ****************************************************************************/

static bool rad_failures(FAR struct rad_s *t)
{
  pnut_radio_lora_t lora;

  if (t->waiting)
    {
      return false;
    }

  rad_lora(&lora, 869525000, 250000, 11, 22);
  switch (t->step)
    {
      case 0:
        t->open_fails = true;
        rad_open(t, &t->client, &lora, 4);
        break;

      case 1:
        assert_int_equal(t->status, PNUT_STATUS_INTERNAL);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_FAILED);
        assert_false(t->state.radios[0].open);
        assert_false(t->opened);
        t->open_fails = false;
        rad_open(t, &t->client, &lora, 4);
        break;

      case 2:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->setups, 1);
        t->setup_fails_hz = 868300000;
        rad_lora(&lora, 868300000, 125000, 7, 14);
        rad_configure(t, &t->client, &lora);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_INVALID);
        assert_int_equal(t->code, PNUT_RADIO_ERROR_CODE_CONFIG);
        assert_int_equal(t->setups, 3);
        assert_int_equal(t->lora.freq_hz, 869525000);
        assert_true(t->state.radios[0].open);
        rad_send(t, &t->client, 50);
        break;

      case 4:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->air_us, 641024);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: rad_run
 ****************************************************************************/

static void rad_tick(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->ticks++;
  if (t->step > t->last || !t->steps(t))
    {
      return;
    }

  t->ticks = 0;
  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void rad_state(FAR struct pnut_radio_info_reader_s *reader,
                      FAR const pnut_radio_info_t *msg, FAR void *arg)
{
  FAR struct rad_s *t = arg;

  t->state = *msg;
}

static int rad_start(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct rad_s *t = (FAR struct rad_s *)
    ((FAR uint8_t *)module - offsetof(struct rad_s, module));

  assert_int_equal(pnut_radio_info_subscribe(loop, rad_state, t,
                                             &t->reader), 0);
  assert_int_equal(pnut_airplane_mode_advertise(loop, &t->airplane), 0);
  assert_int_equal(pnut_radio_connect(loop, RADIO_SERVICE, NULL, NULL,
                                      &t->client), 0);
  assert_int_equal(pnut_radio_connect(loop, RADIO_SERVICE, NULL, NULL,
                                      &t->other), 0);
  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                                         &t->set), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, rad_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void rad_stop_module(FAR struct pnut_module_s *module,
                            FAR struct pnut_loop_s *loop)
{
  FAR struct rad_s *t = (FAR struct rad_s *)
    ((FAR uint8_t *)module - offsetof(struct rad_s, module));

  pnut_radio_info_unsubscribe(&t->reader);
  pnut_topic_unadvertise(t->airplane);
  pnut_radio_disconnect(&t->client);
  pnut_radio_disconnect(&t->other);
  pnut_settings_disconnect(&t->set);
}

static void rad_timeout(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/* Radio's settings are the radios program's */

static int rad_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  *programp = "radios";
  return OK;
}

static bool rad_runs(FAR void *arg, FAR const char *program,
                     FAR const char *service)
{
  return strcmp(program, "radios") == 0;
}

static void rad_run(rad_step_t steps, int last)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 16,
    .texts    = 8,
    .choices  = 16,
    .secrets  = 2,
  };

  FAR struct rad_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct radio_config_s rconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  t->steps = steps;
  t->last  = last;
  pthread_mutex_init(&t->lock, NULL);
  assert_int_equal(sem_init(&t->gate, 0, 0), 0);
  assert_int_equal(pipe(t->pipe), 0);
  assert_int_equal(fcntl(t->pipe[0], F_SETFL, O_NONBLOCK), 0);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-radio-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  assert_int_equal(mkdir(t->sdir, 0700), 0);

  strlcpy(t->part.name, "lora", sizeof(t->part.name));
  strlcpy(t->part.device, "/dev/fake_lora", sizeof(t->part.device));
  strlcpy(t->part.antenna, "/dev/fake_ant", sizeof(t->part.antenna));
  t->part.min_hz        = 863000000;
  t->part.max_hz        = 870000000;
  t->part.min_dbm       = -9;
  t->part.max_dbm       = 22;
  t->part.internal_cdbi = 0;
  t->part.external_cdbi = 300;

  pnut_loop_defaults(&config);
  config.rundir  = t->dir;
  config.workers = 1;
  config.jobs    = 2;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  sconfig.who    = rad_who;
  sconfig.runs   = rad_runs;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&rconfig, 0, sizeof(rconfig));
  rconfig.ops  = &g_fake;
  rconfig.arg  = t;
  rconfig.part = &t->part;
  rconfig.now  = fake_now;
  radio_init(&t->radio, &rconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->radio.module), 0);

  t->module.name  = "test";
  t->module.start = rad_start;
  t->module.stop  = rad_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 10000, 0, rad_timeout, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  radio_deinit(&t->radio);
  settings_deinit(&t->settings);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);
  assert_false(t->opened);
  assert_int_equal(t->bad, 0);
  sem_destroy(&t->gate);
  close(t->pipe[0]);
  close(t->pipe[1]);
  pthread_mutex_destroy(&t->lock);
  assert_int_equal(system(cmd), 0);
  free(t);
}

static void test_radio_listen(void **state)
{
  rad_run(rad_listen, 3);
}

static void test_radio_one_caller(void **state)
{
  rad_run(rad_one_caller, 7);
}

static void test_radio_rules(void **state)
{
  rad_run(rad_rules, 16);
}

static void test_radio_slow_reader(void **state)
{
  rad_run(rad_slow_reader, 3);
}

static void test_radio_airplane(void **state)
{
  rad_run(rad_airplane_mode, 4);
}

static void test_radio_antenna(void **state)
{
  rad_run(rad_antenna, 5);
}

static void test_radio_mid_send(void **state)
{
  rad_run(rad_mid_send, 12);
}

static void test_radio_failures(void **state)
{
  rad_run(rad_failures, 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_radio_listen),
    cmocka_unit_test(test_radio_one_caller),
    cmocka_unit_test(test_radio_rules),
    cmocka_unit_test(test_radio_slow_reader),
    cmocka_unit_test(test_radio_airplane),
    cmocka_unit_test(test_radio_antenna),
    cmocka_unit_test(test_radio_mid_send),
    cmocka_unit_test(test_radio_failures),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
