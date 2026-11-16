/****************************************************************************
 * pnut-os/src/ui/app.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_APP_H
#define __PNUT_OS_UI_APP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pnut/compiler.h>

#include "pnut/ui.pb.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct ui_s;

/* The open app's screens (RFC 0037), as host.c hands them over once it
 * has checked who sends them.  0, or -EINVAL for a description that does
 * not hold together; an Update or Rows for a screen not on top is
 * dropped, and is 0.
 */

int app_show(FAR struct ui_s *ui, FAR const pnut_ui_show_request_t *in);
int app_update(FAR struct ui_s *ui, FAR const pnut_ui_update_request_t *in);
int app_rows(FAR struct ui_s *ui, FAR const pnut_ui_rows_request_t *in);

/* The app is gone: its screen and its sheet off */

void app_gone(FAR struct ui_s *ui);

#endif /* __PNUT_OS_UI_APP_H */
