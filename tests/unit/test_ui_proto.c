/****************************************************************************
 * pnut-os/tests/unit/test_ui_proto.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The apps' interface's messages (RFC 0037): every string at its limit,
 * the most rows and options, encode to exactly the most nanopb gives, and
 * that fits one message (RFC 0023); events come back as they went.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <pnut/msg.h>

#include "pnut/manifest.pb.h"
#include "pnut/ui.pb.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A string field filled to its limit: max_size bytes, the NUL included */

#define FILL(field, c) \
  do \
    { \
      memset((field), (c), sizeof(field) - 1); \
      (field)[sizeof(field) - 1] = '\0'; \
    } \
  while (0)

static void fill_row(FAR pnut_ui_row_t *row, uint32_t id)
{
  row->id = 0xffffffff - id;
  FILL(row->lead, 'l');
  row->initials = true;
  FILL(row->title, 't');
  FILL(row->detail, 'd');
  row->which_value = PNUT_UI_ROW_TEXT_TAG;
  FILL(row->value.text, 'v');
  row->unread  = true;
  row->trailer = PNUT_UI_ROW_TRAILER_CHEVRON;
}

static void fill_screen(FAR pnut_ui_show_request_t *in)
{
  int i;

  in->screen = 0xffffffff;
  FILL(in->title, 'T');
  in->sheet = true;
  in->focus = 0xffffffff;
  FILL(in->middle, 'M');
  in->middle_off = true;

  in->options_count = sizeof(in->options) / sizeof(in->options[0]);
  for (i = 0; i < in->options_count; i++)
    {
      in->options[i].id = 0xffffffff - i;
      FILL(in->options[i].label, 'o');
      FILL(in->options[i].letter, 'X');
    }
}

/* Whether a message encodes into one message's payload; its size */

static size_t encoded(FAR const pb_msgdesc_t *fields, FAR const void *msg)
{
  static uint8_t buf[PNUT_MSG_PAYLOAD_MAX];
  pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));

  assert_true(pb_encode(&out, fields, msg));
  return out.bytes_written;
}

/****************************************************************************
 * Name: test_show_fits
 *
 * Description:
 *   A List's Show at every limit, twenty rows and every option, fits one
 *   message (RFC 0023, RFC 0037): twenty rows a message.
 ****************************************************************************/

static void test_show_fits(void **state)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  fill_screen(&in);

  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 0xffffffff;
  in.content.list.rows_count = 20;
  assert_int_equal(sizeof(in.content.list.rows) /
                   sizeof(in.content.list.rows[0]), 20);
  for (i = 0; i < 20; i++)
    {
      fill_row(&in.content.list.rows[i], i);
    }

  assert_int_equal(encoded(PNUT_UI_SHOW_REQUEST_FIELDS, &in),
                   PNUT_UI_SHOW_REQUEST_SIZE);
  assert_true(PNUT_UI_SHOW_REQUEST_SIZE <= PNUT_MSG_PAYLOAD_MAX);
}

/****************************************************************************
 * Name: test_rows_fits
 ****************************************************************************/

static void test_rows_fits(void **state)
{
  static pnut_ui_rows_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = 0xffffffff;
  in.first  = 0xffffffff;
  in.rows_count = 20;
  assert_int_equal(sizeof(in.rows) / sizeof(in.rows[0]), 20);
  for (i = 0; i < 20; i++)
    {
      fill_row(&in.rows[i], i);
    }

  assert_int_equal(encoded(PNUT_UI_ROWS_REQUEST_FIELDS, &in),
                   PNUT_UI_ROWS_REQUEST_SIZE);
  assert_true(PNUT_UI_ROWS_REQUEST_SIZE <= PNUT_MSG_PAYLOAD_MAX);
}

/****************************************************************************
 * Name: test_detail_fits
 *
 * Description:
 *   A Detail's Show at every limit, twelve items, fits; so do Confirm,
 *   Empty and Busy.
 ****************************************************************************/

static void test_detail_fits(void **state)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  fill_screen(&in);

  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 12;
  assert_int_equal(sizeof(in.content.detail.items) /
                   sizeof(in.content.detail.items[0]), 12);
  for (i = 0; i < 12; i++)
    {
      in.content.detail.items[i].id = 0xffffffff - i;
      FILL(in.content.detail.items[i].label, 'l');
      in.content.detail.items[i].which_value = PNUT_UI_ITEM_TEXT_TAG;
      FILL(in.content.detail.items[i].value.text, 'v');
      FILL(in.content.detail.items[i].icon, 'i');
    }

  assert_true(encoded(PNUT_UI_SHOW_REQUEST_FIELDS, &in) <=
              PNUT_UI_SHOW_REQUEST_SIZE);

  memset(&in.content, 0, sizeof(in.content));
  in.which_content = PNUT_UI_SHOW_REQUEST_CONFIRM_TAG;
  FILL(in.content.confirm.question, 'q');
  FILL(in.content.confirm.detail, 'd');
  FILL(in.content.confirm.confirm, 'c');
  assert_true(encoded(PNUT_UI_SHOW_REQUEST_FIELDS, &in) <=
              PNUT_UI_SHOW_REQUEST_SIZE);

  memset(&in.content, 0, sizeof(in.content));
  in.which_content = PNUT_UI_SHOW_REQUEST_EMPTY_TAG;
  FILL(in.content.empty.heading, 'h');
  FILL(in.content.empty.text, 't');
  FILL(in.content.empty.icon, 'i');
  assert_true(encoded(PNUT_UI_SHOW_REQUEST_FIELDS, &in) <=
              PNUT_UI_SHOW_REQUEST_SIZE);

  memset(&in.content, 0, sizeof(in.content));
  in.which_content = PNUT_UI_SHOW_REQUEST_BUSY_TAG;
  FILL(in.content.busy.heading, 'h');
  FILL(in.content.busy.text, 't');
  in.content.busy.cancel = true;
  assert_true(encoded(PNUT_UI_SHOW_REQUEST_FIELDS, &in) <=
              PNUT_UI_SHOW_REQUEST_SIZE);
}

/****************************************************************************
 * Name: test_event_roundtrip
 *
 * Description:
 *   Each kind of event decodes as it was encoded.
 ****************************************************************************/

static void roundtrip(FAR const pnut_ui_event_t *ev)
{
  static uint8_t buf[PNUT_MSG_PAYLOAD_MAX];
  pnut_ui_event_t back;
  pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));
  pb_istream_t in;

  assert_true(pb_encode(&out, PNUT_UI_EVENT_FIELDS, ev));
  in = pb_istream_from_buffer(buf, out.bytes_written);
  memset(&back, 0, sizeof(back));
  assert_true(pb_decode(&in, PNUT_UI_EVENT_FIELDS, &back));
  assert_memory_equal(&back, ev, sizeof(back));
}

static void test_event_roundtrip(void **state)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_WELCOME_TAG;
  ev.kind.welcome.level = 1;
  ev.kind.welcome.display = PNUT_UI_WELCOME_DISPLAY_EPAPER;
  ev.kind.welcome.rows_per_page = 20;
  ev.kind.welcome.rows_kept = 200;
  ev.kind.welcome.languages_count = 1;
  strcpy(ev.kind.welcome.languages[0], "en");
  ev.kind.welcome.clock24 = true;
  ev.kind.welcome.reason = PNUT_UI_WELCOME_REASON_OPENED;
  roundtrip(&ev);

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_INPUT_TAG;
  ev.kind.input.screen = 7;
  ev.kind.input.action = PNUT_UI_INPUT_ACTION_OPTION_CHOSEN;
  ev.kind.input.row = 3;
  ev.kind.input.option = 4;
  roundtrip(&ev);

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_PAGE_TAG;
  ev.kind.page.screen = 7;
  ev.kind.page.first = 20;
  roundtrip(&ev);

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_STOP_TAG;
  roundtrip(&ev);
}

/****************************************************************************
 * Name: test_manifest_fits
 *
 * Description:
 *   A manifest at every limit fits the most nanopb gives, which is under
 *   the 1 KB the system UI reads.
 ****************************************************************************/

static void test_manifest_fits(void **state)
{
  pnut_app_manifest_t m;
  int i;

  memset(&m, 0, sizeof(m));
  FILL(m.icon, 'i');
  FILL(m.name, 'n');
  m.templates_count = sizeof(m.templates) / sizeof(m.templates[0]);
  for (i = 0; i < m.templates_count; i++)
    {
      m.templates[i] = PNUT_UI_TEMPLATE_UI_TEMPLATE_BUSY;
    }

  /* nanopb's bound counts the packed templates' varints at their widest */

  assert_true(encoded(PNUT_APP_MANIFEST_FIELDS, &m) <=
              PNUT_APP_MANIFEST_SIZE);
  assert_true(PNUT_APP_MANIFEST_SIZE <= 1024);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_show_fits),
    cmocka_unit_test(test_rows_fits),
    cmocka_unit_test(test_detail_fits),
    cmocka_unit_test(test_event_roundtrip),
    cmocka_unit_test(test_manifest_fits),
  };

  return cmocka_run_group_tests_name("ui_proto", tests, NULL, NULL);
}
