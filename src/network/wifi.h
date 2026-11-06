/****************************************************************************
 * pnut-os/src/network/wifi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_NETWORK_WIFI_H
#define __PNUT_OS_NETWORK_WIFI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/services.pnut.h"
#include "pnut/wifi.pnut.h"
#include "pnut/wifi.settings.h"
#include "pnut/wifi.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_SERVICE      "wifi"
#define WIFI_SCAN_MAX     24      /* Networks a scan keeps, the strongest */
#define WIFI_NAME_MAX     33      /* A network's name, its NUL included */
#define WIFI_PASS_MAX     64

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A network a scan found */

struct wifi_found_s
{
  char name[WIFI_NAME_MAX];
  int16_t signal;                 /* dBm */
  uint8_t channel;                /* 0: not known */
  bool secured;
};

/* The radio, as the driver works it: each call blocks, and runs on a
 * worker.  The program gives NuttX's (network/radio.c); the tests a fake.
 */

struct wifi_ops_s
{
  /* Bring the radio up (the driver, then the interface); take it down,
   * any network left and the address cleared
   */

  CODE int (*up)(FAR void *arg);
  CODE void (*down)(FAR void *arg);

  /* The networks in reach, the strongest WIFI_SCAN_MAX; their number, or
   * a negated errno value
   */

  CODE int (*scan)(FAR void *arg, FAR struct wifi_found_s *out, int max);

  /* Join a network (an empty passphrase: an open one) and wait until its
   * access point takes the device: 0; -ECONNREFUSED, the driver refused
   * the join; -ETIMEDOUT, the access point did not take the device (a
   * wrong passphrase and a network out of reach look the same)
   */

  CODE int (*join)(FAR void *arg, FAR const char *name,
                   FAR const char *passphrase, FAR int16_t *signal);

  /* An address from the network (DHCP), set with its router and name
   * servers: 0 and the address, network order; or a negated errno value
   */

  CODE int (*address)(FAR void *arg, FAR uint32_t *addr);

  /* Whether still joined, and the signal */

  CODE bool (*linked)(FAR void *arg, FAR int16_t *signal);
};

struct wifi_config_s
{
  FAR const char *services;       /* Service states', to tell who calls;
                                   * NULL: anyone may (the tests) */
  FAR const struct wifi_ops_s *ops;
  FAR void *arg;
  uint32_t link_ms;               /* Between looks at the link; 0 for
                                   * 10 s */
  uint32_t again_ms;              /* Before a lost network is joined
                                   * again, the first time; 0 for 30 s.
                                   * Doubled after each failure, up to 16
                                   * times as long */
};

/* A request waiting for Service states to say who sent it */

struct wifi_ask_s
{
  FAR struct wifi_s *wifi;
  bool used;
  uint8_t method;
  struct pnut_request_s req;
  pnut_wifi_join_request_t join;
};

/* What a worker does: one job at a time */

enum wifi_job_e
{
  WIFI_JOB_NONE = 0,
  WIFI_JOB_UP,
  WIFI_JOB_DOWN,
  WIFI_JOB_SCAN,
  WIFI_JOB_JOIN,
  WIFI_JOB_LINK
};

struct wifi_work_s
{
  FAR const struct wifi_ops_s *ops;
  FAR void *arg;
  uint8_t job;                    /* enum wifi_job_e */
  bool up;                        /* The radio up when it starts */
  bool keep;                      /* A scan: the radio left up after */
  char name[WIFI_NAME_MAX];
  char passphrase[WIFI_PASS_MAX];
  struct wifi_found_s found[WIFI_SCAN_MAX];
  int nfound;
  int16_t signal;
  uint32_t addr;
  int ret;
  bool linked;
  bool radio;                     /* The radio up when it ends */
};

struct wifi_s
{
  struct pnut_module_s module;    /* First: the module is the service */
  struct wifi_config_s config;

  /* What it serves and announces */

  struct pnut_wifi_server_s server;
  bool serving;
  struct pnut_services_client_s services;
  bool services_open;
  struct wifi_ask_s asks[2];
  FAR struct pnut_publisher_s *topic;
  pnut_wifi_info_t state;

  /* Its settings */

  struct pnut_wifi_settings_s settings;
  bool settings_open;
  bool enabled;                   /* wifi.enabled, as read */

  /* The radio, and the work on it */

  bool radio;                     /* Up */
  bool radio_failed;              /* Would not come up: not tried again
                                   * until Wi-Fi is turned off and on,
                                   * or a request needs it */
  bool working;
  bool in_done;                   /* In wifi_done(), which goes on after */
  struct wifi_work_s work;

  /* What waits for the worker */

  bool scan_wanted;
  struct pnut_stream_s scan;      /* The scan's stream, while open */
  int pushed;                     /* Its networks sent so far */
  struct wifi_found_s found[WIFI_SCAN_MAX];
  int nfound;
  bool scanned;                   /* found holds the scan's networks */

  bool join_wanted;
  bool joining_req;               /* A request waits for the join */
  struct pnut_request_s join_req;
  char name[WIFI_NAME_MAX];       /* The network joined, or to join */
  char passphrase[WIFI_PASS_MAX]; /* Its passphrase, in memory only, to
                                   * join it again once lost */
  uint8_t tries;                  /* Joins of a lost network failed */
  bool leave_wanted;

  bool link_due;                  /* A look at the link is due */
  FAR struct pnut_timer_s *link;  /* Until the next look at the link */
  FAR struct pnut_timer_s *again; /* Until a lost network is joined */
  FAR struct pnut_timer_s *kick;  /* A job not submitted: once more */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: wifi_init
 *
 * Description:
 *   Set the module up; pnut_module_add() then starts it.
 *
 ****************************************************************************/

void wifi_init(FAR struct wifi_s *wifi,
               FAR const struct wifi_config_s *config);

/****************************************************************************
 * Name: wifi_end
 *
 * Description:
 *   After the loop is destroyed (its worker joined): the radio down if a
 *   job still running at the stop left it up.
 *
 ****************************************************************************/

void wifi_end(FAR struct wifi_s *wifi);

#endif /* __PNUT_OS_NETWORK_WIFI_H */
