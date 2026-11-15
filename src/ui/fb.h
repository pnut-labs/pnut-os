/****************************************************************************
 * pnut-os/src/ui/fb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_FB_H
#define __PNUT_OS_UI_FB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

#include <pnut/compiler.h>

#include "display.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Open the device's framebuffer (fb.c, the device's alone): a 1-bit one,
 * mapped, filling in the display's frame and ops, and the mapping's length
 * (unmapped from config->frame).  Returns its descriptor, kept open while
 * the display is, or a negated errno value.
 */

int fb_open(FAR const char *path, FAR struct display_config_s *config,
            FAR size_t *len);

#endif /* __PNUT_OS_UI_FB_H */
