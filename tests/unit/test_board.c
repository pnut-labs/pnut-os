/****************************************************************************
 * pnut-os/tests/unit/test_board.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

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
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "board.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Board and a module that asks it for the description, in one loop */

struct board_test_s
{
  FAR struct pnut_loop_s *loop;
  struct board_s board;
  struct pnut_module_s module;
  struct pnut_board_client_s client;
  char dir[64];
  char path[96];
  int status;                     /* The answer's; -1 none yet */
  pnut_board_description_t desc;  /* The answer's description */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void board_test_stop(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void board_test_described(FAR struct pnut_board_client_s *client,
                                 int status,
                                 FAR const pnut_board_description_t *out,
                                 FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct board_test_s *t = arg;

  t->status = status;
  if (out != NULL)
    {
      t->desc = *out;
    }

  pnut_loop_stop(t->loop, 0);
}

static void board_test_connected(FAR struct pnut_board_client_s *client,
                                 bool connected, FAR void *arg)
{
  FAR struct board_test_s *t = arg;
  pnut_board_describe_request_t req;

  if (connected)
    {
      memset(&req, 0, sizeof(req));
      assert_int_equal(pnut_board_describe(client, &req, 0,
                                           board_test_described, t), 0);
    }
}

static int board_test_start(FAR struct pnut_module_s *module,
                            FAR struct pnut_loop_s *loop)
{
  FAR struct board_test_s *t = (FAR struct board_test_s *)
    ((FAR uint8_t *)module - offsetof(struct board_test_s, module));

  assert_int_equal(pnut_board_connect(loop, BOARD_SERVICE,
                                      board_test_connected, t, &t->client),
                   0);
  pnut_module_ready(module);
  return 0;
}

static void board_test_stop_module(FAR struct pnut_module_s *module,
                                   FAR struct pnut_loop_s *loop)
{
  FAR struct board_test_s *t = (FAR struct board_test_s *)
    ((FAR uint8_t *)module - offsetof(struct board_test_s, module));

  pnut_board_disconnect(&t->client);
}

/* Ask Board for its description, read from path */

static void board_test_run(FAR struct board_test_s *t, FAR const char *path)
{
  struct pnut_loop_config_s config;
  struct board_config_s bconfig;

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  bconfig.path = path;
  board_init(&t->board, &bconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->board.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = board_test_start;
  t->module.stop  = board_test_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  t->status = -1;
  memset(&t->desc, 0, sizeof(t->desc));
  pnut_timer_start(t->loop, 3000, 0, board_test_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(t->loop), 0);
  pnut_loop_destroy(t->loop);
}

static void board_test_write(FAR const char *path, FAR const void *data,
                             size_t len)
{
  FILE *f = fopen(path, "wb");

  assert_non_null(f);
  assert_int_equal(fwrite(data, 1, len, f), len);
  fclose(f);
}

static FAR struct board_test_s *board_test_new(void)
{
  FAR struct board_test_s *t = calloc(1, sizeof(*t));

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-board-%d", (int)getpid());
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->path, sizeof(t->path), "%s/board.pb", t->dir);
  return t;
}

static void board_test_free(FAR struct board_test_s *t)
{
  unlink(t->path);
  rmdir(t->dir);
  free(t);
}

/* The description as written, with what only device data may say left
 * out: no unit is provisioned yet
 */

static void test_board_describe(void **state)
{
  FAR struct board_test_s *t = board_test_new();
  FAR pnut_board_description_t *d = calloc(1, sizeof(*d));
  FAR pnut_board_part_t *part;
  uint8_t buffer[PNUT_BOARD_DESCRIPTION_SIZE];
  pb_ostream_t stream;

  assert_non_null(d);
  d->has_identity = true;
  strlcpy(d->identity.maker, "Maker", sizeof(d->identity.maker));
  strlcpy(d->identity.model, "Model", sizeof(d->identity.model));
  strlcpy(d->identity.target, "test", sizeof(d->identity.target));
  strlcpy(d->identity.serial, "not the file's",
          sizeof(d->identity.serial));
  d->identity.provisioned = true;

  part = &d->parts[d->parts_count++];
  part->kind = PNUT_BOARD_PART_KIND_SCREEN;
  strlcpy(part->name, "screen", sizeof(part->name));
  strlcpy(part->device, "/dev/fb0", sizeof(part->device));
  part->which_detail         = PNUT_BOARD_PART_SCREEN_TAG;
  part->detail.screen.tech   = PNUT_BOARD_SCREEN_TECH_EPAPER;
  part->detail.screen.width  = 240;
  part->detail.screen.height = 320;
  part->detail.screen.depth  = 1;

  part = &d->parts[d->parts_count++];
  part->kind = PNUT_BOARD_PART_KIND_BATTERY;
  strlcpy(part->name, "battery", sizeof(part->name));
  part->which_detail                 = PNUT_BOARD_PART_BATTERY_TAG;
  part->detail.battery.capacity_mah  = 1400;
  part->detail.battery.empty_mv      = 3300;

  stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
  assert_true(pb_encode(&stream, PNUT_BOARD_DESCRIPTION_FIELDS, d));
  board_test_write(t->path, buffer, stream.bytes_written);

  board_test_run(t, t->path);
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_string_equal(t->desc.identity.maker, "Maker");
  assert_string_equal(t->desc.identity.model, "Model");
  assert_string_equal(t->desc.identity.target, "test");
  assert_string_equal(t->desc.identity.serial, "");
  assert_false(t->desc.identity.provisioned);
  assert_int_equal(t->desc.parts_count, 2);
  assert_int_equal(t->desc.parts[0].kind, PNUT_BOARD_PART_KIND_SCREEN);
  assert_string_equal(t->desc.parts[0].device, "/dev/fb0");
  assert_int_equal(t->desc.parts[0].which_detail,
                   PNUT_BOARD_PART_SCREEN_TAG);
  assert_int_equal(t->desc.parts[0].detail.screen.tech,
                   PNUT_BOARD_SCREEN_TECH_EPAPER);
  assert_int_equal(t->desc.parts[0].detail.screen.height, 320);
  assert_int_equal(t->desc.parts[1].detail.battery.empty_mv, 3300);

  free(d);
  board_test_free(t);
}

/* No description named, no file, one that does not decode, or one longer
 * than the largest: Board answers that it has none, and the program goes
 * on
 */

static void test_board_none(void **state)
{
  FAR struct board_test_s *t = board_test_new();
  FAR uint8_t *big;
  size_t i;
  static const uint8_t garbage[] =
  {
    0x0a, 0xff, 0xff, 0xff
  };

  board_test_run(t, NULL);
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);

  board_test_run(t, t->path);
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);

  board_test_write(t->path, garbage, sizeof(garbage));
  board_test_run(t, t->path);
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);

  /* Empty strings, field 1 of the identity, one after another: they would
   * decode, but no description is that long
   */

  big = calloc(1, PNUT_BOARD_DESCRIPTION_SIZE + 2);
  assert_non_null(big);
  for (i = 0; i + 1 < PNUT_BOARD_DESCRIPTION_SIZE + 2; i += 2)
    {
      big[i] = 0x0a;
    }

  board_test_write(t->path, big, PNUT_BOARD_DESCRIPTION_SIZE + 2);
  board_test_run(t, t->path);
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
  free(big);

  board_test_free(t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_board_describe),
    cmocka_unit_test(test_board_none),
  };

  return cmocka_run_group_tests_name("board", tests, NULL, NULL);
}
