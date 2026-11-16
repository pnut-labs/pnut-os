/****************************************************************************
 * pnut-os/src/ui/app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The open app's screens (RFC 0037).  So far the system UI takes them and
 * draws nothing: the templates follow.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "app.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int app_show(FAR struct ui_s *ui, FAR const pnut_ui_show_request_t *in)
{
  return 0;
}

int app_update(FAR struct ui_s *ui, FAR const pnut_ui_update_request_t *in)
{
  return 0;
}

int app_rows(FAR struct ui_s *ui, FAR const pnut_ui_rows_request_t *in)
{
  return 0;
}

void app_gone(FAR struct ui_s *ui)
{
}
