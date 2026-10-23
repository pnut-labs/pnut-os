/****************************************************************************
 * pnut-os/src/lib/pnut_tz.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_TZ_INTERNAL_H
#define __PNUT_OS_LIB_PNUT_TZ_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A zone of the table tools/gen-zones writes (tz_zones.c) */

struct pnut_tz_zone_s
{
  FAR const char *name;
  uint8_t rule;                   /* Its rule's index */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const struct pnut_tz_zone_s g_pnut_tz_zones[];
extern const char * const g_pnut_tz_rules[];
extern const size_t g_pnut_tz_nzones;

#endif /* __PNUT_OS_LIB_PNUT_TZ_INTERNAL_H */
