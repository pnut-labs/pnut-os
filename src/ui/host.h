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
  HOST_STOPPING                   /* Asked to stop, or never to run */
};

struct host_app_s
{
  FAR struct host_s *host;
  uint8_t state;
  bool shell;                     /* Typed in a developer's shell */
  bool connected;                 /* Its Open accepted: its stream open */
  bool drawn;                     /* A screen drawn since its start */
  uint32_t order;                 /* When it was last open */
  uint32_t start;                 /* Its Start's, which its answer names */
  FAR struct pnut_timer_s *busy;  /* Nothing drawn: the UI's Busy */
  FAR struct pnut_timer_s *due;   /* Its Open's deadline, or its Stop's */
  char id[MANIFEST_ID_MAX + 1];
  struct manifest_s manifest;
  struct pnut_stream_s stream;    /* Open's: the events */
  pnut_ui_event_t queue[HOST_EVENTS];
  uint8_t head;
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
  uint32_t orders;
  uint32_t starts;
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
 * is stopped.  A warm app is brought back, and sent Shown.  0 when it is
 * started, brought back, or is opening already; -ENOENT with no manifest;
 * -EBUSY with no room for another app, or while the app stops.
 */

int host_start(FAR struct host_s *h, FAR const char *id);

/* Cancel an app being opened: told to stop if it has opened, stopped by
 * Service states if not; the UI's Busy taken off
 */

void host_cancel(FAR struct host_app_s *app);

/* Home: the open app kept warm (one warm at most: the one open longest
 * ago is stopped), an app being opened cancelled
 */

void host_home(FAR struct host_s *h);

/* An event for the open app, or for an app, sent as its credit allows.
 * -ENOTCONN when there is none, or it is not connected; -ENOSPC when
 * HOST_EVENTS wait already: the app's stream is ended, and the app taken
 * off.
 */

int host_event(FAR struct host_s *h, FAR const pnut_ui_event_t *ev);
int host_event_to(FAR struct host_app_s *app, FAR const pnut_ui_event_t *ev);

/* The open app, or NULL */

FAR struct host_app_s *host_app(FAR struct host_s *h);

#endif /* __PNUT_OS_UI_HOST_H */
