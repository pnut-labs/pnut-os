/****************************************************************************
 * pnut-os/src/lib/pool.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/pool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Objects are aligned for any type, and hold the free list's link */

#define POOL_ALIGN  alignof(max_align_t)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_pool_init(FAR struct pnut_pool_s *pool, size_t size,
                   uint16_t count)
{
  FAR uint8_t *object;
  uint16_t i;

  if (size < sizeof(FAR void *))
    {
      size = sizeof(FAR void *);
    }

  size = (size + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1);

  memset(pool, 0, sizeof(*pool));
  pool->size = size;
  pool->count = count;

  if (count == 0)
    {
      return OK;
    }

  /* malloc() aligns less than max_align_t on some systems (NuttX's
   * simulator: 8 bytes against 16), so ask for the alignment.
   */

  if (posix_memalign((FAR void **)&pool->base, POOL_ALIGN,
                     size * count) != 0)
    {
      pool->base = NULL;
      return -ENOMEM;
    }

  /* Chain every object into the free list, first object first */

  for (i = count; i > 0; i--)
    {
      object = pool->base + (size_t)(i - 1) * size;
      *(FAR void **)object = pool->head;
      pool->head = object;
    }

  pool->avail = count;
  return OK;
}

void pnut_pool_deinit(FAR struct pnut_pool_s *pool)
{
  free(pool->base);
  memset(pool, 0, sizeof(*pool));
}

FAR void *pnut_pool_alloc(FAR struct pnut_pool_s *pool)
{
  FAR void *object = pool->head;

  if (object == NULL)
    {
      return NULL;
    }

  pool->head = *(FAR void **)object;
  pool->avail--;
  memset(object, 0, pool->size);
  return object;
}

void pnut_pool_free(FAR struct pnut_pool_s *pool, FAR void *object)
{
  if (object != NULL)
    {
      *(FAR void **)object = pool->head;
      pool->head = object;
      pool->avail++;
    }
}
