/****************************************************************************
 * pnut-os/tests/unit/test_pool.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdalign.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/pool.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void test_pool_takes_and_gives_back(void **state)
{
  struct pnut_pool_s pool;
  FAR uint8_t *a;
  FAR uint8_t *b;
  FAR uint8_t *c;

  assert_int_equal(pnut_pool_init(&pool, 5, 3), 0);
  assert_true(pool.size >= sizeof(FAR void *));
  assert_int_equal(pool.avail, 3);

  a = pnut_pool_alloc(&pool);
  b = pnut_pool_alloc(&pool);
  c = pnut_pool_alloc(&pool);
  assert_non_null(a);
  assert_non_null(b);
  assert_non_null(c);
  assert_true(a != b && b != c && a != c);
  assert_null(pnut_pool_alloc(&pool));
  assert_int_equal(pool.avail, 0);

  memset(b, 0xa5, 5);
  pnut_pool_free(&pool, b);
  assert_int_equal(pool.avail, 1);
  assert_ptr_equal(pnut_pool_alloc(&pool), b);
  assert_int_equal(b[0], 0);
  assert_int_equal(b[4], 0);

  pnut_pool_deinit(&pool);
}

static void test_pool_aligns(void **state)
{
  struct pnut_pool_s pool;
  FAR void *a;
  FAR void *b;

  assert_int_equal(pnut_pool_init(&pool, 1, 2), 0);
  a = pnut_pool_alloc(&pool);
  b = pnut_pool_alloc(&pool);
  assert_int_equal((uintptr_t)a % alignof(max_align_t), 0);
  assert_int_equal((uintptr_t)b % alignof(max_align_t), 0);
  pnut_pool_deinit(&pool);
}

static void test_pool_empty(void **state)
{
  struct pnut_pool_s pool;

  assert_int_equal(pnut_pool_init(&pool, 8, 0), 0);
  assert_null(pnut_pool_alloc(&pool));
  pnut_pool_deinit(&pool);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_pool_takes_and_gives_back),
    cmocka_unit_test(test_pool_aligns),
    cmocka_unit_test(test_pool_empty),
  };

  return cmocka_run_group_tests_name("pool", tests, NULL, NULL);
}
