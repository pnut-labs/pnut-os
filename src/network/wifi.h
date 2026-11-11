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
#define WIFI_KNOWN_MAX    16      /* Known networks (RFC 0031) */
#define WIFI_DNS_MAX      2       /* Name servers kept from DHCP */

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

/* What DHCP gave: IPv4, network order, 0 for none */

struct wifi_lease_s
{
  uint32_t addr;
  uint32_t router;
  uint32_t dns[WIFI_DNS_MAX];
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

  /* An address from the network (DHCP), set with its router as the
   * interface's: 0 and the lease, whose router and name servers Network
   * may use (RFC 0015); or a negated errno value
   */

  CODE int (*address)(FAR void *arg, FAR struct wifi_lease_s *lease);

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
  uint32_t again_ms;              /* Before a known network whose join
                                   * failed is tried on its own again,
                                   * the first time; 0 for 30 s.  Doubled
                                   * after each failure, up to 8 minutes
                                   * (RFC 0031) */
  uint32_t idle_ms;               /* Between looks for a known network
                                   * while none is in reach; 0 for 60 s */
};

/* A known network (RFC 0031): in memory, last used first */

struct wifi_known_s
{
  char name[WIFI_NAME_MAX];       /* Empty: none */
  bool secured;
  bool refused;                   /* A join the owner asked for failed:
                                   * not joined on its own until the
                                   * owner joins it again */
  uint8_t fails;                  /* Joins on its own failed in a row */
  uint64_t not_before;            /* Not joined on its own before, in
                                   * milliseconds of the monotonic clock */

  /* Its passphrase, new or given by the owner, until a write takes it:
   * then kept in Settings only
   */

  bool pending;
  char pass[WIFI_PASS_MAX];
};

/* A slot of Wi-Fi's settings, as Settings holds it */

struct wifi_slot_s
{
  char name[WIFI_NAME_MAX];       /* Empty: free */
  bool secured;
  bool unsure;                    /* Its write stopped half way: its
                                   * passphrase may be another's */
};

/* A request waiting for Service states to say who sent it */

struct wifi_ask_s
{
  FAR struct wifi_s *wifi;
  bool used;
  uint8_t method;
  struct pnut_request_s req;
  pnut_wifi_join_request_t join;
  pnut_wifi_forget_request_t forget;
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
  struct wifi_lease_s lease;
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

  /* The known networks (RFC 0031): in memory, last used first, read from
   * the slots as Wi-Fi starts; and the slots, written to match after
   * each change, one setting at a time
   */

  struct wifi_known_s known[WIFI_KNOWN_MAX];
  struct wifi_slot_s slots[WIFI_KNOWN_MAX];
  bool known_loaded;
  pnut_wifi_known_networks_t known_out;   /* Known's answer */

  /* The write running: each slot's plan and its passphrase, read before
   * any slot is written
   */

  bool saving;
  bool save_again;                /* The known networks changed since */
  bool save_reading;              /* Reading, then writing */
  uint8_t save_at;                /* The slot */
  uint8_t save_step;              /* Its passphrase, name, security */
  uint8_t save_fails;             /* Calls failed in a row */
  int8_t save_from[WIFI_KNOWN_MAX];
  struct wifi_slot_s save_to[WIFI_KNOWN_MAX];
  char save_psk[WIFI_KNOWN_MAX][WIFI_PASS_MAX];
  FAR struct pnut_timer_s *save_timer;    /* A failed call, again */

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
  bool secured;                   /* Its security */
  bool new_pass;                  /* The owner gave its passphrase */
  char passphrase[WIFI_PASS_MAX]; /* Until joined: then kept in Settings
                                   * as a secret, never in memory */
  bool leave_wanted;

  /* Joining a known network on its own (RFC 0031): a scan, then the
   * one joined last if in reach, else the strongest; its passphrase read
   * from Settings first
   */

  bool held;                      /* Left by the owner: not until a join
                                   * or Wi-Fi off and on */
  bool seek_wanted;               /* A look for a known network is due */
  bool seeking;                   /* The scan runs for it */
  bool fetch_wanted;              /* A passphrase is to be read */
  bool fetching;                  /* And is being read */
  bool fetch_stale;               /* Its answer is no longer wanted */
  FAR struct pnut_timer_s *idle;  /* Until the next look */

  bool link_due;                  /* A look at the link is due */
  FAR struct pnut_timer_s *link;  /* Until the next look at the link */
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
