/****************************************************************************
 * pnut-os/tests/unit/test_ui_app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's screens as the system UI draws them (RFC 0037): a fake app
 * shows each template, the test presses keys and taps, and reads back the
 * events; the screens are drawn to images, compared with tests/screens.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "fake_app.h"

#include "apptpl.h"
#include "blocks.h"
#include "describe.h"
#include "keymap.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define INBOX     7               /* The List's screen id */
#define ROWS      32

#define OPT_CALL    41
#define OPT_DELETE  42
#define OPT_STEER   43

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int g_all[] =
{
  PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST,
  PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL,
  PNUT_UI_TEMPLATE_UI_TEMPLATE_CONFIRM,
  PNUT_UI_TEMPLATE_UI_TEMPLATE_EMPTY,
  PNUT_UI_TEMPLATE_UI_TEMPLATE_BUSY
};

static const char *const g_names[] =
{
  "Ola", "Tomek", "Bank", "Base camp", "Mama", "Kasia", "Piotr", "Ania",
  "Marek", "Zosia", "Office", "Garage"
};

static const char *const g_texts[] =
{
  "Are we still on for Thursday?", "Invoice is in your email",
  "One-time code", "Reached the hut, all fine", "You: I'll call you tonight",
  "You: Photo", "See you at the station", "Thanks!"
};

/* What a step saw, kept while the loop runs */

static char g_top[16];
static char g_title[64];
static char g_page[16];
static uint16_t g_focus;
static int g_from;                /* Events before the keys of a step */
static uint16_t g_focus_before;   /* The focus a first look saw */
static uint8_t g_depth;           /* The stack's depth, before and after */
static uint8_t g_depth_after;
static bool g_drawn_top;          /* LVGL shows the top screen's root */
static bool g_options;
static char g_letters[APP_OPTIONS];
static char g_row_title[APP_TEXT_MAX];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void make_row(FAR pnut_ui_row_t *row, uint32_t i)
{
  memset(row, 0, sizeof(*row));
  row->id = 100 + i;
  row->initials = true;
  strlcpy(row->lead, g_names[i % 12], 3);
  row->lead[1] = row->lead[1] >= 'a' && row->lead[1] <= 'z' ?
                 row->lead[1] - 'a' + 'A' : row->lead[1];
  strlcpy(row->title, g_names[i % 12], sizeof(row->title));
  strlcpy(row->detail, g_texts[i % 8], sizeof(row->detail));
  row->which_value = PNUT_UI_ROW_TEXT_TAG;
  snprintf(row->value.text, sizeof(row->value.text), "%02u:%02u",
           (unsigned)(13 - i / 6), (unsigned)(52 - i % 50));
  row->unread = i < 2;
}

/* The Inbox: ROWS rows, the first 20 of them, three options (the third
 * lettered W, which steers)
 */

static void show_inbox(void)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Inbox");
  in.options_count = 3;
  in.options[0].id = OPT_CALL;
  strcpy(in.options[0].label, "Call");
  strcpy(in.options[0].letter, "c");
  in.options[1].id = OPT_DELETE;
  strcpy(in.options[1].label, "Delete conversation");
  strcpy(in.options[1].letter, "X");
  in.options[2].id = OPT_STEER;
  strcpy(in.options[2].label, "Mark all read");
  strcpy(in.options[2].letter, "w");

  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = ROWS;
  in.content.list.rows_count = 20;
  for (i = 0; i < 20; i++)
    {
      make_row(&in.content.list.rows[i], i);
    }

  app_send_show(&g_app, &in);
}

static void rows_from(uint32_t screen, uint32_t first, int n)
{
  static pnut_ui_rows_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = screen;
  in.first  = first;
  in.rows_count = n;
  for (i = 0; i < n; i++)
    {
      make_row(&in.rows[i], first + i);
    }

  app_send_rows(&g_app, &in);
}

static void connect_app(void)
{
  app_connect(&g_app);
}

static void open_app(void)
{
  app_open(&g_app, NULL, 32);
}

/* What is on top, kept */

static void look(void)
{
  FAR struct screen_s *s = ui_top(&g_ui);
  FAR struct app_scr_s *st;

  strlcpy(g_top, s->cls->name, sizeof(g_top));
  g_title[0] = '\0';
  g_page[0]  = '\0';
  if (s->header != NULL)
    {
      strlcpy(g_title, block_header_text(s->header, BLOCK_HEADER_TITLE),
              sizeof(g_title));
      strlcpy(g_page, block_header_text(s->header, BLOCK_HEADER_PAGE),
              sizeof(g_page));
    }

  g_focus   = s->focus;
  g_options = ui_options_open(&g_ui);
  if (strcmp(g_top, "app") == 0)
    {
      st = s->state;
      memcpy(g_letters, st->letters, sizeof(g_letters));
    }
}

static void mark(void)
{
  g_from = g_app.nevents;
}

static void look_before(void)
{
  look();
  g_focus_before = g_focus;
  g_depth = g_ui.depth;
}

static void look_after(void)
{
  look();
  g_depth_after = g_ui.depth;
  g_drawn_top = lv_screen_active() == ui_top(&g_ui)->root;
}

/****************************************************************************
 * Name: test_list_shown
 *
 * Description:
 *   A List: the app's icon and the title in the header, the page of six,
 *   the first row focused; Back goes to the app, and the screen stays.
 ****************************************************************************/

static void back(void)
{
  key(KEYMAP_BACKSPACE, true);
}

static void img_list(void)
{
  save("app-list");
}

static void test_list_shown(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 400, look },
    { 410, img_list },
    { 420, mark },
    { 430, back },
    { 600, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 8);

  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top, "app");
  assert_string_equal(g_title, "Inbox");
  assert_string_equal(g_page, "1/6");
  assert_int_equal(g_focus, 0);

  assert_int_equal(app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_BACK,
                              &in), 1);
  assert_int_equal(in->screen, INBOX);
}

/****************************************************************************
 * Name: test_list_paging
 *
 * Description:
 *   Down past row 14 asks for rows from 20, once; Rows for another screen,
 *   or from another row, are dropped; Rows from 20 fill the list, so the
 *   focus reaches row 25 and the second page shows.
 ****************************************************************************/

static void down(void)
{
  key('s', false);
}

static void down14(void)
{
  int i;

  for (i = 0; i < 14; i++)
    {
      down();
    }
}

static void down11(void)
{
  int i;

  for (i = 0; i < 11; i++)
    {
      down();
    }
}

static void down6(void)
{
  int i;

  for (i = 0; i < 6; i++)
    {
      down();
    }
}

/* Rows for another screen, and from another row: one message each, a
 * step apart (two of 3 KB fill the connection's send buffer)
 */

static void rows_other_screen(void)
{
  rows_from(INBOX + 1, 20, 12);
}

static void rows_other_row(void)
{
  rows_from(INBOX, 40, 12);
}

static void rows_right(void)
{
  rows_from(INBOX, 20, 12);
}

static void enter(void)
{
  key(KEYMAP_ENTER, true);
}

static void img_list2(void)
{
  save("app-list-2");
}

static void test_list_paging(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, mark },
    { 310, down14 },
    { 400, rows_other_screen },
    { 450, rows_other_row },
    { 500, down11 },
    { 600, look_before },
    { 610, rows_right },
    { 700, down6 },
    { 800, look },
    { 810, img_list2 },
    { 820, enter },
    { 1000, NULL },
  };

  FAR const pnut_ui_page_needed_t *page = NULL;
  FAR const pnut_ui_input_t *in;
  int pages = 0;
  int i;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 15);

  for (i = g_from; i < g_app.nevents; i++)
    {
      if (g_app.events[i].which_kind == PNUT_UI_EVENT_PAGE_TAG)
        {
          page = &g_app.events[i].kind.page;
          pages++;
        }
    }

  assert_int_equal(pages, 1);
  assert_int_equal(page->screen, INBOX);
  assert_int_equal(page->first, 20);

  /* Rows answered: every answer ok, even those dropped */

  assert_int_equal(g_app.status, PNUT_STATUS_OK);

  /* The focus stopped at row 19 until Rows came, then went on to 25, on
   * the fifth page of six
   */

  assert_int_equal(g_focus_before, 19);
  assert_int_equal(g_focus, 25);
  assert_string_equal(g_page, "5/6");
  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_ROW_OPENED, &in), 1);
  assert_int_equal(in->row, 125);
  assert_int_equal(in->screen, INBOX);
}

/****************************************************************************
 * Name: test_list_open
 *
 * Description:
 *   The primary action on the focused row: Enter, E, the middle softkey;
 *   a tap on the third row focuses and opens it.
 ****************************************************************************/

static void e_key(void)
{
  key('e', false);
}

static void middle(void)
{
  key(KEYMAP_F2, true);
}

static void tap_third(void)
{
  tap(120, BLOCK_HEADER_H + 2 * BLOCK_ROW_H + BLOCK_ROW_H / 2);
}

static void test_list_open(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, mark },
    { 310, enter },
    { 400, e_key },
    { 500, middle },
    { 600, tap_third },
    { 700, look },
    { 800, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 10);

  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_ROW_OPENED, &in), 4);
  assert_int_equal(in->row, 102);
  assert_int_equal(g_focus, 2);
  assert_string_equal(g_top, "app");
}

/****************************************************************************
 * Name: test_list_options
 *
 * Description:
 *   The app's options with their letters: one lettered W, which steers,
 *   is drawn without it; C chooses Call, with the focused row.
 ****************************************************************************/

static void options(void)
{
  key(KEYMAP_F1, true);
}

static void letter_c(void)
{
  key('c', false);
}

static void img_options(void)
{
  save("app-list-options");
}

static void test_list_options(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, down },
    { 400, options },
    { 500, look },
    { 510, img_options },
    { 520, mark },
    { 530, letter_c },
    { 700, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 10);

  assert_true(g_options);
  assert_int_equal(g_letters[0], 'c');
  assert_int_equal(g_letters[1], 'x');
  assert_int_equal(g_letters[2], '\0');
  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_OPTION_CHOSEN, &in), 1);
  assert_int_equal(in->option, OPT_CALL);
  assert_int_equal(in->row, 101);
}

/****************************************************************************
 * Name: test_list_text
 *
 * Description:
 *   An app's text that is not valid UTF-8, or holds a control character,
 *   is drawn as describe_name() draws a network's name.
 ****************************************************************************/

static void show_odd(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Odd");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  in.content.list.rows[0].id = 1;
  strcpy(in.content.list.rows[0].title, "A\x01" "B\xff");
  app_send_show(&g_app, &in);
}

static void look_row(void)
{
  FAR struct app_scr_s *st = ui_top(&g_ui)->state;

  strlcpy(g_row_title,
          lv_label_get_text(block_row_title(st->t.list.rows[0])),
          sizeof(g_row_title));
}

static void test_list_text(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_odd },
    { 400, look_row },
    { 500, NULL },
  };

  char want[APP_TEXT_MAX];

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 5);
  describe_name("A\x01" "B\xff", want, sizeof(want));
  assert_string_equal(g_row_title, want);
  assert_string_not_equal(g_row_title, "A\x01" "B\xff");
}

/****************************************************************************
 * Name: test_list_invalid
 *
 * Description:
 *   A List that does not hold together (more rows than its count, a focus
 *   not among them) is invalid and draws nothing; a List as a sheet is
 *   invalid.
 ****************************************************************************/

static void show_bad(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 2;
  in.content.list.rows[0].id = 1;
  in.content.list.rows[1].id = 2;
  app_send_show(&g_app, &in);
}

static void show_sheet_list(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX + 1;
  in.sheet  = true;
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  app_send_show(&g_app, &in);
}

static int g_status1;

static void keep_status(void)
{
  g_status1 = g_app.status;
}

static void test_list_invalid(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_bad },
    { 300, look },
    { 310, keep_status },
    { 320, show_sheet_list },
    { 500, NULL },
  };

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 7);
  assert_int_equal(g_status1, PNUT_STATUS_INVALID);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_app.status, PNUT_STATUS_INVALID);
}

/****************************************************************************
 * Name: test_list_replaced
 *
 * Description:
 *   A second Show replaces the app's screen in its place: the new title,
 *   the stack as deep, Back about the new screen.
 ****************************************************************************/

static void show_archive(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX + 10;
  strcpy(in.title, "Archive");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  make_row(&in.content.list.rows[0], 3);
  app_send_show(&g_app, &in);
}

static void test_list_replaced(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, look_before },
    { 310, show_archive },
    { 400, look_after },
    { 410, mark },
    { 420, back },
    { 600, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 9);
  assert_string_equal(g_title, "Archive");
  assert_int_equal(g_depth_after, g_depth);
  assert_true(g_drawn_top);
  assert_int_equal(app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_BACK,
                              &in), 1);
  assert_int_equal(in->screen, INBOX + 10);
}

/****************************************************************************
 * Name: test_list_updated
 *
 * Description:
 *   Update: the title; a row's title and unread mark; the primary action
 *   off, then Enter sends nothing; an Update of another screen dropped.
 ****************************************************************************/

static void update_title(void)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_change = PNUT_UI_UPDATE_REQUEST_TITLE_TAG;
  strcpy(in.change.title, "Inbox (2)");
  app_send_update(&g_app, &in);
}

static void update_row(void)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_change = PNUT_UI_UPDATE_REQUEST_ROW_TAG;
  make_row(&in.change.row, 1);
  strcpy(in.change.row.title, "Tomek (read)");
  in.change.row.unread = false;
  app_send_update(&g_app, &in);
}

static void update_other(void)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX + 1;
  in.which_change = PNUT_UI_UPDATE_REQUEST_TITLE_TAG;
  strcpy(in.change.title, "Elsewhere");
  app_send_update(&g_app, &in);
}

static void middle_off(void)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_change = PNUT_UI_UPDATE_REQUEST_MIDDLE_TAG;
  in.change.middle.off = true;
  app_send_update(&g_app, &in);
}

static void look_row2(void)
{
  FAR struct app_scr_s *st = ui_top(&g_ui)->state;
  FAR lv_obj_t *title = block_row_title(st->t.list.rows[1]);

  strlcpy(g_row_title, lv_label_get_text(title), sizeof(g_row_title));
  g_options = lv_obj_get_style_text_font(title, 0) == &g_ui_sans_bold_14;
}

static void test_list_updated(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, update_title },
    { 350, update_row },
    { 400, update_other },
    { 450, look_row2 },
    { 460, look_before },
    { 470, middle_off },
    { 550, mark },
    { 560, enter },
    { 700, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 12);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_title, "Inbox (2)");
  assert_string_equal(g_row_title, "Tomek (read)");
  assert_false(g_options);        /* Read, not focused: not bold */
  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_ROW_OPENED, &in), 0);
}

/****************************************************************************
 * Name: test_list_initials
 *
 * Description:
 *   A lead of initials is cut to two characters.
 ****************************************************************************/

static void show_long_initials(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  in.content.list.rows[0].id = 1;
  in.content.list.rows[0].initials = true;
  strcpy(in.content.list.rows[0].lead, "\xc5\x81" "ABCDEF");
  strcpy(in.content.list.rows[0].title, "Łukasz");
  app_send_show(&g_app, &in);
}

static void look_lead(void)
{
  FAR struct app_scr_s *st = ui_top(&g_ui)->state;

  strlcpy(g_row_title, lv_label_get_text(lv_obj_get_child(st->t.list.rows[0],
                                                          0)),
          sizeof(g_row_title));
}

static void test_list_initials(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_long_initials },
    { 300, look_lead },
    { 400, NULL },
  };

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 5);
  assert_string_equal(g_row_title, "\xc5\x81" "A");
}

/****************************************************************************
 * Name: test_list_gone_while_built
 *
 * Description:
 *   A List built with its focus near the end of its rows asks for a page
 *   at once; when that event cannot wait (16 waiting already), the app is
 *   taken off while its screen is built: Home shows, and is drawn.
 ****************************************************************************/

static void open_app_no_credit(void)
{
  app_open(&g_app, NULL, 0);
}

static void fill_queue(void)
{
  pnut_ui_event_t ev;
  int i;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_INPUT_TAG;
  ev.kind.input.action = PNUT_UI_INPUT_ACTION_BACK;
  for (i = 0; i < HOST_EVENTS - 1; i++)
    {
      assert_int_equal(host_event(&g_ui.host, &ev), 0);
    }
}

static void show_inbox_at_end(void)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.focus  = 118;
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = ROWS;
  in.content.list.rows_count = 20;
  for (i = 0; i < 20; i++)
    {
      make_row(&in.content.list.rows[i], i);
    }

  app_send_show(&g_app, &in);
}

static void test_list_gone_while_built(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app_no_credit },
    { 200, fill_queue },
    { 300, show_inbox_at_end },
    { 400, look_after },
    { 500, NULL },
  };

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 6);
  assert_string_equal(g_top, "home");
  assert_true(g_drawn_top);
  assert_int_equal(g_app.ended, PNUT_STATUS_BUSY);
}

/****************************************************************************
 * Name: test_list_under_pin
 *
 * Description:
 *   The SIM PIN screen is the system's (layer 5): an app's first screen
 *   goes beneath it, and the PIN stays on top.
 ****************************************************************************/

static void sim_locked(void)
{
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN);
}

static void test_list_under_pin(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 30, sim_locked },
    { 60, open_app },
    { 200, look_before },
    { 210, show_inbox },
    { 400, look_after },
    { 500, NULL },
  };

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 7);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top, "pin");
  assert_true(g_drawn_top);
  assert_int_equal(g_depth_after, g_depth + 1);
}

/****************************************************************************
 * Name: test_list_gone_with_options
 *
 * Description:
 *   The app goes while its options are open: they close with its screen,
 *   and Home shows.
 ****************************************************************************/

static void disconnect_app(void)
{
  pnut_ui_disconnect(&g_app.client);
}

static void test_list_gone_with_options(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_inbox },
    { 300, options },
    { 400, disconnect_app },
    { 500, look_after },
    { 600, NULL },
  };

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 7);
  assert_string_equal(g_top, "home");
  assert_false(g_options);
  assert_true(g_drawn_top);
}

/****************************************************************************
 * Name: test_detail
 *
 * Description:
 *   A Detail (Cat-Detail): label over value; the items the app numbers
 *   take the focus, the others are skipped; the primary action opens the
 *   one focused; a tap opens the one tapped.
 ****************************************************************************/

#define CONTACT    21

static void show_contact(void)
{
  static pnut_ui_show_request_t in;
  static const char *const labels[5] =
    {
      "Mobile", "Email", "LoRa node", "Birthday", "Last message"
    };

  static const char *const values[5] =
    {
      "601 234 567", "ewa@example.com", "7F3A \xc2\xb7 last heard 2 h ago",
      "14 March", "Mon, 18:20 \xc2\xb7 SMS"
    };

  static const uint32_t ids[5] =
    {
      1, 2, 0, 0, 5
    };

  static const char *const icons[5] =
    {
      "phone", "email", "", "", "messages"
    };

  int i;

  memset(&in, 0, sizeof(in));
  in.screen = CONTACT;
  strcpy(in.title, "Mama");
  strcpy(in.middle, "Call");
  in.options_count = 1;
  in.options[0].id = OPT_CALL;
  strcpy(in.options[0].label, "Send message");
  strcpy(in.options[0].letter, "m");
  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 5;
  for (i = 0; i < 5; i++)
    {
      in.content.detail.items[i].id = ids[i];
      strcpy(in.content.detail.items[i].label, labels[i]);
      in.content.detail.items[i].which_value = PNUT_UI_ITEM_TEXT_TAG;
      strcpy(in.content.detail.items[i].value.text, values[i]);
      strcpy(in.content.detail.items[i].icon, icons[i]);
    }

  app_send_show(&g_app, &in);
}

static void img_detail(void)
{
  save("app-detail");
}

static void down2(void)
{
  down();
  down();
}

static void tap_second(void)
{
  tap(120, BLOCK_HEADER_H + BLOCK_ROW_H + BLOCK_ROW_H / 2);
}

static void tap_fourth(void)
{
  tap(120, BLOCK_HEADER_H + 3 * BLOCK_ROW_H + BLOCK_ROW_H / 2);
}

static void letter_m(void)
{
  key('m', false);
}

static void test_detail(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_contact },
    { 300, look_before },
    { 310, img_detail },
    { 320, mark },
    { 330, enter },
    { 400, down2 },
    { 500, look },
    { 510, enter },
    { 600, tap_second },
    { 650, tap_fourth },
    { 700, options },
    { 750, letter_m },
    { 800, NULL },
  };

  FAR const pnut_ui_input_t *in;
  int n;

  write_manifest(FAKE_NOTES, "contacts", 5, g_all);
  run(steps, 15);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_title, "Mama");
  assert_int_equal(g_focus_before, 1);  /* The first item's block */
  assert_int_equal(g_focus, 5);         /* Down twice: past the two of 0 */

  /* The tap on the fourth item, which is not numbered, sends nothing */

  n = app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_ROW_OPENED, &in);
  assert_int_equal(n, 3);
  assert_int_equal(g_app.events[g_from].kind.input.row, 1);
  assert_int_equal(in->row, 2);         /* The tap on the second, last */

  /* An option, with the item focused: the second, as tapped */

  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_OPTION_CHOSEN, &in), 1);
  assert_int_equal(in->option, OPT_CALL);
  assert_int_equal(in->row, 2);
}

/****************************************************************************
 * Name: test_detail_scrolls
 *
 * Description:
 *   Twelve items, only the first two numbered: down past the second
 *   scrolls the rest into view, a row at a time, though nothing more
 *   takes the focus; the primary action with no item numbered sends
 *   nothing.
 ****************************************************************************/

static bool g_scrolled;

static void show_event(void)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = CONTACT;
  strcpy(in.title, "Event");
  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 12;
  for (i = 0; i < 12; i++)
    {
      in.content.detail.items[i].id = i < 2 ? i + 1 : 0;
      snprintf(in.content.detail.items[i].label,
               sizeof(in.content.detail.items[i].label), "Part %d", i + 1);
      in.content.detail.items[i].which_value = PNUT_UI_ITEM_TEXT_TAG;
      strcpy(in.content.detail.items[i].value.text, "Text");
    }

  app_send_show(&g_app, &in);
}

static void show_event_bare(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = CONTACT + 1;
  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 2;
  strcpy(in.content.detail.items[0].label, "Place");
  strcpy(in.content.detail.items[1].label, "Notes");
  app_send_show(&g_app, &in);
}

static void down10(void)
{
  int i;

  for (i = 0; i < 10; i++)
    {
      down();
    }
}

static void look_scrolled(void)
{
  FAR struct app_scr_s *st = ui_top(&g_ui)->state;

  g_scrolled = lv_obj_get_scroll_bottom(st->t.detail.list.box) <= 0;
}

static void test_detail_scrolls(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_event },
    { 300, down10 },
    { 400, look_scrolled },
    { 410, show_event_bare },
    { 500, mark },
    { 510, enter },
    { 600, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "contacts", 5, g_all);
  run(steps, 9);
  assert_true(g_scrolled);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_int_equal(app_inputs(&g_app, g_from,
                              PNUT_UI_INPUT_ACTION_ROW_OPENED, &in), 0);
  assert_int_equal(app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_MIDDLE,
                              &in), 0);
}

/****************************************************************************
 * Name: test_detail_invalid
 *
 * Description:
 *   Two items of one id, or a focus that is no item's: invalid.
 ****************************************************************************/

static void show_contact_twice(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = CONTACT;
  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 2;
  in.content.detail.items[0].id = 4;
  in.content.detail.items[1].id = 4;
  app_send_show(&g_app, &in);
}

static void show_contact_focus(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = CONTACT;
  in.focus  = 9;
  in.which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in.content.detail.items_count = 1;
  in.content.detail.items[0].id = 4;
  app_send_show(&g_app, &in);
}

static void test_detail_invalid(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_contact_twice },
    { 300, keep_status },
    { 310, show_contact_focus },
    { 500, NULL },
  };

  write_manifest(FAKE_NOTES, "contacts", 5, g_all);
  run(steps, 6);
  assert_int_equal(g_status1, PNUT_STATUS_INVALID);
  assert_int_equal(g_app.status, PNUT_STATUS_INVALID);
}

/****************************************************************************
 * Name: test_empty
 *
 * Description:
 *   An Empty (Cat-Empty): the primary action is the middle action; Back
 *   goes to the app; with the primary action off, Enter sends nothing.
 ****************************************************************************/

#define DRAFTS     31

static void show_drafts(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = DRAFTS;
  strcpy(in.title, "Drafts");
  strcpy(in.middle, "New");
  in.which_content = PNUT_UI_SHOW_REQUEST_EMPTY_TAG;
  strcpy(in.content.empty.heading, "No drafts");
  strcpy(in.content.empty.text, "Messages you leave unsent are kept here.");
  strcpy(in.content.empty.icon, "messages");
  app_send_show(&g_app, &in);
}

static void drafts_off(void)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = DRAFTS;
  in.which_change = PNUT_UI_UPDATE_REQUEST_MIDDLE_TAG;
  in.change.middle.off = true;
  app_send_update(&g_app, &in);
}

static void img_empty(void)
{
  save("app-empty");
}

static void test_empty(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect_app },
    { 60, open_app },
    { 200, show_drafts },
    { 300, look },
    { 310, img_empty },
    { 320, mark },
    { 330, enter },
    { 400, back },
    { 500, drafts_off },
    { 600, enter },
    { 800, NULL },
  };

  FAR const pnut_ui_input_t *in;

  write_manifest(FAKE_NOTES, "messages", 5, g_all);
  run(steps, 11);
  assert_string_equal(g_title, "Drafts");
  assert_int_equal(app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_MIDDLE,
                              &in), 1);
  assert_int_equal(in->screen, DRAFTS);
  assert_int_equal(app_inputs(&g_app, g_from, PNUT_UI_INPUT_ACTION_BACK,
                              &in), 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_list_shown, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_paging, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_open, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_options, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_text, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_invalid, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_replaced, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_updated, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_initials, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_gone_while_built, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_under_pin, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_list_gone_with_options, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_detail, fake_setup, fake_teardown),
    cmocka_unit_test_setup_teardown(test_detail_invalid, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_detail_scrolls, fake_setup,
                                    fake_teardown),
    cmocka_unit_test_setup_teardown(test_empty, fake_setup, fake_teardown),
  };

  return cmocka_run_group_tests_name("ui_app", tests, NULL, NULL);
}
