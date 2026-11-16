/****************************************************************************
 * pnut-os/tests/unit/test_ui_manifest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <pb_encode.h>

#include "manifest.h"
#include "pnut/manifest.pb.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char g_dir[] = "/tmp/pnut-manifest-XXXXXX";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void write_file(FAR const char *id, FAR const void *data, size_t len)
{
  char path[160];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s.manifest", g_dir, id);
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(data, 1, len, f), len);
  fclose(f);
}

static void write_manifest(FAR const char *id,
                           FAR const pnut_app_manifest_t *m)
{
  uint8_t buf[1024];
  pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));

  assert_true(pb_encode(&out, PNUT_APP_MANIFEST_FIELDS, m));
  write_file(id, buf, out.bytes_written);
}

static int setup(void **state)
{
  strcpy(g_dir, "/tmp/pnut-manifest-XXXXXX");
  assert_non_null(mkdtemp(g_dir));
  return 0;
}

static int teardown(void **state)
{
  char cmd[64];

  snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
  assert_int_equal(system(cmd), 0);
  return 0;
}

/****************************************************************************
 * Name: test_manifest_load
 *
 * Description:
 *   A manifest's icon, name and templates; a template it does not list is
 *   not allowed, nor one the catalogue does not have.
 ****************************************************************************/

static void test_manifest_load(void **state)
{
  pnut_app_manifest_t m;
  struct manifest_s got;

  memset(&m, 0, sizeof(m));
  strcpy(m.icon, "notes");
  strcpy(m.name, "Notes");
  m.templates_count = 2;
  m.templates[0] = PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST;
  m.templates[1] = PNUT_UI_TEMPLATE_UI_TEMPLATE_BUSY;
  write_manifest("org.example.notes", &m);

  assert_int_equal(manifest_load(g_dir, "org.example.notes", &got), 0);
  assert_string_equal(got.icon, "notes");
  assert_string_equal(got.name, "Notes");
  assert_true(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST));
  assert_true(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_BUSY));
  assert_false(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL));
  assert_false(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE));
  assert_false(manifest_allows(&got, 31));
  assert_false(manifest_allows(&got, -1));

  /* The developer's shell: every template the catalogue has */

  manifest_any(&got);
  assert_true(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL));
  assert_true(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_CONFIRM));
  assert_false(manifest_allows(&got, PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE));
  assert_false(manifest_allows(&got, 31));
}

/****************************************************************************
 * Name: test_manifest_bad
 *
 * Description:
 *   No manifest; one that does not decode; one over 1 KB; an id that is
 *   not one is never looked for.
 ****************************************************************************/

static void test_manifest_bad(void **state)
{
  static const uint8_t garbage[] =
    {
      0xff, 0xff, 0xff, 0xff, 0x0f
    };

  static uint8_t big[MANIFEST_SIZE_MAX + 1];
  pnut_app_manifest_t m;
  pb_ostream_t out;
  struct manifest_s got;
  size_t pad;

  assert_int_equal(manifest_load(g_dir, "org.example.none", &got), -ENOENT);

  write_file("org.example.garbage", garbage, sizeof(garbage));
  assert_int_equal(manifest_load(g_dir, "org.example.garbage", &got),
                   -EINVAL);

  /* One byte over: a good manifest, then a field it does not know (tag
   * 15, bytes), which a decoder skips; only the size refuses it
   */

  memset(&m, 0, sizeof(m));
  strcpy(m.icon, "notes");
  m.templates_count = 1;
  m.templates[0] = PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST;
  out = pb_ostream_from_buffer(big, sizeof(big));
  assert_true(pb_encode(&out, PNUT_APP_MANIFEST_FIELDS, &m));
  pad = sizeof(big) - out.bytes_written - 3;
  big[out.bytes_written]     = (15 << 3) | 2;
  big[out.bytes_written + 1] = 0x80 | (pad & 0x7f);
  big[out.bytes_written + 2] = pad >> 7;
  memset(big + out.bytes_written + 3, 'x', pad);
  write_file("org.example.big", big, sizeof(big));
  assert_int_equal(manifest_load(g_dir, "org.example.big", &got), -EINVAL);

  big[out.bytes_written + 1] = 0x80 | ((pad - 1) & 0x7f);
  big[out.bytes_written + 2] = (pad - 1) >> 7;
  write_file("org.example.big", big, sizeof(big) - 1);
  assert_int_equal(manifest_load(g_dir, "org.example.big", &got), 0);
  assert_string_equal(got.icon, "notes");

  write_file("x", garbage, sizeof(garbage));
  assert_int_equal(manifest_load(g_dir, "../x", &got), -EINVAL);
}

/****************************************************************************
 * Name: test_manifest_good_id
 ****************************************************************************/

static void test_manifest_good_id(void **state)
{
  char id[65];

  assert_true(manifest_good_id("org.pnut.messages"));
  assert_true(manifest_good_id("org.example.a-b_c.d9"));

  memset(id, 'a', sizeof(id));
  memcpy(id, "org.", 4);
  id[63] = '\0';
  assert_true(manifest_good_id(id));
  id[63] = 'a';
  id[64] = '\0';
  assert_false(manifest_good_id(id));

  assert_false(manifest_good_id(""));
  assert_false(manifest_good_id("a"));
  assert_false(manifest_good_id("Org.x"));
  assert_false(manifest_good_id(".a.b"));
  assert_false(manifest_good_id("a.b."));
  assert_false(manifest_good_id("a..b"));
  assert_false(manifest_good_id("a/b.c"));
  assert_false(manifest_good_id("a b.c"));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_manifest_load, setup, teardown),
    cmocka_unit_test_setup_teardown(test_manifest_bad, setup, teardown),
    cmocka_unit_test(test_manifest_good_id),
  };

  return cmocka_run_group_tests_name("ui_manifest", tests, NULL, NULL);
}
