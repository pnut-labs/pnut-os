/****************************************************************************
 * pnut-os/src/sensors/location.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SENSORS_LOCATION_H
#define __PNUT_OS_SENSORS_LOCATION_H

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
#include "pnut/location.pnut.h"
#include "pnut/location.topics.h"
#include "pnut/time.topics.h"

#include "ubx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOCATION_PATH      24     /* A device file's name, its NUL in */
#define LOCATION_CHIP      24
#define LOCATION_WAITERS   4      /* Current calls waiting at once */
#define LOCATION_FOLLOWS   2      /* Follow streams at once */
#define LOCATION_OUT       1024   /* Bytes queued for the receiver */
#define LOCATION_DUMP_MAX  24576  /* Orbit data kept, at most */

/* Approximate positions: a grid of 0.05 degrees (1e-7 units), about
 * 5.6 km of latitude
 */

#define LOCATION_GRID      500000
#define LOCATION_GRID_MM   5560000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The receiver's device: NuttX's GNSS driver's raw file on the device, a
 * fake in the tests.  open returns a descriptor readable while bytes
 * wait, or a negated errno value; read returns 0 when none wait; write
 * returns the bytes taken, maybe fewer than given.
 */

struct location_dev_ops_s
{
  CODE int (*open)(FAR void *arg, FAR const char *path);
  CODE void (*close)(FAR void *arg);
  CODE ssize_t (*read)(FAR void *arg, FAR uint8_t *buf, size_t len);
  CODE ssize_t (*write)(FAR void *arg, FAR const uint8_t *buf,
                        size_t len);
};

struct location_config_s
{
  FAR const struct location_dev_ops_s *ops;
  FAR void *arg;

  /* Board's service, asked for the receiver; or the receiver as given,
   * Board not asked (device non-NULL)
   */

  FAR const char *board;
  FAR const char *device;
  FAR const char *chip;

  FAR const char *dir;            /* The saved state's directory */
  uint32_t linger_ms;             /* On after the last caller */
};

/* A position, as the receiver gave it */

struct location_fix_s
{
  bool valid;
  int32_t lat;                    /* 1e-7 degrees */
  int32_t lon;
  int32_t alt_mm;
  uint32_t hacc_mm;
  uint32_t vacc_mm;
  uint32_t speed_mms;
  uint32_t heading_e5;
  uint8_t sats;
  int64_t utc;                    /* 0: not known */
  uint64_t taken_ms;              /* CLOCK_MONOTONIC */
};

/* A Current call waiting for a position */

struct location_waiter_s
{
  bool used;
  struct pnut_request_s req;
  uint64_t deadline_ms;
  uint32_t accuracy_mm;
  bool precise;
};

/* A Follow stream */

struct location_follow_s
{
  bool used;
  struct pnut_stream_s stream;
  uint32_t interval_ms;
  uint32_t accuracy_mm;
  bool precise;
  uint64_t last_ms;
};

/* The receiver's states */

enum location_state_e
{
  LOCATION_OFF = 0,
  LOCATION_STARTING,              /* On: configured, the saved state read,
                                   * then given */
  LOCATION_ON,
  LOCATION_SAVING,                /* Its orbit data being dumped */
  LOCATION_CLOSING                /* The state being written */
};

struct location_s
{
  struct pnut_module_s module;    /* First */
  struct location_config_s config;

  struct pnut_location_server_s server;
  bool serving;
  FAR struct pnut_publisher_s *topic;
  FAR struct pnut_publisher_s *gnss_time;
  struct pnut_time_change_reader_s time_reader;
  bool time_open;
  struct pnut_board_client_s board;
  bool board_open;
  FAR struct pnut_timer_s *tick;
  FAR struct pnut_timer_s *flush;

  /* The receiver, once known; whether it speaks UBX */

  bool present;
  bool ubx;
  char path[LOCATION_PATH];
  char chip[LOCATION_CHIP];

  /* Its state; the wall clock's source good enough to give it */

  uint8_t state;
  int fd;
  bool clock_good;
  uint8_t clock_source;           /* Time's, a pnut_time_info_source_t */
  bool heard;                     /* It said something since it was on */
  bool configured;                /* It took the configuration */
  bool loaded;                    /* The saved state read */
  uint8_t tries;                  /* The configuration sent, times */
  uint64_t start_ms;              /* Switched on */
  uint64_t sent_ms;               /* The configuration sent last */
  struct ubx_parser_s parser;
  bool has_fix;                   /* The topic's */
  uint8_t sats;
  uint64_t time_sent_ms;          /* gnss_time's last */
  uint64_t idle_ms;               /* Since nobody asks, 0 while asked */
  struct location_fix_s fix;      /* The last good one */

  struct location_waiter_s waiters[LOCATION_WAITERS];
  struct location_follow_s follows[LOCATION_FOLLOWS];

  /* Bytes for the receiver, written as it takes them; then the saved
   * orbit data's frames, from blob_pos
   */

  uint8_t out[LOCATION_OUT];
  size_t out_len;
  FAR uint8_t *blob;              /* The saved state, read at start */
  size_t blob_len;
  size_t blob_pos;
  uint16_t restored;              /* Its frames given to the receiver */
  uint16_t restore_acks;          /* The receiver's acks of them */
  uint64_t restore_ms;            /* The last given */

  /* The orbit data dumped before switching off */

  FAR uint8_t *dump;
  size_t dump_len;
  uint32_t dump_count;            /* Its frames heard, kept or not */
  uint64_t dump_deadline;

  /* A job on the worker: reading the state, or writing it */

  bool job;
  int job_ret;
  struct location_fix_s job_fix;
  FAR uint8_t *job_blob;          /* The state read, for the loop */
  size_t job_len;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's GNSS driver, its raw file (device.c) */

extern const struct location_dev_ops_s g_location_gnss;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: location_init, location_deinit
 *
 * Description:
 *   Set Location up, as a module to add to a loop; after the loop is
 *   destroyed, free what it kept.
 *
 ****************************************************************************/

void location_init(FAR struct location_s *loc,
                   FAR const struct location_config_s *config);
void location_deinit(FAR struct location_s *loc);

/****************************************************************************
 * Name: location_round
 *
 * Description:
 *   A position as an approximate one: to the grid, no altitude, speed or
 *   heading, the grid's accuracy.
 *
 ****************************************************************************/

void location_round(FAR pnut_location_position_t *pos);

#endif /* __PNUT_OS_SENSORS_LOCATION_H */
