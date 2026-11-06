/****************************************************************************
 * pnut-os/tests/unit/test_storage.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Storage's first part (RFC 0011): the slots from Board's description,
 * each card mounted through the mount given (a fake here: the computer's
 * mount needs root), and the result on the storage topic.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#define _GNU_SOURCE

#include <errno.h>
#include <ftw.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pb_encode.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "board.h"
#include "storage.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_DEVICE    "/dev/mmcsd0"
#define TEST_OTHER     "/dev/mmcsd1"
#define TEST_MB        7382
#define TEST_WAIT_MS   2000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct storage_test_s
{
  struct pnut_module_s module;    /* First */
  struct pnut_storage_info_reader_s reader;

  int ret[STORAGE_SLOTS];         /* What the fake mount gives, by slot */
  int mounts;                     /* Calls to it */
  char devices[STORAGE_SLOTS][STORAGE_DEVICE];
  char paths[STORAGE_SLOTS][32];

  pnut_storage_info_t info;       /* The last message */
  int messages;
  int expect;                     /* Messages: the slots, then the mounts */
  bool done;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fake_mount(FAR void *arg, FAR const char *device,
                      FAR const char *path, FAR uint32_t *size_mb)
{
  FAR struct storage_test_s *t = arg;
  int i = t->mounts++;

  assert_true(i < STORAGE_SLOTS);
  strlcpy(t->devices[i], device, sizeof(t->devices[i]));
  strlcpy(t->paths[i], path, sizeof(t->paths[i]));
  if (t->ret[i] == 0)
    {
      *size_mb = TEST_MB + i;
    }

  return t->ret[i];
}

static void test_info(FAR struct pnut_storage_info_reader_s *reader,
                      FAR const pnut_storage_info_t *msg, FAR void *arg)
{
  FAR struct storage_test_s *t = arg;

  t->info = *msg;
  t->messages++;
  if (t->messages == t->expect)
    {
      t->done = true;
      pnut_loop_stop(t->module.loop, 0);
    }
}

static int test_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct storage_test_s *t = (FAR struct storage_test_s *)module;

  assert_int_equal(pnut_storage_info_subscribe(loop, test_info, t,
                                               &t->reader), 0);
  pnut_module_ready(module);
  return 0;
}

static void test_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct storage_test_s *t = (FAR struct storage_test_s *)module;

  pnut_storage_info_unsubscribe(&t->reader);
}

static void test_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static int test_remove(FAR const char *path, FAR const struct stat *st,
                       int flag, FAR struct FTW *ftw)
{
  return remove(path);
}

/* Write Board's description: the slots' devices, among other parts */

static void test_description(FAR const char *path,
                             FAR const char *const *slots, int nslots)
{
  FAR pnut_board_description_t *d = calloc(1, sizeof(*d));
  FAR uint8_t *buffer = malloc(PNUT_BOARD_DESCRIPTION_SIZE);
  FAR pnut_board_part_t *p;
  pb_ostream_t stream;
  FILE *f;
  int i;

  assert_non_null(d);
  assert_non_null(buffer);

  p = &d->parts[d->parts_count++];
  p->kind = PNUT_BOARD_PART_KIND_CHARGER;
  strlcpy(p->device, "/dev/charger0", sizeof(p->device));

  for (i = 0; i < nslots; i++)
    {
      p = &d->parts[d->parts_count++];
      p->kind = PNUT_BOARD_PART_KIND_STORAGE_SLOT;
      strlcpy(p->name, "microsd", sizeof(p->name));
      strlcpy(p->device, slots[i], sizeof(p->device));
    }

  stream = pb_ostream_from_buffer(buffer, PNUT_BOARD_DESCRIPTION_SIZE);
  assert_true(pb_encode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, d));
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buffer, 1, stream.bytes_written, f),
                   stream.bytes_written);
  fclose(f);
  free(d);
  free(buffer);
}

/* Run Storage until it announces what it mounted: with Board's
 * description of nslots slots, or without Board, device the one slot
 */

static void test_run(FAR struct storage_test_s *t, bool with_board,
                     FAR const char *const *slots, int nslots,
                     FAR const char *device)
{
  FAR struct storage_s *storage = calloc(1, sizeof(*storage));
  FAR struct board_s *board = calloc(1, sizeof(*board));
  struct pnut_loop_config_s lconfig;
  struct storage_config_s config;
  struct board_config_s bconfig;
  FAR struct pnut_loop_s *loop;
  char dir[48];
  char path[96];

  assert_non_null(storage);
  assert_non_null(board);
  t->expect = nslots > 0 || device != NULL ? 2 : 1;

  snprintf(dir, sizeof(dir), "/tmp/pnut-storage-%d", (int)getpid());
  nftw(dir, test_remove, 8, FTW_DEPTH | FTW_PHYS);   /* A run before's */
  assert_int_equal(mkdir(dir, 0700), 0);
  snprintf(path, sizeof(path), "%s/board.pb", dir);

  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);

  if (with_board)
    {
      test_description(path, slots, nslots);
      memset(&bconfig, 0, sizeof(bconfig));
      bconfig.path = path;
      board_init(board, &bconfig);
      assert_int_equal(pnut_module_add(loop, &board->module), 0);
    }

  t->module.name  = "storage_test";
  t->module.start = test_start;
  t->module.stop  = test_stop;
  assert_int_equal(pnut_module_add(loop, &t->module), 0);

  memset(&config, 0, sizeof(config));
  config.board     = with_board ? BOARD_SERVICE : NULL;
  config.device    = device;
  config.media     = "/media";
  config.mount     = fake_mount;
  config.mount_arg = t;
  storage_init(storage, &config);
  assert_int_equal(pnut_module_add(loop, &storage->module), 0);

  assert_int_equal(pnut_timer_start(loop, TEST_WAIT_MS, 0, test_timeout,
                                    NULL, NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_loop_destroy(loop);
  free(storage);
  free(board);

  assert_int_equal(nftw(dir, test_remove, 8, FTW_DEPTH | FTW_PHYS), 0);
}

/* Board's slot, its card FAT: mounted at /media/sd0 and announced */

static void test_storage_mounted(void **state)
{
  static const char *const slots[] =
  {
    TEST_DEVICE
  };

  struct storage_test_s t;

  memset(&t, 0, sizeof(t));
  test_run(&t, true, slots, 1, NULL);

  assert_true(t.done);
  assert_int_equal(t.mounts, 1);
  assert_string_equal(t.devices[0], TEST_DEVICE);
  assert_string_equal(t.paths[0], "/media/sd0");
  assert_int_equal(t.info.volumes_count, 1);
  assert_string_equal(t.info.volumes[0].name, "sd0");
  assert_string_equal(t.info.volumes[0].path, "/media/sd0");
  assert_int_equal(t.info.volumes[0].state,
                   PNUT_STORAGE_VOLUME_STATE_MOUNTED);
  assert_int_equal(t.info.volumes[0].size_mb, TEST_MB);
}

/* Two slots: sd0 and sd1, each as its card is.  A card that is not FAT
 * is left as it is; one that answers nothing is absent.
 */

static void test_storage_two(void **state)
{
  static const char *const slots[] =
  {
    TEST_DEVICE, TEST_OTHER
  };

  struct storage_test_s t;

  memset(&t, 0, sizeof(t));
  t.ret[0] = -EINVAL;
  t.ret[1] = -ENODEV;
  test_run(&t, true, slots, 2, NULL);

  assert_true(t.done);
  assert_int_equal(t.mounts, 2);
  assert_string_equal(t.devices[1], TEST_OTHER);
  assert_string_equal(t.paths[1], "/media/sd1");
  assert_int_equal(t.info.volumes_count, 2);
  assert_string_equal(t.info.volumes[1].name, "sd1");
  assert_int_equal(t.info.volumes[0].state,
                   PNUT_STORAGE_VOLUME_STATE_UNSUPPORTED);
  assert_int_equal(t.info.volumes[1].state,
                   PNUT_STORAGE_VOLUME_STATE_ABSENT);
  assert_int_equal(t.info.volumes[0].size_mb, 0);
  assert_int_equal(t.info.volumes[1].size_mb, 0);
}

/* Without Board: the slot given; a mount that fails otherwise */

static void test_storage_failed(void **state)
{
  struct storage_test_s t;

  memset(&t, 0, sizeof(t));
  t.ret[0] = -EIO;
  test_run(&t, false, NULL, 0, TEST_DEVICE);

  assert_true(t.done);
  assert_int_equal(t.mounts, 1);
  assert_int_equal(t.info.volumes_count, 1);
  assert_int_equal(t.info.volumes[0].state,
                   PNUT_STORAGE_VOLUME_STATE_FAILED);
  assert_int_equal(t.info.volumes[0].size_mb, 0);
}

/* A description without slots: none announced, nothing mounted */

static void test_storage_none(void **state)
{
  struct storage_test_s t;

  memset(&t, 0, sizeof(t));
  test_run(&t, true, NULL, 0, NULL);

  assert_true(t.done);
  assert_int_equal(t.mounts, 0);
  assert_int_equal(t.info.volumes_count, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_storage_mounted),
    cmocka_unit_test(test_storage_two),
    cmocka_unit_test(test_storage_failed),
    cmocka_unit_test(test_storage_none),
  };

  return cmocka_run_group_tests_name("storage", tests, NULL, NULL);
}
