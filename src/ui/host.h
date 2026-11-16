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

#include "manifest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The system UI's service for apps, its socket's name (RFC 0037) */

#define HOST_SERVICE    "ui"

/* The catalogue level served, and what Welcome tells of lists */

#define HOST_LEVEL      1
#define HOST_ROWS_PAGE  20
#define HOST_ROWS_KEPT  200

/* Events waiting for the app's credit, Welcome among them; one more ends
 * its stream.  How long Service states has to say who an app is.  How
 * soon an event a full send buffer kept back is tried again.
 */

#define HOST_EVENTS     16
#define HOST_WHO_MS     1000
#define HOST_RETRY_MS   50

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ui_s;

/* The open app (one at a time until the life cycle, RFC 0037 step 2) */

struct host_app_s
{
  bool used;                      /* Open accepted: the app is open */
  bool shell;                     /* Typed in a developer's shell */
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

  struct host_app_s app;
  FAR struct pnut_timer_s *retry;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Serve apps on HOST_SERVICE, naming them with Service states' Who on
 * the service services, their manifests in the folder apps
 */

int host_open(FAR struct host_s *h, FAR struct ui_s *ui,
              FAR struct pnut_loop_s *loop, FAR const char *apps,
              FAR const char *services);
void host_close(FAR struct host_s *h);

/* An event for the open app, sent as its credit allows.  -ENOTCONN when
 * no app is open; -ENOSPC when HOST_EVENTS wait already: the app's stream
 * is ended, and the app taken off.
 */

int host_event(FAR struct host_s *h, FAR const pnut_ui_event_t *ev);

/* The open app, or NULL */

FAR struct host_app_s *host_app(FAR struct host_s *h);

#endif /* __PNUT_OS_UI_HOST_H */
