/****************************************************************************
 * pnut-os/tests/unit/test_ui_host.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The apps' connection to the system UI (RFC 0037): a fake app and a fake
 * Service states, on one loop with the ui module, which draws into a
 * frame in memory.  The app is this test's own task, so Who is asked
 * about this process's pid.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "fake_app.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NOTES    FAKE_NOTES

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_depth;

/* The host's state, kept by a step while the loop runs: once it stops,
 * the module has stopped, and its apps are gone
 */

static char g_id[64];
static bool g_shell;
static bool g_open;
static bool g_depth_kept;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A Show of a template with made-up content; NONE: no template */

static void show_template(FAR struct app_s *app, uint32_t screen, int tmpl)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = screen;
  strcpy(in.title, "Inbox");
  switch (tmpl)
    {
      case PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST:
        in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
        in.content.list.count = 1;
        in.content.list.rows_count = 1;
        in.content.list.rows[0].id = 1;
        strcpy(in.content.list.rows[0].title, "Shopping");
        break;

      case PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL:
        in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
        in.content.detail.items_count = 1;
        strcpy(in.content.detail.items[0].label, "Note");
        in.content.detail.items[0].which_value = PNUT_UI_ITEM_TEXT_TAG;
        strcpy(in.content.detail.items[0].value.text, "Milk");
        break;

      default:
        break;                    /* No template */
    }

  app_send_show(app, &in);
}

/* An event from the system UI, as a screen would send it */

static int send_back(uint32_t screen)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_INPUT_TAG;
  ev.kind.input.screen = screen;
  ev.kind.input.action = PNUT_UI_INPUT_ACTION_BACK;
  return host_event(&g_ui.host, &ev);
}

static int setup(void **state)
{
  fake_setup(state);
  g_id[0] = '\0';
  g_shell = false;
  g_open = false;
  g_depth_kept = false;
  return 0;
}

static int teardown(void **state)
{
  return fake_teardown(state);
}

static const int g_list[] =
{
  PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST
};

/* Steps shared by the tests */

static void connect_app(void)
{
  app_connect(&g_app);
}

static void open_app(void)
{
  assert_true(g_app.connected);
  app_open(&g_app, NULL, 4);
}

static void open_app_no_credit(void)
{
  assert_true(g_app.connected);
  app_open(&g_app, NULL, 0);
}

static void show_list(void)
{
  show_template(&g_app, 1, PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST);
}

static void keep_depth(void)
{
  g_depth = g_ui.depth;
}

/* Whether the stack is as deep as keep_depth() saw */

static void check_depth(void)
{
  g_depth_kept = g_ui.depth == g_depth;
}

static void keep_app(void)
{
  FAR struct host_app_s *app = host_app(&g_ui.host);

  g_open = app != NULL;
  strlcpy(g_id, app != NULL ? app->id : "", sizeof(g_id));
  g_shell = app != NULL && app->shell;
}

/****************************************************************************
 * Name: test_open_named
 *
 * Description:
 *   A task Service states names, whose manifest exists, is welcomed: the
 *   stream's first item is Welcome.
 ****************************************************************************/

static void test_open_named(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 400, NULL },
  };

  FAR const pnut_ui_welcome_t *w;

  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 3);

  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_whos, 1);
  assert_int_equal(g_app.nevents, 1);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  w = &g_app.events[0].kind.welcome;
  assert_int_equal(w->level, 1);
  assert_int_equal(w->display, PNUT_UI_WELCOME_DISPLAY_EPAPER);
  assert_int_equal(w->rows_per_page, 20);
  assert_int_equal(w->rows_kept, 200);
  assert_int_equal(w->reason, PNUT_UI_WELCOME_REASON_OPENED);
  assert_int_equal(w->languages_count, 1);
  assert_string_equal(w->languages[0], "en");
  assert_true(w->clock24);
}

/****************************************************************************
 * Name: test_open_unknown
 *
 * Description:
 *   A task that is no service's is refused; so is a service without a
 *   manifest, and a system service.
 ****************************************************************************/

static void test_open_unknown(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 400, NULL },
  };

  g_who = WHO_NOTFOUND;
  run(steps, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_DENIED);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_NOT_AN_APP);
  assert_false(g_app.up);

  g_who = WHO_NAMED;
  run(steps, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_DENIED);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_NOT_AN_APP);

  /* A system service: no app, whatever it claims */

  strcpy(g_named, "telephony");
  run(steps, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_DENIED);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_NOT_AN_APP);
}

/****************************************************************************
 * Name: test_open_shell
 *
 * Description:
 *   A program typed in the developer's shell opens under the id it gives,
 *   with every template when it has no manifest, with its manifest's when
 *   it has one; an id that is not one is invalid.
 ****************************************************************************/

static void open_as_demo(void)
{
  app_open(&g_app, "org.pnut.demo", 4);
}

static void open_as_bad(void)
{
  app_open(&g_app, "Notes", 4);
}

static void show_detail(void)
{
  show_template(&g_app, 2, PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL);
}

static void test_open_shell(void **state)
{
  static const struct step_s good[] =
  {
    { 20, connect_app },
    { 60, open_as_demo },
    { 200, show_list },
    { 300, keep_app },
    { 400, NULL },
  };

  static const struct step_s limited[] =
  {
    { 20, connect_app },
    { 60, open_as_demo },
    { 200, show_detail },
    { 400, NULL },
  };

  static const struct step_s bad[] =
  {
    { 20, connect_app },
    { 60, open_as_bad },
    { 400, NULL },
  };

  g_who = WHO_SHELL;
  run(good, 5);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_true(g_open);
  assert_string_equal(g_id, "org.pnut.demo");
  assert_true(g_shell);

  run(bad, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_INVALID);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_BAD_ID);

  /* With a manifest of its own: the templates it lists, no more */

  write_manifest("org.pnut.demo", "notes", 1, g_list);
  run(limited, 4);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.status, PNUT_STATUS_DENIED);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_NOT_LISTED);
}

/****************************************************************************
 * Name: test_open_no_answer
 *
 * Description:
 *   Service states does not answer: Open fails unavailable after
 *   HOST_WHO_MS, never accepted unnamed.
 ****************************************************************************/

static void test_open_no_answer(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 60 + HOST_WHO_MS + 500, NULL },
  };

  write_manifest(NOTES, "notes", 1, g_list);
  g_who = WHO_SILENT;
  run(steps, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_UNAVAILABLE);
  assert_false(g_app.up);
}

/****************************************************************************
 * Name: test_open_second
 *
 * Description:
 *   One app at a time: a second Open while one is open is busy.
 ****************************************************************************/

static void connect_app2(void)
{
  app_connect(&g_app2);
}

static void open_app2(void)
{
  app_open(&g_app2, NULL, 4);
}

static void test_open_second(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 30, connect_app2 },
    { 60, open_app },
    { 200, open_app2 },
    { 400, NULL },
  };

  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 5);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app2.opened, PNUT_STATUS_BUSY);
  assert_int_equal(g_app2.code, PNUT_UI_ERROR_CODE_ANOTHER_APP);
}

/****************************************************************************
 * Name: test_show_denied
 *
 * Description:
 *   A template the manifest does not list is denied; a Show with no
 *   template is invalid; a Show from a connection that has not opened is
 *   denied.  The UI's stack is left as it was.
 ****************************************************************************/

static void show_none(void)
{
  show_template(&g_app, 3, PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE);
}

static void show_list_app2(void)
{
  show_template(&g_app2, 4, PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST);
}

static void test_show_denied(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 30, connect_app2 },
    { 60, open_app },
    { 150, keep_depth },
    { 200, show_detail },
    { 350, check_depth },
    { 400, NULL },
  };

  static const struct step_s none[] =
  {
    { 20, connect_app },
    { 30, connect_app2 },
    { 60, open_app },
    { 200, show_none },
    { 250, show_list_app2 },
    { 450, NULL },
  };

  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 7);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.replies, 1);
  assert_int_equal(g_app.status, PNUT_STATUS_DENIED);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_NOT_LISTED);
  assert_true(g_depth_kept);

  run(none, 6);
  assert_int_equal(g_app.status, PNUT_STATUS_INVALID);
  assert_int_equal(g_app.code, PNUT_UI_ERROR_CODE_UNKNOWN_TEMPLATE);
  assert_int_equal(g_app2.replies, 1);
  assert_int_equal(g_app2.status, PNUT_STATUS_DENIED);
  assert_int_equal(g_app2.code, PNUT_UI_ERROR_CODE_NOT_OPEN);
}

/****************************************************************************
 * Name: test_credit
 *
 * Description:
 *   An app that gives no credit: HOST_EVENTS events wait, Welcome among
 *   them; one more ends its stream, and the app is no longer open.
 *   Credit given before then lets the waiting ones go, in order.
 ****************************************************************************/

static int g_sent[HOST_EVENTS + 2];

static void flood(void)
{
  int i;

  for (i = 0; i < HOST_EVENTS + 1; i++)
    {
      g_sent[i] = send_back(100 + i);
    }

  g_sent[HOST_EVENTS + 1] = send_back(999);
}

static void some(void)
{
  int i;

  for (i = 0; i < 3; i++)
    {
      g_sent[i] = send_back(100 + i);
    }
}

static void give_credit(void)
{
  assert_int_equal(pnut_ui_open_credit(&g_app.client, g_app.stream, 8), 0);
}

static void test_credit(void **state)
{
  static const struct step_s flooded[] =
  {
    { 20, connect_app },
    { 60, open_app_no_credit },
    { 200, flood },
    { 300, keep_app },
    { 400, NULL },
  };

  static const struct step_s paced[] =
  {
    { 20, connect_app },
    { 60, open_app_no_credit },
    { 200, some },
    { 300, give_credit },
    { 450, keep_app },
    { 500, NULL },
  };

  int i;

  write_manifest(NOTES, "notes", 1, g_list);
  run(flooded, 5);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);

  /* Welcome waits first: 15 more fit, the 16th ends the stream */

  for (i = 0; i < HOST_EVENTS - 1; i++)
    {
      assert_int_equal(g_sent[i], 0);
    }

  assert_int_equal(g_sent[HOST_EVENTS - 1], -ENOSPC);
  assert_int_equal(g_sent[HOST_EVENTS], -ENOTCONN);
  assert_int_equal(g_sent[HOST_EVENTS + 1], -ENOTCONN);
  assert_int_equal(g_app.ended, PNUT_STATUS_BUSY);
  assert_false(g_open);

  run(paced, 6);
  assert_int_equal(g_app.nevents, 4);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  for (i = 0; i < 3; i++)
    {
      assert_int_equal(g_app.events[i + 1].kind.input.screen, 100 + i);
    }

  assert_true(g_open);
}

/****************************************************************************
 * Name: test_gone
 *
 * Description:
 *   The app goes with events waiting: nothing is sent after, the system
 *   UI's stack is back as it was, and another app may open.
 ****************************************************************************/

static void disconnect_app(void)
{
  pnut_ui_disconnect(&g_app.client);
}

static void check_gone(void)
{
  assert_null(host_app(&g_ui.host));
  assert_int_equal(send_back(5), -ENOTCONN);
  assert_int_equal(g_ui.depth, g_depth);
}

static void test_gone(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 30, keep_depth },
    { 60, open_app_no_credit },
    { 200, show_list },
    { 250, some },
    { 300, disconnect_app },
    { 400, check_gone },
    { 420, connect_app2 },
    { 460, open_app2 },
    { 700, NULL },
  };

  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 10);
  assert_int_equal(g_app2.opened, PNUT_STATUS_OK);
}

/****************************************************************************
 * Name: test_leave
 *
 * Description:
 *   Leave takes the app off; its stream ends; another Open is accepted.
 ****************************************************************************/

static void leave_app(void)
{
  app_leave(&g_app);
}

static void test_leave(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 30, connect_app2 },
    { 40, keep_depth },
    { 60, open_app },
    { 200, show_list },
    { 300, leave_app },
    { 350, check_depth },
    { 400, open_app2 },
    { 600, NULL },
  };

  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 9);
  assert_int_equal(g_app.replies, 2);       /* Show's, then Leave's */
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_int_equal(g_app.ended, PNUT_STATUS_ENDED);
  assert_int_equal(g_app2.opened, PNUT_STATUS_OK);
  assert_true(g_depth_kept);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_open_named, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_unknown, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_shell, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_no_answer, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_second, setup, teardown),
    cmocka_unit_test_setup_teardown(test_show_denied, setup, teardown),
    cmocka_unit_test_setup_teardown(test_credit, setup, teardown),
    cmocka_unit_test_setup_teardown(test_gone, setup, teardown),
    cmocka_unit_test_setup_teardown(test_leave, setup, teardown),
  };

  return cmocka_run_group_tests_name("ui_host", tests, NULL, NULL);
}
