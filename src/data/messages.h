/****************************************************************************
 * pnut-os/src/data/messages.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_DATA_MESSAGES_H
#define __PNUT_OS_DATA_MESSAGES_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/messages.pnut.h"
#include "pnut/messages.topics.h"
#include "pnut/services.pnut.h"

#include "store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MESSAGES_SERVICE   "messages"
#define MESSAGES_UI        "ui"        /* The system UI's program */
#define MESSAGES_SMS       "sms"       /* SMS's transport, and its program */
#define MESSAGES_SMS_OWNER "telephony"
#define MESSAGES_TRANSPORTS 4          /* Transports registered at once */
#define MESSAGES_ASKS      4           /* Callers waiting to be told apart */
#define MESSAGES_QUEUE     8           /* Jobs waiting for the store */
#define MESSAGES_RETRY     30000       /* ms before the store is read again,
                                        * when it could not be */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the store is asked to do, by a caller or by Messages itself */

enum messages_kind_e
{
  MESSAGES_LIST,                  /* A page of conversations, which needs
                                   * the files read, but no worker */
  MESSAGES_READ,                  /* A page of a conversation */
  MESSAGES_MARK_READ,
  MESSAGES_DELETE,                /* A message, or a whole conversation */
  MESSAGES_SEND,
  MESSAGES_DELIVER,               /* From a transport */
  MESSAGES_REGISTER,              /* A transport, which needs no store */
  MESSAGES_ROOM,                  /* Room made, over the limit */
  MESSAGES_COMPACT,               /* Deleted messages' segment rewritten */
  MESSAGES_LATEST,                /* A conversation's newest text read */
  MESSAGES_EMPTY                  /* A conversation with no message gone */
};

/* The work a worker has: which part of the union is in use */

enum messages_work_e
{
  MESSAGES_WORK_NONE = 0,
  MESSAGES_WORK_LOAD,
  MESSAGES_WORK_WRITE,
  MESSAGES_WORK_READ,
  MESSAGES_WORK_REMOVE,
  MESSAGES_WORK_REWRITE
};

struct messages_s;

/* A job for the store, waiting for its turn: the store does one at a
 * time, each planned once the last is committed
 */

struct messages_job_s
{
  uint8_t kind;                   /* enum messages_kind_e */
  bool answer;                    /* A caller waits for it */
  struct pnut_request_s req;
  char transport[65];             /* DELIVER's */
  union
    {
      pnut_messages_page_request_t list;
      pnut_messages_read_request_t read;
      pnut_messages_mark_read_request_t mark_read;
      pnut_messages_delete_request_t delete;
      pnut_messages_send_request_t send;
      pnut_messages_register_request_t reg;
      pnut_messages_deliver_request_t deliver;
      uint64_t conversation;      /* LATEST and EMPTY */
    } in;
};

/* A transport, and the connection it registered on */

struct messages_transport_s
{
  bool used;
  char name[65];
  struct pnut_endpoint_s from;
};

/* The store's files read at start, on a worker */

struct messages_load_s
{
  struct store_s store;
  int ret;
};

/* A caller waiting for Service states to say who it is */

struct messages_ask_s
{
  FAR struct messages_s *messages;
  bool used;
  struct messages_job_s job;
};

struct messages_config_s
{
  FAR const char *dir;            /* The store's directory */
  uint64_t limit;                 /* Its limit, in bytes */
  uint32_t segmax;                /* A segment's size at most */
  FAR const char *services;       /* Service states' service, or NULL to
                                   * let every caller in (tests) */
  uint32_t retry;                 /* ms before a broken store is read
                                   * again; 0 for MESSAGES_RETRY */
};

struct messages_s
{
  struct pnut_module_s module;    /* First: the library's */
  struct messages_config_s config;

  /* The store: its index, and the job a worker has */

  struct store_s store;
  bool loaded;                    /* The files read */
  bool failed;                    /* and found unreadable, or the index
                                   * short of what is on flash */
  bool working;                   /* A worker has the job */
  bool stopped;
  bool full;
  bool more;                      /* A page read has more after it */
  bool fresh;                     /* Read again, and no change made since:
                                   * a failure now is not the index's */
  bool room;                      /* Room to make, before any job */
  bool compact;                   /* A segment to compact, after it */
  FAR struct pnut_timer_s *retry; /* Until the store is read again */
  uint8_t retries;                /* Loads failed in a row */
  struct messages_job_s current;
  uint8_t work_kind;              /* enum messages_work_e */
  union
    {
      struct messages_load_s load;
      struct store_write_s write;
      struct store_read_s read;
      struct store_remove_s remove;
      struct store_rewrite_s rewrite;
    } work;

  /* Jobs waiting */

  struct messages_job_s queue[MESSAGES_QUEUE];
  uint8_t head;
  uint8_t count;

  /* Who calls */

  struct pnut_services_client_s services;
  bool services_open;
  struct messages_ask_s asks[MESSAGES_ASKS];

  /* The transports */

  struct messages_transport_s transports[MESSAGES_TRANSPORTS];

  /* What it serves and announces */

  struct pnut_messages_server_s server;
  bool serving;
  FAR struct pnut_publisher_s *changes;
  uint32_t seq;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: messages_init
 *
 * Description:
 *   Messages (RFC 0029), to add to the data program's loop.
 *
 ****************************************************************************/

void messages_init(FAR struct messages_s *messages,
                   FAR const struct messages_config_s *config);

/****************************************************************************
 * Name: messages_deinit
 *
 * Description:
 *   Give back what a job left when the program stopped while a worker had
 *   it: after pnut_loop_destroy(), which waits for the job.
 *
 ****************************************************************************/

void messages_deinit(FAR struct messages_s *messages);

#endif /* __PNUT_OS_DATA_MESSAGES_H */
