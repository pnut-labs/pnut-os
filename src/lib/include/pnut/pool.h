/****************************************************************************
 * pnut-os/src/lib/include/pnut/pool.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_POOL_H
#define __PNUT_OS_LIB_PNUT_POOL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A fixed pool of objects of one size, taken from the heap once, when the
 * program starts (RFC 0023).  A pool is used from one thread only.
 */

struct pnut_pool_s
{
  FAR uint8_t *base;              /* The objects */
  FAR void *head;                 /* The first free object */
  size_t size;                    /* One object, aligned */
  uint16_t count;                 /* Objects in all */
  uint16_t avail;                 /* Objects free */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_pool_init
 *
 * Description:
 *   Allocate count objects of size bytes each.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENOMEM.
 *
 ****************************************************************************/

int pnut_pool_init(FAR struct pnut_pool_s *pool, size_t size,
                   uint16_t count);

/****************************************************************************
 * Name: pnut_pool_deinit
 *
 * Description:
 *   Free the pool's memory, and every object with it.
 *
 ****************************************************************************/

void pnut_pool_deinit(FAR struct pnut_pool_s *pool);

/****************************************************************************
 * Name: pnut_pool_alloc
 *
 * Returned Value:
 *   An object, zeroed; NULL when the pool is empty.
 *
 ****************************************************************************/

FAR void *pnut_pool_alloc(FAR struct pnut_pool_s *pool);

/****************************************************************************
 * Name: pnut_pool_free
 *
 * Description:
 *   Return an object to the pool it came from.
 *
 ****************************************************************************/

void pnut_pool_free(FAR struct pnut_pool_s *pool, FAR void *object);

#endif /* __PNUT_OS_LIB_PNUT_POOL_H */
