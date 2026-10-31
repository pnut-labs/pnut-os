/****************************************************************************
 * pnut-os/src/telephony/telephony.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_TELEPHONY_H
#define __PNUT_OS_TELEPHONY_TELEPHONY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <pnut/compiler.h>
#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>

#include "pnut/board.pnut.h"
#include "pnut/board.topics.h"
#include "pnut/services.pnut.h"
#include "pnut/telephony.pnut.h"
#include "pnut/telephony.settings.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"

#include "at.h"
#include "mux.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TELEPHONY_SERVICE  "telephony"
#define TELEPHONY_PATH     24        /* A device file's name, at most */
#define TELEPHONY_ASKS     4         /* Callers waiting to be told apart */
#define TELEPHONY_RESTARTS 3         /* The modem's restarts in an hour */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The modem's lines (RFC 0030) */

enum telephony_line_e
{
  TELEPHONY_POWER = 0,            /* The supply's switch: the rail */
  TELEPHONY_KEY,                  /* The power key */
  TELEPHONY_RING,                 /* The ring indicator */
  TELEPHONY_DTR,
  TELEPHONY_LINES
};

struct telephony_config_s
{
  FAR const char *board;          /* Board's service: the modem and its
                                   * lines (RFC 0024) */
  FAR const char *services;       /* Service states': who calls */

  /* The device, opened by the program: the UART, raw and not blocking,
   * at the modem's speed, as a descriptor or a negated errno value; a
   * line set and read, a negated errno value on failure
   */

  CODE int (*open_uart)(FAR void *arg, FAR const char *path);
  CODE int (*open_line)(FAR void *arg, FAR const char *path);
  CODE int (*set_line)(FAR void *arg, int fd, bool high);
  CODE int (*get_line)(FAR void *arg, int fd, FAR bool *high);
  FAR void *arg;

  /* Times, in ms; zero for the modem's own (telephony.c) */

  uint32_t boot;                  /* To its first answer after power-up */
  uint32_t off;                   /* To stop answering when shut down */
  uint32_t settle;                /* After that, before the rail goes */
  uint32_t key_on;                /* The key pressed to start it */
  uint32_t key_off;               /* and to stop it */
  uint32_t open;                  /* For a channel's answer */
  uint32_t ready;                 /* After the channels open, before
                                   * they answer */
  uint32_t wait;                  /* For Settings' values, at start */
};

/* A request kept while Service states says who sent it */

struct telephony_ask_s
{
  FAR struct telephony_s *tel;
  struct pnut_request_s req;
  uint16_t method;
  bool used;
  union
  {
    pnut_telephony_pin_request_t pin;
    pnut_telephony_puk_request_t puk;
    pnut_telephony_command_request_t command;
  } in;
};

/* Telephony (RFC 0030), step 1: the modem powered, its UART multiplexed,
 * its state and the network's time announced, the SIM's PIN taken.  The
 * telephony program's one module.
 */

struct telephony_s
{
  struct pnut_module_s module;    /* First: the module is the telephony */
  struct telephony_config_s config;

  /* Board's description: the modem's UART, its lines, its bands */

  struct pnut_board_client_s board;
  bool board_open;
  bool described;
  bool modem;                     /* The description has one */
  char uart_path[TELEPHONY_PATH + 1];
  char line_path[TELEPHONY_LINES][TELEPHONY_PATH + 1];
  uint64_t lte_bands;             /* AT+CNBP's masks */
  uint64_t gsm_bands;

  /* The device */

  int uart;
  int line[TELEPHONY_LINES];
  bool writing;                   /* The UART is watched for room */

  /* Settings, Board's airplane mode, Service states */

  struct pnut_telephony_settings_s settings;
  bool settings_open;
  bool settings_read;
  bool enabled;                   /* As last acted on */
  bool gsm;
  struct pnut_airplane_mode_reader_s airplanes;
  bool airplanes_open;
  bool airplane;
  bool air_set;                   /* As the modem was last told */
  bool air_keeping;               /* AT+CFUN=4 given again, unanswered */
  struct pnut_services_client_s services;
  bool services_open;
  struct telephony_ask_s asks[TELEPHONY_ASKS];

  /* What it serves and announces */

  struct pnut_telephony_server_s server;
  bool serving;
  FAR struct pnut_publisher_s *states;
  FAR struct pnut_publisher_s *times;
  pnut_telephony_info_t state;
  pnut_telephony_info_t published;
  bool announced;

  /* The modem */

  struct mux_s mux;
  struct at_s at;
  uint8_t step;                   /* enum telephony_step_e */
  FAR struct pnut_timer_s *timer; /* The step's */
  FAR struct pnut_timer_s *trial; /* GSM's two minutes */
  FAR struct pnut_timer_s *wait;  /* Settings', at start */
  uint64_t since;                 /* The step's start, in ms */
  uint8_t tries;
  uint8_t setup;                  /* The next of the settings */
  uint8_t after;                  /* What follows a power-down */
  uint64_t up_at;                 /* Its start, in ms */
  uint64_t refreshed;             /* Its state read last, in ms */
  uint8_t silent;                 /* Commands unanswered in a row */
  uint8_t nrestarts;
  uint64_t restarts[TELEPHONY_RESTARTS];
  int cereg;                      /* The registrations, LTE and GSM */
  int creg;
  bool refreshing;
  bool again;                     /* Refresh once more after this one */
  bool finishing;                 /* Asked to finish: powering down */
  bool down_key;                  /* Powering down without AT+CPOF */

  /* The network's time */

  bool nitz;                      /* The network sent it, this start */
  uint64_t nitz_at;               /* When, in ms */
  uint64_t clock_at;              /* The modem's clock read last */

  /* Requests waiting for the modem */

  struct pnut_request_s pin_req;
  bool pin_pending;
  struct pnut_request_s command_req;
  bool command_pending;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: telephony_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop.
 *
 ****************************************************************************/

void telephony_init(FAR struct telephony_s *tel,
                    FAR const struct telephony_config_s *config);

#endif /* __PNUT_OS_TELEPHONY_TELEPHONY_H */
