/****************************************************************************
 * pnut-os/src/ui/glue.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_GLUE_H
#define __PNUT_OS_UI_GLUE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* LVGL on the program's loop (RFC 0019): its tick is the monotonic clock,
 * and its timers run from one of the loop's.  LVGL is called from the
 * loop's thread only.
 */

int glue_start(FAR struct pnut_loop_s *loop);
void glue_stop(void);

/* Run LVGL's timers soon, after one was made or changed */

void glue_kick(void);

/* The monotonic clock, in ms */

uint64_t glue_now(void);

#endif /* __PNUT_OS_UI_GLUE_H */
