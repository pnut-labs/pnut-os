/****************************************************************************
 * pnut-os/tests/unit/test_settings.c
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
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/client.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "settings.h"
#include "settings_store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define OWNER  "test.owner"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fixture_s
{
  struct settings_store_s store;
  pnut_settings_register_request_t reg;
  pnut_setting_value_t value;
  pnut_settings_values_t values;
  pnut_settings_schema_t schema;
  uint8_t file[4096];
  bool load;
  uint32_t code;
};

/* A Settings module in a loop, and a client of it */

struct service_s
{
  FAR struct pnut_loop_s *loop;
  struct settings_s settings;
  struct pnut_settings_client_s client;
  pnut_settings_register_request_t reg;
  char dir[64];
  bool reread;                    /* Only read the value back */
  bool two;                       /* Two owners, then wait */
  struct pnut_settings_change_reader_s changes;
  int nchanges;                   /* Changes announced */
  char keys[16][66];              /* Their keys */
  uint32_t versions[16];          /* And versions */
  int step;                       /* Where the calls have got to */
  int status;                     /* The last answer's */
  uint32_t code;                  /* And its detail's code, if any */
  pnut_setting_value_t value;     /* A Get's answer */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct settings_limits_s g_limits =
{
  .owners   = 4,
  .settings = 32,
  .texts    = 8,
  .choices  = 16,
};

static const char * const g_colours[] =
{
  "red", "green", "blue"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Declarations and values */

static FAR pnut_setting_t *declare(FAR pnut_settings_register_request_t *r,
                                   FAR const char *key, int type)
{
  FAR pnut_setting_t *s = &r->settings[r->settings_count++];

  memset(s, 0, sizeof(*s));
  strcpy(s->key, key);
  s->type = type;
  return s;
}

static void declare_int(FAR pnut_settings_register_request_t *r,
                        FAR const char *key, int64_t min, int64_t max,
                        int64_t def)
{
  FAR pnut_setting_t *s = declare(r, key, PNUT_SETTING_TYPE_INTEGER);

  s->min = min;
  s->max = max;
  s->has_default_value             = true;
  s->default_value.which_value     = PNUT_SETTING_VALUE_INTEGER_TAG;
  s->default_value.value.integer   = def;
}

static void declare_string(FAR pnut_settings_register_request_t *r,
                           FAR const char *key, uint32_t max,
                           FAR const char *def)
{
  FAR pnut_setting_t *s = declare(r, key, PNUT_SETTING_TYPE_STRING);

  s->max_length = max;
  s->has_default_value         = true;
  s->default_value.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  strcpy(s->default_value.value.text, def);
}

static void declare_choice(FAR pnut_settings_register_request_t *r,
                           FAR const char *key, FAR const char *def)
{
  FAR pnut_setting_t *s = declare(r, key, PNUT_SETTING_TYPE_CHOICE);
  int i;

  for (i = 0; i < 3; i++)
    {
      strcpy(s->choices[s->choices_count++], g_colours[i]);
    }

  s->has_default_value         = true;
  s->default_value.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  strcpy(s->default_value.value.text, def);
}

static void page(FAR pnut_settings_register_request_t *r, bool first,
                 bool last)
{
  memset(r, 0, sizeof(*r));
  strcpy(r->owner, OWNER);
  r->first = first;
  r->last  = last;
}

static pnut_setting_value_t val_int(int64_t integer)
{
  pnut_setting_value_t v;

  memset(&v, 0, sizeof(v));
  v.which_value   = PNUT_SETTING_VALUE_INTEGER_TAG;
  v.value.integer = integer;
  return v;
}

static pnut_setting_value_t val_bool(bool boolean)
{
  pnut_setting_value_t v;

  memset(&v, 0, sizeof(v));
  v.which_value   = PNUT_SETTING_VALUE_BOOLEAN_TAG;
  v.value.boolean = boolean;
  return v;
}

static pnut_setting_value_t val_text(FAR const char *text)
{
  pnut_setting_value_t v;

  memset(&v, 0, sizeof(v));
  v.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  strcpy(v.value.text, text);
  return v;
}

/* Register a page; returns its status, and checks the detail's code */

static int reg(FAR struct fixture_s *f, uint32_t expect)
{
  FAR const char *key;
  uint32_t code = 0;
  bool load;
  int ret;

  ret = settings_store_register(&f->store, &f->reg, &load, &key, &code);
  assert_int_equal(code, expect);
  return ret;
}

/* The usual schema: an integer, a string, a choice, a boolean, a secret */

static void usual(FAR struct fixture_s *f)
{
  page(&f->reg, true, true);
  declare_int(&f->reg, "volume", 0, 10, 5);
  declare_string(&f->reg, "name", 8, "phone");
  declare_choice(&f->reg, "colour", "green");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
}

static int get(FAR struct fixture_s *f, FAR const char *key,
               uint32_t expect)
{
  uint32_t code = 0;
  int ret;

  ret = settings_store_get(&f->store, OWNER, key, true, &f->value, &code);
  assert_int_equal(code, expect);
  return ret;
}

static int set(FAR struct fixture_s *f, FAR const char *key,
               pnut_setting_value_t value, uint32_t expect)
{
  uint32_t code = 0;
  bool changed;
  int ret;

  ret = settings_store_set(&f->store, OWNER, key, &value, &changed,
                           &code);
  assert_int_equal(code, expect);
  return ret;
}

static int setup(void **state)
{
  FAR struct fixture_s *f = calloc(1, sizeof(*f));

  assert_int_equal(settings_store_init(&f->store, &g_limits), 0);
  *state = f;
  return 0;
}

static int teardown(void **state)
{
  FAR struct fixture_s *f = *state;

  settings_store_deinit(&f->store);
  free(f);
  return 0;
}

/* Defaults, then values set and read back */

static void test_settings_values(void **state)
{
  FAR struct fixture_s *f = *state;

  usual(f);

  assert_int_equal(get(f, "volume", 0), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 5);
  assert_int_equal(get(f, "name", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "phone");
  assert_int_equal(get(f, "colour", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "green");

  assert_int_equal(set(f, "volume", val_int(7), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "name", val_text("pnut"), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "colour", val_text("blue"), 0), PNUT_STATUS_OK);

  assert_int_equal(get(f, "volume", 0), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 7);
  assert_int_equal(get(f, "name", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "pnut");
  assert_int_equal(get(f, "colour", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "blue");

  /* Changed since encoded */

  assert_string_equal(settings_store_dirty(&f->store, 0), OWNER);
}

/* What a value must fit, each with its code */

static void test_settings_checks(void **state)
{
  FAR struct fixture_s *f = *state;
  uint32_t code = 0;

  usual(f);

  assert_int_equal(set(f, "volume", val_int(11),
                       PNUT_SETTINGS_ERROR_CODE_OUT_OF_RANGE),
                   PNUT_STATUS_INVALID);
  assert_int_equal(set(f, "volume", val_bool(true),
                       PNUT_SETTINGS_ERROR_CODE_WRONG_TYPE),
                   PNUT_STATUS_INVALID);
  assert_int_equal(set(f, "name", val_text("too long a name"),
                       PNUT_SETTINGS_ERROR_CODE_TOO_LONG),
                   PNUT_STATUS_INVALID);
  assert_int_equal(set(f, "colour", val_text("pink"),
                       PNUT_SETTINGS_ERROR_CODE_NOT_A_CHOICE),
                   PNUT_STATUS_INVALID);
  assert_int_equal(set(f, "nothing", val_int(1),
                       PNUT_SETTINGS_ERROR_CODE_UNKNOWN_KEY),
                   PNUT_STATUS_NOTFOUND);
  assert_int_equal(settings_store_get(&f->store, "nobody", "volume", true,
                                      &f->value, &code),
                   PNUT_STATUS_NOTFOUND);
  assert_int_equal(code, PNUT_SETTINGS_ERROR_CODE_UNKNOWN_OWNER);

  /* Nothing changed */

  assert_int_equal(get(f, "volume", 0), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 5);
  assert_null(settings_store_dirty(&f->store, 0));
}

/* A secret can be declared, but not stored yet */

static void test_settings_secret(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR pnut_setting_t *s;

  page(&f->reg, true, true);
  s = declare(&f->reg, "passphrase", PNUT_SETTING_TYPE_SECRET);
  s->max_length = 64;
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(get(f, "passphrase", PNUT_SETTINGS_ERROR_CODE_SECRET),
                   PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(set(f, "passphrase", val_text("x"),
                       PNUT_SETTINGS_ERROR_CODE_SECRET),
                   PNUT_STATUS_UNAVAILABLE);
}

/* A schema in pages, then again, changed: a value the new declaration
 * allows is kept, one whose type changed goes back to its default, and
 * one no longer declared is dropped
 */

static void test_settings_reregister(void **state)
{
  FAR struct fixture_s *f = *state;

  page(&f->reg, true, false);
  declare_int(&f->reg, "a", 0, 10, 1);
  declare_int(&f->reg, "b", 0, 10, 2);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  page(&f->reg, false, true);
  declare_string(&f->reg, "c", 8, "three");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(set(f, "a", val_int(9), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "c", val_text("drei"), 0), PNUT_STATUS_OK);

  page(&f->reg, true, false);
  declare_string(&f->reg, "a", 8, "one");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  page(&f->reg, false, true);
  declare_string(&f->reg, "c", 16, "three");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(get(f, "a", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "one");
  assert_int_equal(get(f, "b", PNUT_SETTINGS_ERROR_CODE_UNKNOWN_KEY),
                   PNUT_STATUS_NOTFOUND);
  assert_int_equal(get(f, "c", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "drei");

  /* The texts are back in the pool: 8, two each for a and c */

  assert_int_equal(f->store.texts.avail, g_limits.texts - 4);
}

/* Schemas that do not hold together, and pages out of order */

static void test_settings_bad_schemas(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR const char *key;
  FAR pnut_setting_t *s;
  uint32_t code;
  bool load;

  page(&f->reg, true, true);
  declare_int(&f->reg, "Volume", 0, 10, 5);
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_NAME),
                   PNUT_STATUS_INVALID);

  page(&f->reg, true, true);
  declare_int(&f->reg, "fine", 0, 10, 5);
  declare_int(&f->reg, "volume", 10, 0, 5);
  assert_int_equal(settings_store_register(&f->store, &f->reg, &load, &key,
                                           &code),
                   PNUT_STATUS_INVALID);
  assert_int_equal(code, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA);
  assert_string_equal(key, "volume");

  page(&f->reg, true, true);
  declare_int(&f->reg, "volume", 0, 10, 11);
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA),
                   PNUT_STATUS_INVALID);

  page(&f->reg, true, true);
  declare_choice(&f->reg, "colour", "pink");
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA),
                   PNUT_STATUS_INVALID);

  page(&f->reg, true, true);
  declare_int(&f->reg, "volume", 0, 10, 5);
  declare_int(&f->reg, "volume", 0, 10, 5);
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA),
                   PNUT_STATUS_INVALID);

  page(&f->reg, true, true);
  s = declare(&f->reg, "passphrase", PNUT_SETTING_TYPE_SECRET);
  s->max_length = 64;
  s->has_default_value = true;
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA),
                   PNUT_STATUS_INVALID);

  page(&f->reg, false, true);
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_NOT_REGISTERING),
                   PNUT_STATUS_INVALID);

  /* Declared on two pages of one schema */

  page(&f->reg, true, false);
  declare_int(&f->reg, "volume", 0, 10, 5);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  page(&f->reg, false, true);
  declare_int(&f->reg, "volume", 0, 10, 5);
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA),
                   PNUT_STATUS_INVALID);
}

/* A page the pools have no room for is refused whole */

static void test_settings_full(void **state)
{
  FAR struct fixture_s *f = *state;

  page(&f->reg, true, true);
  declare_string(&f->reg, "a", 8, "");
  declare_string(&f->reg, "b", 8, "");
  declare_string(&f->reg, "c", 8, "");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  page(&f->reg, true, true);
  declare_string(&f->reg, "a", 8, "");
  declare_string(&f->reg, "d", 8, "");
  declare_string(&f->reg, "e", 8, "");
  assert_int_equal(reg(f, PNUT_SETTINGS_ERROR_CODE_FULL), PNUT_STATUS_BUSY);

  /* The page whole was refused: d is not there, a still is */

  assert_int_equal(get(f, "d", PNUT_SETTINGS_ERROR_CODE_UNKNOWN_KEY),
                   PNUT_STATUS_NOTFOUND);
  assert_int_equal(get(f, "a", 0), PNUT_STATUS_OK);
}

/* Values and schemas in pages, by key */

static void test_settings_pages(void **state)
{
  FAR struct fixture_s *f = *state;
  uint32_t code;
  char key[8];
  int i;

  for (i = 0; i < 10; i++)
    {
      if (i % 3 == 0)
        {
          page(&f->reg, i == 0, false);
        }

      snprintf(key, sizeof(key), "k%d", 9 - i);
      declare_int(&f->reg, key, 0, 100, i);

      if (i % 3 == 2)
        {
          assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
        }
    }

  f->reg.last = true;
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(settings_store_list(&f->store, OWNER, "", true,
                                       &f->values, &code), PNUT_STATUS_OK);
  assert_int_equal(f->values.entries_count, 8);
  assert_true(f->values.more);
  assert_string_equal(f->values.entries[0].key, "k0");
  assert_string_equal(f->values.entries[7].key, "k7");

  assert_int_equal(settings_store_list(&f->store, OWNER, "k7", true,
                                       &f->values, &code), PNUT_STATUS_OK);
  assert_int_equal(f->values.entries_count, 2);
  assert_false(f->values.more);
  assert_string_equal(f->values.entries[1].key, "k9");
  assert_int_equal(f->values.entries[1].value.value.integer, 0);

  assert_int_equal(settings_store_describe(&f->store, OWNER, "", true,
                                           &f->schema, &code),
                   PNUT_STATUS_OK);
  assert_int_equal(f->schema.settings_count, 3);
  assert_true(f->schema.more);
  assert_string_equal(f->schema.settings[2].key, "k2");

  assert_int_equal(settings_store_describe(&f->store, OWNER, "k8", true,
                                           &f->schema, &code),
                   PNUT_STATUS_OK);
  assert_int_equal(f->schema.settings_count, 1);
  assert_false(f->schema.more);
  assert_string_equal(f->schema.settings[0].key, "k9");
  assert_int_equal(f->schema.settings[0].max, 100);
}

/* One setting back to its default, then all of them */

static void test_settings_reset(void **state)
{
  FAR struct fixture_s *f = *state;
  uint32_t code;
  bool changed;

  usual(f);
  assert_int_equal(set(f, "volume", val_int(7), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "name", val_text("pnut"), 0), PNUT_STATUS_OK);

  assert_int_equal(settings_store_reset(&f->store, OWNER, "volume",
                                        &changed, &code), PNUT_STATUS_OK);
  assert_true(changed);
  assert_int_equal(get(f, "volume", 0), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 5);
  assert_int_equal(get(f, "name", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "pnut");

  assert_int_equal(settings_store_reset(&f->store, OWNER, "", &changed,
                                        &code), PNUT_STATUS_OK);
  assert_true(changed);
  assert_int_equal(get(f, "name", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "phone");

  assert_int_equal(settings_store_reset(&f->store, OWNER, "", &changed,
                                        &code), PNUT_STATUS_OK);
  assert_false(changed);
}

/* Values through a file, into a store that registers the same schema */

static void test_settings_file(void **state)
{
  FAR struct fixture_s *f = *state;
  struct settings_store_s other;
  FAR const char *key;
  pb_ostream_t out;
  pb_istream_t in;
  uint32_t code;
  bool load;

  usual(f);
  assert_int_equal(set(f, "volume", val_int(7), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "colour", val_text("blue"), 0), PNUT_STATUS_OK);

  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, OWNER, &out));
  assert_null(settings_store_dirty(&f->store, 0));

  /* The schema has changed meanwhile: name is gone */

  assert_int_equal(settings_store_init(&other, &g_limits), 0);
  page(&f->reg, true, true);
  declare_int(&f->reg, "volume", 0, 10, 5);
  declare_choice(&f->reg, "colour", "green");
  assert_int_equal(settings_store_register(&other, &f->reg, &load, &key,
                                           &code),
                   PNUT_STATUS_OK);
  assert_true(load);

  in = pb_istream_from_buffer(f->file, out.bytes_written);
  assert_true(settings_store_decode(&other, OWNER, &in));

  assert_int_equal(settings_store_get(&other, OWNER, "volume", true,
                                      &f->value, &code), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 7);
  assert_int_equal(settings_store_get(&other, OWNER, "colour", true,
                                      &f->value, &code), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "blue");

  /* Its file holds a value no longer declared: written again */

  assert_string_equal(settings_store_dirty(&other, 0), OWNER);

  /* Registered again, it is not read again */

  assert_int_equal(settings_store_register(&other, &f->reg, &load, &key,
                                           &code),
                   PNUT_STATUS_OK);
  assert_false(load);

  settings_store_deinit(&other);
}

/* A choice is kept by its name when the choices change, and goes back to
 * the default once its name is gone
 */

static void test_settings_choice_kept(void **state)
{
  FAR struct fixture_s *f = *state;
  FAR pnut_setting_t *s;

  page(&f->reg, true, true);
  declare_choice(&f->reg, "colour", "green");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "colour", val_text("blue"), 0), PNUT_STATUS_OK);

  page(&f->reg, true, true);
  s = declare(&f->reg, "colour", PNUT_SETTING_TYPE_CHOICE);
  strcpy(s->choices[s->choices_count++], "yellow");
  strcpy(s->choices[s->choices_count++], "blue");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  assert_int_equal(get(f, "colour", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "blue");

  page(&f->reg, true, true);
  s = declare(&f->reg, "colour", PNUT_SETTING_TYPE_CHOICE);
  strcpy(s->choices[s->choices_count++], "yellow");
  strcpy(s->choices[s->choices_count++], "pink");
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  assert_int_equal(get(f, "colour", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "yellow");

  /* Changed while loaded: its file is written again */

  assert_string_equal(settings_store_dirty(&f->store, 0), OWNER);
}

/* A file cut short: what was read is kept */

static void test_settings_cut_file(void **state)
{
  FAR struct fixture_s *f = *state;
  pb_ostream_t out;
  pb_istream_t in;

  usual(f);
  assert_int_equal(set(f, "colour", val_text("blue"), 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "volume", val_int(9), 0), PNUT_STATUS_OK);

  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, OWNER, &out));

  /* Back to the defaults, then the file without its last bytes: colour
   * comes first, by key, and volume last
   */

  assert_int_equal(settings_store_reset(&f->store, OWNER, "", &f->load,
                                        &f->code), PNUT_STATUS_OK);
  in = pb_istream_from_buffer(f->file, out.bytes_written - 2);
  assert_false(settings_store_decode(&f->store, OWNER, &in));

  assert_int_equal(get(f, "colour", 0), PNUT_STATUS_OK);
  assert_string_equal(f->value.value.text, "blue");
  assert_int_equal(get(f, "volume", 0), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 5);
}

/* Owners take turns to be written: one whose writes fail steps aside,
 * and is tried again later and later
 */

static void test_settings_turns(void **state)
{
  FAR struct fixture_s *f = *state;
  pb_ostream_t out;

  usual(f);
  page(&f->reg, true, true);
  strcpy(f->reg.owner, "other.owner");
  declare_int(&f->reg, "volume", 0, 10, 5);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(set(f, "volume", val_int(7), 0), PNUT_STATUS_OK);
  settings_store_touch(&f->store, "other.owner");
  assert_int_equal(settings_store_due(&f->store, 0), 0);

  /* Asking again moves no turn */

  assert_string_equal(settings_store_dirty(&f->store, 0), OWNER);
  assert_string_equal(settings_store_dirty(&f->store, 0), OWNER);

  /* The first owner's write fails: the other's turn, and the first may be
   * tried after 1000 ms
   */

  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, OWNER, &out));
  assert_int_equal(settings_store_failed(&f->store, OWNER, 0, 1000), 1);
  assert_string_equal(settings_store_dirty(&f->store, 0), "other.owner");

  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, "other.owner", &out));
  settings_store_written(&f->store, "other.owner");
  assert_null(settings_store_dirty(&f->store, 0));
  assert_int_equal(settings_store_due(&f->store, 0), 1000);

  /* Its second failure in a row waits twice as long */

  assert_string_equal(settings_store_dirty(&f->store, 1000), OWNER);
  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, OWNER, &out));
  assert_int_equal(settings_store_failed(&f->store, OWNER, 1000, 1000), 2);
  assert_int_equal(settings_store_due(&f->store, 1000), 2000);

  /* Written at last: nothing is due */

  out = pb_ostream_from_buffer(f->file, sizeof(f->file));
  assert_true(settings_store_encode(&f->store, OWNER, &out));
  settings_store_written(&f->store, OWNER);
  assert_int_equal(settings_store_due(&f->store, 3000), -1);
}

/* Nothing but its schema before the first is whole, and its file read:
 * a change then would be written over the values in its file.  An owner
 * held is never written.
 */

static void test_settings_before_load(void **state)
{
  FAR struct fixture_s *f = *state;

  page(&f->reg, true, false);
  declare_int(&f->reg, "volume", 0, 10, 5);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(set(f, "volume", val_int(7),
                       PNUT_SETTINGS_ERROR_CODE_REGISTERING),
                   PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(get(f, "volume", PNUT_SETTINGS_ERROR_CODE_REGISTERING),
                   PNUT_STATUS_UNAVAILABLE);
  settings_store_touch(&f->store, OWNER);
  assert_null(settings_store_dirty(&f->store, 0));
  assert_int_equal(settings_store_due(&f->store, 0), -1);

  page(&f->reg, false, true);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);
  assert_int_equal(set(f, "volume", val_int(7), 0), PNUT_STATUS_OK);
  assert_string_equal(settings_store_dirty(&f->store, 0), OWNER);

  settings_store_hold(&f->store, OWNER);
  assert_null(settings_store_dirty(&f->store, UINT64_MAX));
  assert_int_equal(settings_store_due(&f->store, 0), -1);
}

/* The service, through a client: a schema, a value set and read back, an
 * error's detail; then the module stops, writing its files, and a new
 * one reads the value back
 */

static void service_stop(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, 0);
}

static void service_next(FAR struct service_s *s);

static void service_done(FAR struct service_s *s, int status,
                         FAR const pnut_error_t *err)
{
  s->status = status;
  s->code   = err != NULL ? err->code : 0;
  s->step++;
  service_next(s);
}

static void service_registered(FAR struct pnut_settings_client_s *client,
                               int status,
                               FAR const pnut_settings_reply_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  service_done(arg, status, err);
}

static void service_got(FAR struct pnut_settings_client_s *client,
                        int status, FAR const pnut_setting_value_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct service_s *s = arg;

  if (out != NULL)
    {
      s->value = *out;
    }

  service_done(s, status, err);
}

static void service_set_done(FAR struct pnut_settings_client_s *client,
                             int status,
                             FAR const pnut_settings_reply_t *out,
                             FAR const pnut_error_t *err, FAR void *arg)
{
  service_done(arg, status, err);
}

/* The steps: register, then Get, Set and Get again, or only Get */

static void service_next(FAR struct service_s *s)
{
  pnut_settings_get_request_t get;
  int ret;
  pnut_settings_set_request_t set;

  memset(&get, 0, sizeof(get));
  strcpy(get.owner, OWNER);
  strcpy(get.key, "volume");

  switch (s->step)
    {
      case 0:
        page(&s->reg, true, true);
        declare_int(&s->reg, "volume", 0, 10, 5);
        assert_int_equal(pnut_settings_register_schema(&s->client, &s->reg,
                                                       0, service_registered,
                                                       s), 0);
        break;

      case 1:
        assert_int_equal(s->status, PNUT_STATUS_OK);
        if (s->two)
          {
            /* The other owner, both values changed, then only time */

            page(&s->reg, true, true);
            strcpy(s->reg.owner, "other.owner");
            declare_int(&s->reg, "volume", 0, 10, 5);
            s->step = 10;
            ret = pnut_settings_register_schema(&s->client, &s->reg, 0,
                                                service_registered, s);
            assert_int_equal(ret, 0);
            break;
          }

        if (s->reread)
          {
            s->step = 3;
            assert_int_equal(pnut_settings_get(&s->client, &get, 0,
                                               service_got, s), 0);
            break;
          }

        strcpy(get.key, "nothing");
        assert_int_equal(pnut_settings_get(&s->client, &get, 0,
                                           service_got, s), 0);
        break;

      case 2:
        assert_int_equal(s->status, PNUT_STATUS_NOTFOUND);
        assert_int_equal(s->code, PNUT_SETTINGS_ERROR_CODE_UNKNOWN_KEY);
        memset(&set, 0, sizeof(set));
        strcpy(set.owner, OWNER);
        strcpy(set.key, "volume");
        set.has_value = true;
        set.value     = val_int(8);
        assert_int_equal(pnut_settings_set(&s->client, &set, 0,
                                           service_set_done, s), 0);
        break;

      case 3:
        assert_int_equal(s->status, PNUT_STATUS_OK);
        assert_int_equal(pnut_settings_get(&s->client, &get, 0,
                                           service_got, s), 0);
        break;

      case 11:
      case 12:
        assert_int_equal(s->status, PNUT_STATUS_OK);
        memset(&set, 0, sizeof(set));
        strcpy(set.owner, s->step == 11 ? OWNER : "other.owner");
        strcpy(set.key, "volume");
        set.has_value = true;
        set.value     = val_int(3);
        assert_int_equal(pnut_settings_set(&s->client, &set, 0,
                                           service_set_done, s), 0);
        break;

      case 13:
        assert_int_equal(s->status, PNUT_STATUS_OK);
        break;

      default:

        /* A moment for the last change to be announced */

        pnut_timer_start(s->loop, 50, 0, service_stop, NULL, NULL);
        break;
    }
}

static void service_change(FAR struct pnut_settings_change_reader_s *reader,
                           FAR const pnut_settings_change_t *msg,
                           FAR void *arg)
{
  FAR struct service_s *s = arg;

  if (s->nchanges < 16 && strcmp(msg->owner, OWNER) == 0)
    {
      strcpy(s->keys[s->nchanges], msg->key);
      s->versions[s->nchanges++] = msg->version;
    }
}

static void service_state(FAR struct pnut_settings_client_s *client,
                          bool up, FAR void *arg)
{
  if (up)
    {
      service_next(arg);
    }
}

/* Run a Settings module and a client until the steps are done */

static void service_run(FAR struct service_s *s, bool reread)
{
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;

  pnut_loop_defaults(&config);
  config.rundir = s->dir;
  assert_int_equal(pnut_loop_create(&config, &s->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = s->dir;
  sconfig.limits = g_limits;
  sconfig.delay  = 20;
  settings_init(&s->settings, &sconfig);
  assert_int_equal(pnut_module_add(s->loop, &s->settings.module), 0);

  s->step     = 0;
  s->reread   = reread;
  s->nchanges = 0;
  assert_int_equal(pnut_settings_change_subscribe(s->loop, service_change,
                                                  s, &s->changes), 0);
  assert_int_equal(pnut_settings_connect(s->loop, SETTINGS_SERVICE,
                                         service_state, s, &s->client), 0);
  pnut_timer_start(s->loop, s->two ? 500 : 2000, 0, service_stop, NULL,
                   NULL);
  assert_int_equal(pnut_loop_run(s->loop), 0);

  pnut_settings_disconnect(&s->client);
  pnut_settings_change_unsubscribe(&s->changes);
  pnut_loop_destroy(s->loop);
  settings_deinit(&s->settings);
}

/* Remove a test's directory, with the settings topic's, on the computer */

static void service_clean(FAR struct service_s *s)
{
  char path[160];

  snprintf(path, sizeof(path), "%s/topic.settings/last", s->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.settings", s->dir);
  rmdir(path);
  rmdir(s->dir);
}

static void test_settings_service(void **state)
{
  FAR struct service_s *s = calloc(1, sizeof(*s));
  char path[128];
  char temp[160];
  struct stat st;

  snprintf(s->dir, sizeof(s->dir), "/tmp/pnut-settings-%d", getpid());
  mkdir(s->dir, 0700);

  service_run(s, false);
  assert_int_equal(s->step, 4);
  assert_int_equal(s->status, PNUT_STATUS_OK);
  assert_int_equal(s->value.value.integer, 8);

  /* Announced: the schema, then the value set, one version after; on
   * NuttX the topic's latest, from before, may come first
   */

  assert_true(s->nchanges >= 2);
  assert_string_equal(s->keys[s->nchanges - 2], "");
  assert_string_equal(s->keys[s->nchanges - 1], "volume");
  assert_int_equal(s->versions[s->nchanges - 1],
                   s->versions[s->nchanges - 2] + 1);

  /* Written as the module stopped, at the latest */

  snprintf(path, sizeof(path), "%s/%s.pb", s->dir, OWNER);
  assert_int_equal(stat(path, &st), 0);

  /* A new module: registered again, the value is read back */

  memset(&s->value, 0, sizeof(s->value));
  service_run(s, true);
  assert_int_equal(s->step, 4);
  assert_int_equal(s->value.value.integer, 8);

  /* Stopped while the new file was being put in the old one's place: it
   * is found under its temporary name, and put there
   */

  snprintf(temp, sizeof(temp), "%s.new", path);
  assert_int_equal(rename(path, temp), 0);
  memset(&s->value, 0, sizeof(s->value));
  service_run(s, true);
  assert_int_equal(s->value.value.integer, 8);
  assert_int_equal(stat(path, &st), 0);

  unlink(path);
  service_clean(s);
  free(s);
}

/* An owner whose file can never be written, since a directory is in the
 * way of the one written beside it, keeps no other owner waiting
 */

static void test_settings_service_starve(void **state)
{
  FAR struct service_s *s = calloc(1, sizeof(*s));
  char blocked[160];
  char path[128];
  struct stat st;

  snprintf(s->dir, sizeof(s->dir), "/tmp/pnut-settings-%d", getpid());
  mkdir(s->dir, 0700);
  snprintf(blocked, sizeof(blocked), "%s/%s.pb.new", s->dir, OWNER);
  assert_int_equal(mkdir(blocked, 0700), 0);

  s->two = true;
  service_run(s, false);
  assert_int_equal(s->step, 13);

  snprintf(path, sizeof(path), "%s/other.owner.pb", s->dir);
  assert_int_equal(stat(path, &st), 0);
  unlink(path);

  snprintf(path, sizeof(path), "%s/%s.pb", s->dir, OWNER);
  assert_int_not_equal(stat(path, &st), 0);

  rmdir(blocked);
  service_clean(s);
  free(s);
}

/* A file that is there but cannot be read is never written over: here a
 * directory stands at its name
 */

static void test_settings_service_unreadable(void **state)
{
  FAR struct service_s *s = calloc(1, sizeof(*s));
  char path[128];
  struct stat st;

  snprintf(s->dir, sizeof(s->dir), "/tmp/pnut-settings-%d", getpid());
  mkdir(s->dir, 0700);
  snprintf(path, sizeof(path), "%s/%s.pb", s->dir, OWNER);
  assert_int_equal(mkdir(path, 0700), 0);

  /* Set and read back, in memory */

  service_run(s, false);
  assert_int_equal(s->step, 4);
  assert_int_equal(s->value.value.integer, 8);

  assert_int_equal(stat(path, &st), 0);
  assert_true(S_ISDIR(st.st_mode));

  rmdir(path);
  service_clean(s);
  free(s);
}

/* A file that does not decode is kept aside, and written again */

static void test_settings_service_bad_file(void **state)
{
  FAR struct service_s *s = calloc(1, sizeof(*s));
  static const uint8_t junk[] =
  {
    0x0a, 0x7f, 'x'               /* An entry 127 bytes long: cut short */
  };

  char path[128];
  char bad[160];
  uint8_t back[sizeof(junk)];
  struct stat st;
  FILE *file;

  snprintf(s->dir, sizeof(s->dir), "/tmp/pnut-settings-%d", getpid());
  mkdir(s->dir, 0700);
  snprintf(path, sizeof(path), "%s/%s.pb", s->dir, OWNER);
  snprintf(bad, sizeof(bad), "%s.bad", path);

  file = fopen(path, "wb");
  assert_non_null(file);
  assert_int_equal(fwrite(junk, 1, sizeof(junk), file), sizeof(junk));
  fclose(file);

  service_run(s, true);
  assert_int_equal(s->value.value.integer, 5);

  file = fopen(bad, "rb");
  assert_non_null(file);
  assert_int_equal(fread(back, 1, sizeof(back), file), sizeof(junk));
  fclose(file);
  assert_memory_equal(back, junk, sizeof(junk));
  assert_int_equal(stat(path, &st), 0);

  unlink(bad);
  unlink(path);
  service_clean(s);
  free(s);
}

/* What a caller who is neither the owner nor the system UI sees: the
 * settings marked public, and no other
 */

static void test_settings_public(void **state)
{
  FAR struct fixture_s *f = *state;
  uint32_t code;

  page(&f->reg, true, true);
  declare_int(&f->reg, "region", 0, 10, 1);
  f->reg.settings[0].is_public = true;
  declare_int(&f->reg, "secret", 0, 10, 2);
  assert_int_equal(reg(f, 0), PNUT_STATUS_OK);

  assert_int_equal(settings_store_get(&f->store, OWNER, "region", false,
                                      &f->value, &code), PNUT_STATUS_OK);
  assert_int_equal(f->value.value.integer, 1);
  assert_int_equal(settings_store_get(&f->store, OWNER, "secret", false,
                                      &f->value, &code),
                   PNUT_STATUS_DENIED);
  assert_int_equal(code, PNUT_SETTINGS_ERROR_CODE_DENIED);

  assert_int_equal(settings_store_list(&f->store, OWNER, "", false,
                                       &f->values, &code), PNUT_STATUS_OK);
  assert_int_equal(f->values.entries_count, 1);
  assert_string_equal(f->values.entries[0].key, "region");

  assert_int_equal(settings_store_describe(&f->store, OWNER, "", false,
                                           &f->schema, &code),
                   PNUT_STATUS_OK);
  assert_int_equal(f->schema.settings_count, 1);
  assert_string_equal(f->schema.settings[0].key, "region");
}

/* Who may do what, through a client (RFC 0025): the program that runs
 * wifi, a caller Settings does not know, the system UI, and a caller who
 * cannot be told now
 */

struct access_s
{
  FAR struct pnut_loop_s *loop;
  struct settings_s settings;
  struct pnut_settings_client_s client;
  pnut_settings_register_request_t reg;
  char dir[64];
  FAR const char *me;             /* Who calls: NULL for a stranger */
  bool unknown;                   /* Who calls cannot be told */
  int step;
  int status[16];
  uint32_t code[16];
  int64_t value;
  int listed;
};

static int access_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  FAR struct access_s *a = arg;

  assert_int_equal(pid, getpid());
  *programp = a->me;
  return a->unknown ? -EAGAIN : a->me != NULL ? OK : -ESRCH;
}

static bool access_runs(FAR void *arg, FAR const char *program,
                        FAR const char *service)
{
  return (strcmp(program, "radios") == 0 && strcmp(service, "wifi") == 0) ||
         (strcmp(program, "ui") == 0 && strcmp(service, "ui") == 0);
}

static void access_next(FAR struct access_s *a);

static void access_record(FAR struct access_s *a, int status,
                          FAR const pnut_error_t *err)
{
  a->status[a->step] = status;
  a->code[a->step]   = err != NULL ? err->code : 0;
  a->step++;
  access_next(a);
}

static void access_replied(FAR struct pnut_settings_client_s *client,
                           int status, FAR const pnut_settings_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  access_record(arg, status, err);
}

static void access_got(FAR struct pnut_settings_client_s *client,
                       int status, FAR const pnut_setting_value_t *out,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct access_s *a = arg;

  if (out != NULL)
    {
      a->value = out->value.integer;
    }

  access_record(a, status, err);
}

static void access_listed(FAR struct pnut_settings_client_s *client,
                          int status, FAR const pnut_settings_values_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct access_s *a = arg;

  a->listed = out != NULL ? out->entries_count : -1;
  access_record(a, status, err);
}

static void access_register(FAR struct access_s *a, FAR const char *owner)
{
  page(&a->reg, true, true);
  strcpy(a->reg.owner, owner);
  declare_int(&a->reg, "power", 0, 20, 10);
  declare_int(&a->reg, "region", 0, 10, 1);
  a->reg.settings[1].is_public = true;
  assert_int_equal(pnut_settings_register_schema(&a->client, &a->reg, 0,
                                                 access_replied, a), 0);
}

static void access_set(FAR struct access_s *a, FAR const char *key,
                       int64_t value)
{
  pnut_settings_set_request_t set;

  memset(&set, 0, sizeof(set));
  strcpy(set.owner, "wifi");
  strcpy(set.key, key);
  set.has_value = true;
  set.value     = val_int(value);
  assert_int_equal(pnut_settings_set(&a->client, &set, 0, access_replied,
                                     a), 0);
}

static void access_get(FAR struct access_s *a, FAR const char *key)
{
  pnut_settings_get_request_t get;

  memset(&get, 0, sizeof(get));
  strcpy(get.owner, "wifi");
  strcpy(get.key, key);
  assert_int_equal(pnut_settings_get(&a->client, &get, 0, access_got, a),
                   0);
}

static void access_next(FAR struct access_s *a)
{
  pnut_settings_page_request_t list;

  switch (a->step)
    {
      case 0:
        a->me = "radios";
        access_register(a, "wifi");
        break;

      case 1:
        access_register(a, "telephony");
        break;

      case 2:
        access_set(a, "power", 7);
        break;

      case 3:
        a->me = NULL;
        access_get(a, "power");
        break;

      case 4:
        access_get(a, "region");
        break;

      case 5:
        access_set(a, "region", 2);
        break;

      case 6:
        memset(&list, 0, sizeof(list));
        strcpy(list.owner, "wifi");
        assert_int_equal(pnut_settings_list(&a->client, &list, 0,
                                            access_listed, a), 0);
        break;

      case 7:
        a->me = "ui";
        access_set(a, "power", 9);
        break;

      case 8:
        access_register(a, "wifi");
        break;

      case 9:
        access_get(a, "power");
        break;

      case 10:
        access_register(a, "ui");
        break;

      case 11:
        a->unknown = true;
        access_get(a, "region");
        break;

      case 12:
        access_set(a, "power", 3);
        break;

      case 13:
        access_register(a, "wifi");
        break;

      default:
        pnut_loop_stop(a->loop, 0);
        break;
    }
}

static void access_state(FAR struct pnut_settings_client_s *client,
                         bool up, FAR void *arg)
{
  if (up)
    {
      access_next(arg);
    }
}

static void test_settings_access(void **state)
{
  FAR struct access_s *a = calloc(1, sizeof(*a));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  char path[160];

  snprintf(a->dir, sizeof(a->dir), "/tmp/pnut-access-%d", getpid());
  mkdir(a->dir, 0700);

  pnut_loop_defaults(&config);
  config.rundir  = a->dir;
  config.initctl = NULL;
  assert_int_equal(pnut_loop_create(&config, &a->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir      = a->dir;
  sconfig.limits   = g_limits;
  sconfig.delay    = 20;
  sconfig.who      = access_who;
  sconfig.runs     = access_runs;
  sconfig.identity = a;
  settings_init(&a->settings, &sconfig);
  assert_int_equal(pnut_module_add(a->loop, &a->settings.module), 0);
  assert_int_equal(pnut_settings_connect(a->loop, SETTINGS_SERVICE,
                                         access_state, a, &a->client), 0);
  pnut_timer_start(a->loop, 2000, 0, service_stop, NULL, NULL);
  assert_int_equal(pnut_loop_run(a->loop), 0);

  pnut_settings_disconnect(&a->client);
  pnut_loop_destroy(a->loop);
  settings_deinit(&a->settings);

  assert_int_equal(a->step, 14);

  /* The program that runs wifi: its schema and values, no other's */

  assert_int_equal(a->status[0], PNUT_STATUS_OK);
  assert_int_equal(a->status[1], PNUT_STATUS_DENIED);
  assert_int_equal(a->code[1], PNUT_SETTINGS_ERROR_CODE_DENIED);
  assert_int_equal(a->status[2], PNUT_STATUS_OK);

  /* A stranger: the public setting, read only */

  assert_int_equal(a->status[3], PNUT_STATUS_DENIED);
  assert_int_equal(a->status[4], PNUT_STATUS_OK);
  assert_int_equal(a->status[5], PNUT_STATUS_DENIED);
  assert_int_equal(a->status[6], PNUT_STATUS_OK);
  assert_int_equal(a->listed, 1);

  /* The system UI: every value, but no schema of its own making */

  assert_int_equal(a->status[7], PNUT_STATUS_OK);
  assert_int_equal(a->status[8], PNUT_STATUS_DENIED);
  assert_int_equal(a->status[9], PNUT_STATUS_OK);
  assert_int_equal(a->value, 9);

  /* Its own settings it owns, as any program its services' */

  assert_int_equal(a->status[10], PNUT_STATUS_OK);

  /* A caller who cannot be told now: to be tried again */

  assert_int_equal(a->status[11], PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(a->code[11], PNUT_SETTINGS_ERROR_CODE_CALLER_UNKNOWN);
  assert_int_equal(a->status[12], PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(a->status[13], PNUT_STATUS_UNAVAILABLE);

  snprintf(path, sizeof(path), "%s/wifi.pb", a->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/ui.pb", a->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.settings/last", a->dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.settings", a->dir);
  rmdir(path);
  rmdir(a->dir);
  free(a);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_settings_values, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_checks, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_secret, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_reregister, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_settings_bad_schemas, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_settings_full, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_pages, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_public, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_reset, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_file, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_choice_kept, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_settings_cut_file, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_settings_turns, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_before_load, setup,
                                    teardown),
    cmocka_unit_test(test_settings_service),
    cmocka_unit_test(test_settings_access),
    cmocka_unit_test(test_settings_service_starve),
    cmocka_unit_test(test_settings_service_unreadable),
    cmocka_unit_test(test_settings_service_bad_file),
  };

  return cmocka_run_group_tests_name("settings", tests, NULL, NULL);
}
