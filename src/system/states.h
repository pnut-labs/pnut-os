/****************************************************************************
 * pnut-os/src/system/states.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_STATES_H
#define __PNUT_OS_SYSTEM_STATES_H

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
#include <pnut/topic.h>

#include "pnut/services.pb.h"
#include "pnut/services.pnut.h"
#include "pnut/services.topics.h"
#include "programs.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The service's socket, in the run directory */

#define STATES_SERVICE    "services"

/* NxInit's names, without their NUL, and its lines (control.h) */

#define STATES_NAME_MAX   31
#define STATES_LINE_MAX   80

/* Commands sent to NxInit and not answered yet; pids NxInit said are no
 * service's, remembered so as not to ask again
 */

#define STATES_PENDING    8
#define STATES_STRANGERS  8

/* How long the caller lookup waits for NxInit, in milliseconds; after it
 * failed, it does not ask again for as long
 */

#define STATES_ASK_MS     1000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Service states (RFC 0006): a module of the system program that watches
 * NxInit's services on its control socket, publishes their changes on the
 * services topic, serves pnut.Services, and tells the program's other
 * modules which program a task is.
 */

struct states_config_s
{
  FAR const char *initctl;        /* NxInit's control socket, or NULL */
  FAR const char *programs;       /* The programs' table (programs.h) */
  uint8_t services;               /* NxInit's services, at most */
};

struct states_service_s
{
  char name[STATES_NAME_MAX + 1];
  uint8_t state;                  /* pnut_service_state_state_t */
  bool seen;                      /* In the list being read */
  pid_t pid;                      /* 0 when not running */
};

enum states_command_e
{
  STATES_WATCH = 0,
  STATES_LIST,
  STATES_START,
  STATES_STOP,
};

/* A command sent to NxInit: its answers come in the commands' order */

struct states_command_s
{
  uint8_t kind;
  struct pnut_request_s req;      /* START, STOP: the request to answer */
};

struct states_s
{
  struct pnut_module_s module;    /* First: the module is the states */
  struct states_config_s config;
  struct programs_s programs;

  /* NxInit's services, in the order of their names */

  FAR struct states_service_s *services;
  uint8_t nservices;

  /* The watch on NxInit's control socket */

  int fd;                         /* -1 when not connected */
  FAR struct pnut_timer_s *timer; /* Kept: the next try */
  uint32_t backoff;
  bool version;                   /* NxInit's version line has come */
  bool listed;                    /* A whole list has come */
  bool warned;                    /* A lost watch was logged */
  uint16_t listing;               /* Lines of a list still to come */
  uint16_t len;
  char in[STATES_LINE_MAX];
  struct states_command_s pending[STATES_PENDING];
  uint8_t head;
  uint8_t npending;

  pid_t strangers[STATES_STRANGERS];
  uint8_t nextstranger;
  int64_t holdoff;                /* No asking NxInit before, in ms */

  /* A name NxInit gave, when the table had no room for it */

  char asked[STATES_NAME_MAX + 1];

  struct pnut_services_server_s server;
  bool serving;
  FAR struct pnut_publisher_s *changes;
  uint32_t seq;

  union
  {
    pnut_services_page_t page;
    pnut_service_state_t state;
    pnut_services_who_reply_t who;
  } out;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: states_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop; the
 *   configuration's strings are kept, not copied.
 *
 ****************************************************************************/

void states_init(FAR struct states_s *states,
                 FAR const struct states_config_s *config);

/****************************************************************************
 * Name: states_deinit
 ****************************************************************************/

void states_deinit(FAR struct states_s *states);

/****************************************************************************
 * Name: states_who
 *
 * Description:
 *   Which service of NxInit's a task is: for a program, its name (RFC
 *   0007).  A task the watch has not shown yet, such as a program just
 *   started, is asked of NxInit directly, waiting STATES_ASK_MS at most.
 *
 * Returned Value:
 *   Zero (OK), with *namep valid until the next call or until the loop
 *   runs on; -ESRCH for a task that is no running service's, or whose
 *   name cannot be a program's; -EAGAIN when NxInit cannot tell now.
 *
 ****************************************************************************/

int states_who(FAR struct states_s *states, pid_t pid,
               FAR const char **namep);

/****************************************************************************
 * Name: states_runs
 *
 * Description:
 *   Whether a program runs a service, by the programs' table.
 *
 ****************************************************************************/

bool states_runs(FAR struct states_s *states, FAR const char *program,
                 FAR const char *service);

#endif /* __PNUT_OS_SYSTEM_STATES_H */
