/****************************************************************************
 * pnut-os/src/radios/packetradio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_RADIOS_PACKETRADIO_H
#define __PNUT_OS_RADIOS_PACKETRADIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/board.pnut.h"
#include "pnut/board.topics.h"
#include "pnut/radio.pnut.h"
#include "pnut/radio.settings.h"
#include "pnut/radio.topics.h"

#include "rules.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_NAME       16       /* A radio's name, its NUL included */
#define RADIO_PATH       24       /* A device file's, likewise */
#define RADIO_PAYLOAD    255      /* A packet, at most */
#define RADIO_HEARD      8        /* Packets heard kept for the caller */

/* A duty cycle's listen or sleep, at most: 24 bits of 15.625 us */

#define RADIO_DUTY_MAX   262143984

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A job on the worker (struct radio_s) */

enum radio_job_e
{
  RADIO_JOB_NONE = 0,
  RADIO_JOB_OPEN,                 /* Open the device and set it up */
  RADIO_JOB_SETUP,                /* Set it up anew */
  RADIO_JOB_SEND,
  RADIO_JOB_ANTENNA               /* Switch the antenna line */
};

/* A packet heard */

struct radio_heard_s
{
  uint64_t ended_ms;              /* Taken from the radio, monotonic */
  int16_t rssi_dbm;
  int16_t snr_cdb;
  bool crc_error;
  uint8_t len;
  uint8_t data[RADIO_PAYLOAD];
};

/* How the radio listens: pnut_radio_listen_t's values */

/* The radio: NuttX's SX126x driver on the device, a fake in the tests.
 * Each returns OK or a negated errno value.
 *
 *   open    - The radio's device: a descriptor that is readable while
 *             packets heard wait, or a negated errno value.
 *   setup   - Configure it and have it listen, between packets.
 *   receive - A packet heard: 1, or 0 when none waits.
 *   send    - A packet, returning once it is on the air.
 *
 * open, setup, send and antenna run on a worker, one at a time (and close
 * there after an open whose setup failed); meanwhile the loop calls
 * receive only.  close runs on the loop otherwise.
 *   antenna - The antenna line: on chooses the internal antenna.  It
 *             takes milliseconds over an I/O expander's bus.
 */

struct radio_dev_ops_s
{
  CODE int (*open)(FAR void *arg, FAR const char *path);
  CODE void (*close)(FAR void *arg);
  CODE int (*setup)(FAR void *arg, FAR const struct radio_lora_s *lora,
                    uint8_t listen, uint32_t listen_us, uint32_t sleep_us);
  CODE int (*receive)(FAR void *arg, FAR struct radio_heard_s *heard);
  CODE int (*send)(FAR void *arg, FAR const uint8_t *data, size_t len);
  CODE int (*antenna)(FAR void *arg, FAR const char *line, bool internal);
};

/* A radio, as Board's description gives it */

struct radio_part_s
{
  char name[RADIO_NAME];
  char device[RADIO_PATH];
  char antenna[RADIO_PATH];       /* Its antenna line, "" for none */
  uint32_t min_hz;
  uint32_t max_hz;
  int8_t min_dbm;
  int8_t max_dbm;
  int16_t internal_cdbi;
  int16_t external_cdbi;
};

struct radio_config_s
{
  FAR const struct radio_dev_ops_s *ops;
  FAR void *arg;

  /* Board's service, asked for the LoRa radio; or the radio as given,
   * Board not asked (part non-NULL)
   */

  FAR const char *board;
  FAR const struct radio_part_s *part;

  /* The monotonic clock in ms: NULL for the system's (the tests' own) */

  CODE uint64_t (*now)(void);
};

struct radio_s
{
  struct pnut_module_s module;    /* First */
  struct radio_config_s config;

  struct pnut_radio_server_s server;
  bool serving;
  struct pnut_radio_settings_s settings;
  bool settings_open;
  FAR struct pnut_publisher_s *topic;
  struct pnut_airplane_mode_reader_s airplane_reader;
  bool airplane_open;
  struct pnut_board_client_s board;
  bool board_open;

  /* The radio, once known; the band plan in use */

  bool present;
  struct radio_part_s part;
  FAR const struct radio_plan_s *plan;
  struct radio_air_s air[RADIO_BANDS_MAX];
  bool airplane;

  /* Its caller, while open: its stream, configuration and listening */

  bool held;
  struct pnut_stream_s stream;
  pid_t pid;
  uint16_t app;
  struct radio_lora_s lora;
  uint8_t listen;
  uint32_t listen_us;
  uint32_t sleep_us;

  /* The device, open while held and off airplane mode */

  bool dev_open;
  int fd;

  /* Packets heard, waiting for the caller's credit; a timer pushing them
   * again when the connection had no room
   */

  struct radio_heard_s heard[RADIO_HEARD];
  uint8_t heard_head;
  uint8_t heard_count;
  uint32_t dropped;
  FAR struct pnut_timer_s *retry;

  /* A job on the worker, one at a time (enum radio_job_e): opening the
   * device, setting it up anew, or a send.  Meanwhile the loop only reads
   * the device, and an Open waiting for it (opening) or a Configure or a
   * Send keeps its request to answer.  The job reads and writes only the
   * job_ fields.
   */

  uint8_t job;
  bool opening;
  bool antenna_due;               /* The antenna line to switch */
  bool antenna_internal;
  struct pnut_request_s job_req;
  struct radio_lora_s job_lora;
  struct radio_lora_s job_old;    /* A setup's fallback: the one before */
  uint8_t job_listen;
  uint32_t job_listen_us;
  uint32_t job_sleep_us;
  uint8_t job_old_listen;
  uint32_t job_old_listen_us;
  uint32_t job_old_sleep_us;
  uint8_t job_data[RADIO_PAYLOAD];
  size_t job_len;
  uint32_t job_air_us;
  bool job_internal;              /* An antenna switch's */
  int job_fd;                     /* An open's descriptor, -1 for none */
  int job_ret;
  int job_old_ret;                /* The fallback's */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's SX126x driver (device.c) */

extern const struct radio_dev_ops_s g_radio_sx126x;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: radio_init
 *
 * Description:
 *   Set Radio up, as a module to add to a loop.
 *
 ****************************************************************************/

void radio_init(FAR struct radio_s *radio,
                FAR const struct radio_config_s *config);

/****************************************************************************
 * Name: radio_deinit
 *
 * Description:
 *   After the loop is destroyed, its workers stopped: close the device if
 *   a send kept it open.
 *
 ****************************************************************************/

void radio_deinit(FAR struct radio_s *radio);

#endif /* __PNUT_OS_RADIOS_PACKETRADIO_H */
