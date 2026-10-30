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
#include "settings.h"

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

  /* Airplane mode: Settings in the loop, and the topic read */

  struct settings_s settings;
  struct pnut_settings_client_s other;
  struct pnut_airplane_mode_reader_s airplanes;
  char sdir[80];
  char seen[8];                   /* The values announced, in order, a
                                   * repeated one once */
  int nseen;
  int asked;                      /* Switches asked for */
  bool connected;                 /* The Settings client is */
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
  static FAR const char * const topics[] =
  {
    "airplane_mode", "settings"
  };

  char path[128];
  size_t i;

  /* The topics Board announces and reads: directories on the computer */

  for (i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    {
      snprintf(path, sizeof(path), "%s/topic.%s/last", t->dir, topics[i]);
      unlink(path);
      snprintf(path, sizeof(path), "%s/topic.%s", t->dir, topics[i]);
      rmdir(path);
    }

  unlink(t->path);
  assert_int_equal(rmdir(t->dir), 0);
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

  /* LTE bands 1, 3 and 20, and the last the buffer holds, 128 */

  part = &d->parts[d->parts_count++];
  part->kind = PNUT_BOARD_PART_KIND_MODEM;
  strlcpy(part->name, "modem", sizeof(part->name));
  part->which_detail                     = PNUT_BOARD_PART_MODEM_TAG;
  part->detail.modem.lte_bands.size      = 16;
  part->detail.modem.lte_bands.bytes[0]  = 0x05;
  part->detail.modem.lte_bands.bytes[2]  = 0x08;
  part->detail.modem.lte_bands.bytes[15] = 0x80;
  part->detail.modem.gsm_bands_count     = 2;
  part->detail.modem.gsm_bands[0]        = PNUT_BOARD_MODEM_GSM_GSM_900;
  part->detail.modem.gsm_bands[1]        = PNUT_BOARD_MODEM_GSM_DCS_1800;
  part->detail.modem.sims                = 1;

  d->lines_count = 1;
  d->lines[0].kind = PNUT_BOARD_PART_KIND_MODEM;
  strlcpy(d->lines[0].part, "modem", sizeof(d->lines[0].part));
  d->lines[0].role = PNUT_BOARD_LINE_ROLE_POWER;
  strlcpy(d->lines[0].device, "/dev/modem_pwr", sizeof(d->lines[0].device));

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
  assert_int_equal(t->desc.parts_count, 3);
  assert_int_equal(t->desc.parts[0].kind, PNUT_BOARD_PART_KIND_SCREEN);
  assert_string_equal(t->desc.parts[0].device, "/dev/fb0");
  assert_int_equal(t->desc.parts[0].which_detail,
                   PNUT_BOARD_PART_SCREEN_TAG);
  assert_int_equal(t->desc.parts[0].detail.screen.tech,
                   PNUT_BOARD_SCREEN_TECH_EPAPER);
  assert_int_equal(t->desc.parts[0].detail.screen.height, 320);
  assert_int_equal(t->desc.parts[1].detail.battery.empty_mv, 3300);
  assert_int_equal(t->desc.parts[2].which_detail,
                   PNUT_BOARD_PART_MODEM_TAG);
  assert_int_equal(t->desc.parts[2].detail.modem.lte_bands.size, 16);
  assert_int_equal(t->desc.parts[2].detail.modem.lte_bands.bytes[0], 0x05);
  assert_int_equal(t->desc.parts[2].detail.modem.lte_bands.bytes[2], 0x08);
  assert_int_equal(t->desc.parts[2].detail.modem.lte_bands.bytes[15],
                   0x80);
  assert_int_equal(t->desc.parts[2].detail.modem.gsm_bands_count, 2);
  assert_int_equal(t->desc.parts[2].detail.modem.gsm_bands[1],
                   PNUT_BOARD_MODEM_GSM_DCS_1800);
  assert_int_equal(t->desc.parts[2].detail.modem.sims, 1);
  assert_int_equal(t->desc.lines_count, 1);
  assert_int_equal(t->desc.lines[0].kind, PNUT_BOARD_PART_KIND_MODEM);
  assert_string_equal(t->desc.lines[0].part, "modem");
  assert_int_equal(t->desc.lines[0].role, PNUT_BOARD_LINE_ROLE_POWER);
  assert_string_equal(t->desc.lines[0].device, "/dev/modem_pwr");

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

/* Airplane mode: announced once Settings' value is read, then switched on
 * and off again as the system UI would, through Settings.  Off at the
 * end: on the simulator the topic outlives the test.
 */

static void board_test_switched(FAR struct pnut_settings_client_s *client,
                                int status,
                                FAR const pnut_settings_reply_t *out,
                                FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct board_test_s *t = arg;

  t->status = status;
}

static void board_test_other(FAR struct pnut_settings_client_s *client,
                             bool connected, FAR void *arg)
{
  FAR struct board_test_s *t = arg;

  t->connected = connected;
}

static void board_test_airplane(FAR struct pnut_airplane_mode_reader_s *r,
                                FAR const pnut_airplane_mode_t *msg,
                                FAR void *arg)
{
  FAR struct board_test_s *t = arg;
  char c = msg->on ? '1' : '0';

  /* A state's topic may give a value again (on the simulator, the first
   * came twice): the changes are what count
   */

  if (t->nseen < (int)sizeof(t->seen) - 1 &&
      (t->nseen == 0 || t->seen[t->nseen - 1] != c))
    {
      t->seen[t->nseen++] = c;
    }
}

/* Each switch once the message before it came, and Settings is reached */

static void board_test_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct board_test_s *t = arg;
  pnut_settings_set_request_t req;

  if (t->nseen >= 3)
    {
      pnut_loop_stop(loop, 0);
      return;
    }

  if (!t->connected || t->nseen != t->asked + 1)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_BOARD_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, "airplane", sizeof(req.key));
  req.has_value           = true;
  req.value.which_value   = PNUT_SETTING_VALUE_BOOLEAN_TAG;
  req.value.value.boolean = t->asked == 0;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0,
                                     board_test_switched, t), 0);
  t->asked++;
}

static int board_test_start_airplane(FAR struct pnut_module_s *module,
                                     FAR struct pnut_loop_s *loop)
{
  FAR struct board_test_s *t = (FAR struct board_test_s *)
    ((FAR uint8_t *)module - offsetof(struct board_test_s, module));

  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE,
                                         board_test_other, t, &t->other),
                   0);
  assert_int_equal(pnut_airplane_mode_subscribe(loop, board_test_airplane,
                                                t, &t->airplanes), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, board_test_tick, t, NULL),
                   0);
  pnut_module_ready(module);
  return 0;
}

static void board_test_stop_airplane(FAR struct pnut_module_s *module,
                                     FAR struct pnut_loop_s *loop)
{
  FAR struct board_test_s *t = (FAR struct board_test_s *)
    ((FAR uint8_t *)module - offsetof(struct board_test_s, module));

  pnut_airplane_mode_unsubscribe(&t->airplanes);
  pnut_settings_disconnect(&t->other);
}

static void test_board_airplane(void **state)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 16,
    .texts    = 4,
    .choices  = 8,
  };

  FAR struct board_test_s *t = board_test_new();
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct board_config_s bconfig;
  char path[128];

  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  assert_int_equal(mkdir(t->sdir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  bconfig.path = NULL;
  board_init(&t->board, &bconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->board.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = board_test_start_airplane;
  t->module.stop  = board_test_stop_airplane;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  /* Off once read, on and off as switched: no message before Settings'
   * value
   */

  t->status = -1;
  pnut_timer_start(t->loop, 3000, 0, board_test_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(t->loop), 0);
  assert_string_equal(t->seen, "010");
  assert_int_equal(t->status, PNUT_STATUS_OK);
  pnut_loop_destroy(t->loop);

  settings_deinit(&t->settings);

  snprintf(path, sizeof(path), "%s/%s.pb", t->sdir,
           PNUT_BOARD_SETTINGS_OWNER);
  unlink(path);
  assert_int_equal(rmdir(t->sdir), 0);
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
    cmocka_unit_test(test_board_airplane),
  };

  return cmocka_run_group_tests_name("board", tests, NULL, NULL);
}
