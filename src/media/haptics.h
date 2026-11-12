/****************************************************************************
 * pnut-os/src/media/haptics.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_MEDIA_HAPTICS_H
#define __PNUT_OS_MEDIA_HAPTICS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/board.pnut.h"
#include "pnut/haptics.pnut.h"
#include "pnut/haptics.settings.h"
#include "pnut/haptics.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HAPTICS_SEQUENCE   8      /* Library effects in one sequence */
#define HAPTICS_STEPS      16     /* A pattern's steps */
#define HAPTICS_PATH       32     /* The motor's device, its NUL included */
#define HAPTICS_CHIP       24     /* Its chip's name, likewise */
#define HAPTICS_MAX_MS     5000   /* A caller's pattern, at most */
#define HAPTICS_APP_MS     1000   /* An app's, at most one in this */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The motor: its driver on the device, a fake in the tests.  A level is
 * 0 (still) to 100; ms 0 keeps it until the next call.  A sequence is of
 * the motor chip's own effects, by their numbers.  Each returns OK or a
 * negated errno value.
 */

struct haptics_motor_ops_s
{
  CODE int (*open)(FAR void *arg, FAR const char *path);
  CODE void (*close)(FAR void *arg);
  CODE int (*level)(FAR void *arg, uint8_t level, uint32_t ms);
  CODE int (*sequence)(FAR void *arg, FAR const uint8_t *effects,
                       size_t count);
  CODE int (*stop)(FAR void *arg);
};

struct haptics_config_s
{
  FAR const struct haptics_motor_ops_s *ops;
  FAR void *arg;

  /* Board's service, asked for the motor; or the motor's device and chip
   * as given, Board not asked (device non-NULL)
   */

  FAR const char *board;
  FAR const char *device;
  FAR const char *chip;
};

/* A step of what plays: the motor at a level (before the strength
 * scales it) for a time
 */

struct haptics_step_s
{
  uint16_t ms;
  uint8_t level;
};

struct haptics_s
{
  struct pnut_module_s module;    /* First */
  struct haptics_config_s config;

  struct pnut_haptics_server_s server;
  bool serving;
  struct pnut_haptics_settings_s settings;
  bool settings_open;
  FAR struct pnut_publisher_s *topic;
  struct pnut_board_client_s board;
  bool board_open;

  /* The motor, once known */

  bool motor;
  bool library;                   /* Its chip plays library effects */
  char path[HAPTICS_PATH];
  char chip[HAPTICS_CHIP];

  /* What plays: a use, an effect or a caller's pattern, its steps and the
   * one playing, whether it repeats; a sequence plays as one step
   */

  bool playing;
  uint8_t use;                    /* pnut_haptics_use_t */
  bool pattern;
  uint8_t effect;                 /* pnut_haptics_effect_name_t */
  bool repeat;
  struct haptics_step_s steps[HAPTICS_STEPS];
  uint8_t nsteps;
  uint8_t step;
  FAR struct pnut_timer_s *timer;

  uint64_t app_at;                /* An app's last start, CLOCK_MONOTONIC */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* The force-feedback driver, /dev/input_ffN (motor.c) */

extern const struct haptics_motor_ops_s g_haptics_ff;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: haptics_init
 *
 * Description:
 *   Set Haptics up, as a module to add to a loop.
 *
 ****************************************************************************/

void haptics_init(FAR struct haptics_s *haptics,
                  FAR const struct haptics_config_s *config);

#endif /* __PNUT_OS_MEDIA_HAPTICS_H */
