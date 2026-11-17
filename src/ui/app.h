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
struct host_app_s;
struct screen_s;
struct ui_app_s;

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

/* The UI's Busy over an app (scr_opening.c), saying what its wait is:
 * drawn, drawn again as the wait changes (or drawn), taken off, and found
 * (any app's for NULL)
 */

void app_opening(FAR struct ui_s *ui, FAR struct host_app_s *app);
void app_opening_mode(FAR struct ui_s *ui, FAR struct host_app_s *app);
void app_opening_gone(FAR struct ui_s *ui, FAR const struct host_app_s *app);
FAR struct screen_s *app_opening_screen(FAR struct ui_s *ui,
                                        FAR const struct host_app_s *app);

/* Open an app of Menu or Home: Settings, a native app (started through
 * Service states), or the placeholder of one that is not there yet
 */

void app_launch(FAR struct ui_s *ui, FAR const struct ui_app_s *a);

#endif /* __PNUT_OS_UI_APP_H */
