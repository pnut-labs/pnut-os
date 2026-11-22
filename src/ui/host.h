/****************************************************************************
 * pnut-os/src/ui/host.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_HOST_H
#define __PNUT_OS_UI_HOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/service.h>
#include <pnut/timer.h>

#include "pnut/services.pnut.h"
#include "pnut/services.topics.h"
#include "pnut/ui.pnut.h"

#include "applist.h"
#include "manifest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The system UI's service for apps, its socket's name (RFC 0037) */

#define HOST_SERVICE    "ui"

/* The catalogue level served, and what Welcome tells of lists */

#define HOST_LEVEL      1
#define HOST_ROWS_PAGE  20
#define HOST_ROWS_KEPT  APPLIST_KEPT

/* Apps connected at once: the open one, one kept warm, one being opened
 * (RFC 0037), and one stopping.  Events waiting for an app's credit,
 * Welcome among them; one more ends its stream.  How soon an event a full
 * send buffer kept back is tried again.
 */

#define HOST_APPS       4
#define HOST_EXITING    4         /* Apps whose old task may still exit */
#define HOST_RESUMES    4         /* Apps whose resume token is kept */
#define HOST_CRASHES    4         /* Apps whose crashes are counted */
#define HOST_CRASHED    3         /* Crashes in a row that disable one */
#define HOST_EVENTS     16
#define HOST_RETRY_MS   50

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ui_s;
struct host_s;

/* Where an app is in its life (RFC 0037, The life cycle) */

enum host_state_e
{
  HOST_FREE = 0,                  /* No app here */
  HOST_OPENING,                   /* Started; its Open not accepted yet */
  HOST_OPEN,                      /* Its screen on top */
  HOST_WARM,                      /* Running, behind */
  HOST_STOPPING                   /* Asked to stop, or never to run; or
                                   * done with, its task still exiting */
};

/* What the UI's own Busy over an app says (scr_opening.c) */

enum host_wait_e
{
  HOST_WAIT_NONE = 0,
  HOST_WAIT_OPENING,              /* "Opening <app>…" */
  HOST_WAIT_WAITING,              /* An answer owed for 3 s */
  HOST_WAIT_HUNG                  /* ... for 10 s: Wait or Close */
};

struct host_app_s
{
  FAR struct host_s *host;
  uint8_t state;
  bool shell;                     /* Typed in a developer's shell */
  bool connected;                 /* Its Open accepted: its stream open */
  bool drawn;                     /* A screen drawn since its start */
  bool switching;                 /* Brought to the top: its next screen
                                   * takes the full refresh */
  uint32_t order;                 /* When it was last open */
  uint32_t start;                 /* Its Start's, which its answer names */
  FAR struct pnut_timer_s *busy;  /* Nothing drawn: the UI's Busy */
  FAR struct pnut_timer_s *due;   /* Its Open's deadline, or its Stop's */
  FAR struct pnut_timer_s *owe;   /* An answer owed: Back, a page, Shown,
                                   * its first screen */
  FAR struct pnut_timer_s *stall; /* Events waiting with no credit */
  uint8_t wait;                   /* What the UI's Busy says */
  bool closed;                    /* The owner closed it: its place is
                                   * not kept */
  int32_t pid;                    /* Its task, as the services topic told
                                   * since its start; 0 before */
  bool gone;                      /* Its task exited (the topic) */
  bool asked;                     /* Service states asked to stop it */
  bool told;                      /* Sent Stop */
  char id[MANIFEST_ID_MAX + 1];
  struct manifest_s manifest;
  struct pnut_stream_s stream;    /* Open's: the events */
  FAR pnut_ui_event_t *queue;     /* HOST_EVENTS of them, its slot's */
  uint8_t head;
  uint8_t count;
};

/* An app's resume token (RFC 0037), given back in each Welcome: kept
 * until the app gives another or clears it, leaves of itself, crashes,
 * or the owner closes it
 */

struct host_resume_s
{
  char id[MANIFEST_ID_MAX + 1];   /* Empty: none */
  uint32_t order;                 /* When it was given */
  uint16_t len;
  uint8_t token[sizeof(((FAR pnut_ui_resume_request_t *)0)->token.bytes)];
};

/* An app whose old task was not seen gone when its slot was freed at
 * Stop's deadline: a new start of the app waits until the services topic,
 * or Service states' Get, says its service stopped (the owner,
 * 2026-11-22).  When the table is full, the one marked longest ago goes:
 * that app is started at once again, as before.
 */

struct host_exiting_s
{
  char id[MANIFEST_ID_MAX + 1];   /* Empty: none */
  uint32_t order;                 /* When it was marked; Get's answer
                                   * names it */
};

/* An app's crashes in a row (RFC 0037, Failures; RFC 0008) */

struct host_crash_s
{
  char id[MANIFEST_ID_MAX + 1];   /* Empty: none */
  uint8_t count;
};

struct host_s
{
  FAR struct ui_s *ui;
  FAR struct pnut_loop_s *loop;
  FAR const char *apps;           /* The manifests' folder */
  struct pnut_ui_server_s server;
  bool serving;
  struct pnut_services_client_s services;
  bool services_open;

  /* An Open waiting for Service states' Who */

  bool asking;
  struct pnut_request_s asked;
  char claimed[MANIFEST_ID_MAX + 1];

  struct host_app_s slots[HOST_APPS];
  pnut_ui_event_t queues[HOST_APPS][HOST_EVENTS];
  uint32_t orders;
  uint32_t starts;

  /* An app waiting to start, its tile pressed while a task of its id
   * still exited or no slot was free: no slot of its own, no queue (its
   * state HOST_FREE for none); started as soon as it can
   */

  struct host_app_s next;
  FAR struct pnut_timer_s *kick;
  bool getting;                   /* Get asked about its old task */
  bool get_out;                   /* A Get unanswered: one at a time */

  /* Apps whose old task may still exit, their slots freed */

  struct host_exiting_s exiting[HOST_EXITING];
  uint32_t exiting_orders;

  /* Service states' services topic: the apps' tasks started and gone */

  struct pnut_service_change_reader_s changes;
  bool changes_open;
  uint32_t seq;
  struct host_resume_s resumes[HOST_RESUMES];
  uint32_t resume_orders;
  struct host_crash_s crashes[HOST_CRASHES];
  FAR struct pnut_timer_s *retry;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Serve apps on HOST_SERVICE, naming them with Service states' Who on
 * the service services (which starts and stops them too), their
 * manifests in the folder apps
 */

int host_open(FAR struct host_s *h, FAR struct ui_s *ui,
              FAR struct pnut_loop_s *loop, FAR const char *apps,
              FAR const char *services);
void host_close(FAR struct host_s *h);

/* Start an app by its id (a native app's service name), as Menu opens
 * it: Service states starts its task, and its Open makes it the open app;
 * an app being opened before is cancelled.  The UI's Busy is drawn when
 * nothing is by the times' opening; with no Open by their open, the app
 * is stopped.  A warm app is brought back, and sent Shown.  While a task
 * of the app still exits (its slot stopping, or freed at Stop's deadline
 * with the task not seen gone), or with no slot free, the UI's Busy is
 * drawn at once, and the app started as soon as that task is gone and a
 * slot is free: the services topic tells, or Service states' Get where
 * the topic may have missed it.  0 when it is started, brought back,
 * waits to start, or is opening already;
 * -ENOENT with no manifest; -EACCES for an app disabled after
 * HOST_CRASHED crashes in a row.
 */

int host_start(FAR struct host_s *h, FAR const char *id);

/* Cancel an app being opened: told to stop if it has opened, stopped by
 * Service states if not, forgotten if it was waiting to start; the UI's
 * Busy taken off
 */

void host_cancel(FAR struct host_app_s *app);

/* The owner, on the UI's Busy: waits another 10 s for the app, or closes
 * it (told to stop, its resume token forgotten)
 */

void host_wait(FAR struct host_app_s *app);

/* The owner turns a disabled app on again: its crashes forgotten */

void host_enable(FAR struct host_s *h, FAR const char *id);
void host_close_app(FAR struct host_app_s *app);

/* An app's kept resume token, or NULL */

FAR const struct host_resume_s *host_token(FAR struct host_s *h,
                                           FAR const char *id);

/* Home: the open app kept warm (one warm at most: the one open longest
 * ago is stopped), an app being opened, or waiting to start, cancelled
 */

void host_home(FAR struct host_s *h);

/* An event for the open app, or for an app, sent as its credit allows.
 * -ENOTCONN when there is none, or it is not connected; -ENOSPC when
 * HOST_EVENTS wait already: the app's stream is ended, the app taken off,
 * and its task stopped by Service states.
 */

int host_event(FAR struct host_s *h, FAR const pnut_ui_event_t *ev);
int host_event_to(FAR struct host_app_s *app, FAR const pnut_ui_event_t *ev);

/* The open app, or NULL */

FAR struct host_app_s *host_app(FAR struct host_s *h);

#endif /* __PNUT_OS_UI_HOST_H */
