/****************************************************************************
 * pnut-os/tests/unit/test_ui_life.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The apps' life cycle in the system UI (RFC 0037, The life cycle): an
 * app opened from Menu through Service states' Start, the Busy the UI
 * draws while it opens, Cancel, the deadline of its Open.  A fake app and
 * a fake Service states on one loop with the ui module; the timings
 * shortened.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "fake_app.h"

#include "apps.h"
#include "apptpl.h"
#include "blocks.h"
#include "icons.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NOTES      FAKE_NOTES
#define INBOX      1
#define TODO       "org.example.todo"
#define REMINDERS  "org.example.reminders"
#define MAIL       "org.example.mail"     /* No manifest */

/* The timings these tests run with, in ms */

#define T_OPENING  100
#define T_OPEN     600
#define T_STOP     200

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int g_list[1] =
{
  PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST
};

static const int g_two[2] =
{
  PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST, PNUT_UI_TEMPLATE_UI_TEMPLATE_EMPTY
};

/* A folder of Menu with one native app, and one that is not */

static const struct ui_app_s g_test_apps[2] =
{
  { "Memo",    UI_ICON_NOTES,   NULL, false, NOTES },
  { "Weather", UI_ICON_WEATHER, NULL, false, NULL },
};

static const struct ui_folder_s g_test_menu =
{
  "Menu", UI_ICON_MENU, g_test_apps, 2
};

/* Two native apps, for one kept warm */

static const struct ui_app_s g_two_apps[2] =
{
  { "Notes", UI_ICON_NOTES,     NULL, false, NOTES },
  { "Todo",  UI_ICON_REMINDERS, NULL, false, TODO },
};

static const struct ui_folder_s g_two_menu =
{
  "Menu", UI_ICON_MENU, g_two_apps, 2
};

/* Four, the last with no manifest */

static const struct ui_app_s g_four_apps[4] =
{
  { "Notes",     UI_ICON_NOTES,     NULL, false, NOTES },
  { "Todo",      UI_ICON_REMINDERS, NULL, false, TODO },
  { "Reminders", UI_ICON_REMINDERS, NULL, false, REMINDERS },
  { "Mail",      UI_ICON_EMAIL,     NULL, false, MAIL },
};

static const struct ui_folder_s g_four_menu =
{
  "Menu", UI_ICON_MENU, g_four_apps, 4
};

/* The slots' states, kept by a step: Notes', Todo's, Reminders' */

static int g_states[3];

/* What a step saw, kept while the loop runs */

static char g_top[32];
static char g_title[64];
static char g_busy[64];
static char g_top2[32];
static int g_depth;
static int g_depth2;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void times_short(void)
{
  ui_times_default(&g_times);
  g_times.opening = T_OPENING;
  g_times.open    = T_OPEN;
  g_times.stop    = T_STOP;
}

static void see(FAR char *top, FAR int *depth, FAR char *title)
{
  FAR struct screen_s *s = ui_top(&g_ui);

  strlcpy(top, s->cls->name, 32);
  *depth = g_ui.depth;
  if (title != NULL && s->header != NULL)
    {
      strlcpy(title, block_header_text(s->header, BLOCK_HEADER_TITLE), 64);
    }
}

static void look(void)
{
  see(g_top, &g_depth, g_title);
}

static void look2(void)
{
  see(g_top2, &g_depth2, NULL);
}

/* The Opening screen's heading */

static void look_busy(void)
{
  FAR struct screen_s *s = ui_top(&g_ui);
  FAR struct tpl_busy_s *t = s->state;

  look();
  if (strcmp(g_top, "opening") == 0)
    {
      strlcpy(g_busy, lv_label_get_text(t->heading), sizeof(g_busy));
    }
}

static void img_opening(void)
{
  save("app-opening");
}

static void menu(void)
{
  ui_push(&g_ui, &g_scr_menu, &g_test_menu);
}

static void enter(void)
{
  key(KEYMAP_ENTER, true);
}

static void back(void)
{
  key(KEYMAP_BACKSPACE, true);
}

static void right(void)
{
  key('d', false);
}

/* The app, started: its task connects and opens */

static void connect(void)
{
  app_connect_unstarted(&g_app);
}

static void open_app(void)
{
  app_open(&g_app, NULL, 4);
}

static void show_inbox(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Inbox");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  in.content.list.rows[0].id = 1;
  strcpy(in.content.list.rows[0].title, "Shopping");
  app_send_show(&g_app, &in);
}

static int setup(void **state)
{
  fake_setup(state);
  times_short();
  g_top[0] = '\0';
  g_top2[0] = '\0';
  g_title[0] = '\0';
  g_busy[0] = '\0';
  g_depth = 0;
  g_depth2 = 0;
  write_manifest(NOTES, "notes", 1, g_list);
  return 0;
}

static int teardown(void **state)
{
  return fake_teardown(state);
}

/****************************************************************************
 * Name: test_menu_starts
 *
 * Description:
 *   A native app's tile asks Service states to start it; its Open is
 *   welcomed, and its first Show, inside 0.7 s, is drawn with no Busy
 *   before it.
 ****************************************************************************/

static void test_menu_starts(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 40, enter },
    { 50, connect },
    { 60, open_app },
    { 80, show_inbox },
    { 90 + T_OPENING, look },
    { 300, NULL },
  };

  run(steps, 7);
  assert_int_equal(g_starts, 1);
  assert_string_equal(g_started, NOTES);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_OPENED);
  assert_string_equal(g_top, "app");
  assert_int_equal(g_depth, 3);           /* Home, Menu, the app */
}

/****************************************************************************
 * Name: test_menu_not_native
 *
 * Description:
 *   A tile with no app behind it opens the placeholder, and starts
 *   nothing.
 ****************************************************************************/

static void test_menu_not_native(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 30, right },
    { 40, enter },
    { 60, look },
    { 100, NULL },
  };

  run(steps, 5);
  assert_int_equal(g_starts, 0);
  assert_string_equal(g_top, "unavailable");
}

/****************************************************************************
 * Name: test_opening_busy
 *
 * Description:
 *   Nothing drawn by 0.7 s: the UI's Busy "Opening Notes…" (the
 *   manifest's name); the app's first Show then takes its place.
 ****************************************************************************/

static void test_opening_busy(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 40, enter },
    { 60 + T_OPENING, look_busy },
    { 70 + T_OPENING, img_opening },
    { 200, connect },
    { 210, open_app },
    { 300, show_inbox },
    { 400, look2 },
    { 500, NULL },
  };

  run(steps, 9);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_title, "Notes");
  assert_string_equal(g_busy, "Opening Notes\xe2\x80\xa6");
  assert_string_equal(g_top2, "app");
  assert_int_equal(g_depth2, 3);          /* The Busy replaced */
}

/****************************************************************************
 * Name: test_opening_cancel
 *
 * Description:
 *   Back on the Opening screen cancels: Service states stops the app, and
 *   Menu is on top again.  Its Open, coming late, is answered with Stop.
 ****************************************************************************/

static void test_opening_cancel(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 40, enter },
    { 60 + T_OPENING, back },
    { 200, look },
    { 210, connect },
    { 220, open_app },
    { 400, NULL },
  };

  run(steps, 7);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_string_equal(g_top, "menu");
  assert_int_equal(g_app.nevents, 1);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_STOP_TAG);
}

/****************************************************************************
 * Name: test_open_deadline
 *
 * Description:
 *   No Open 10 s after a start: Service states stops the app, the Busy
 *   goes, Menu is on top again.
 ****************************************************************************/

static void test_open_deadline(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 40, enter },
    { 60 + T_OPENING, look },
    { 60 + T_OPEN, look2 },
    { 100 + T_OPEN, NULL },
  };

  run(steps, 5);
  assert_string_equal(g_top, "opening");
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_string_equal(g_top2, "menu");
}

/****************************************************************************
 * Name: test_start_twice
 *
 * Description:
 *   A tile pressed twice while its app opens: one start.
 ****************************************************************************/

static void test_start_twice(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu },
    { 40, enter },
    { 50, enter },
    { 60 + T_OPENING, enter },
    { 200, NULL },
  };

  run(steps, 5);
  assert_int_equal(g_starts, 1);
}

/****************************************************************************
 * Name: test_warm_reopen
 *
 * Description:
 *   Notes open, then Todo opened: Notes is kept warm.  Notes' tile then
 *   brings it back without a start: it is sent Shown, its Show is drawn,
 *   and Todo is warm in its turn.
 ****************************************************************************/

static void menu_two(void)
{
  ui_push(&g_ui, &g_scr_menu, &g_two_menu);
}

static void connect_todo(void)
{
  app_connect_unstarted(&g_app2);
}

static void open_todo(void)
{
  strcpy(g_named, TODO);          /* Who names the next task Todo */
  app_open(&g_app2, NULL, 4);
}

static void show_todo(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Today");
  in.which_content = PNUT_UI_SHOW_REQUEST_EMPTY_TAG;
  strcpy(in.content.empty.heading, "Nothing to do");
  app_send_show(&g_app2, &in);
}

/* Menu again over Todo's screen, as Home will leave it (Task A3) */

static void todo_tile(void)
{
  ui_push(&g_ui, &g_scr_menu, &g_two_menu);
  key('d', false);
  key(KEYMAP_ENTER, true);
}

static void notes_tile(void)
{
  ui_push(&g_ui, &g_scr_menu, &g_two_menu);
  key(KEYMAP_ENTER, true);
}

static void named_notes(void)
{
  strcpy(g_named, NOTES);
}

/* An app's slot's state, HOST_FREE for none */

static int slot_state(FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (g_ui.host.slots[i].state != HOST_FREE &&
          strcmp(g_ui.host.slots[i].id, id) == 0)
        {
          return g_ui.host.slots[i].state;
        }
    }

  return HOST_FREE;
}

static void keep_states(void)
{
  FAR struct host_app_s *app = host_app(&g_ui.host);

  strlcpy(g_busy, app != NULL ? app->id : "", sizeof(g_busy));
  g_states[0] = slot_state(NOTES);
  g_states[1] = slot_state(TODO);
  g_states[2] = slot_state(REMINDERS);
}

/* A tile of the four, from a Menu pushed afresh */

static void tile(int n)
{
  int i;

  ui_push(&g_ui, &g_scr_menu, &g_four_menu);
  for (i = 0; i < n % 3; i++)
    {
      key('d', false);
    }

  for (i = 0; i < n / 3; i++)
    {
      key('s', false);
    }

  key(KEYMAP_ENTER, true);
}

static void tile_notes(void)
{
  tile(0);
}

static void tile_todo(void)
{
  tile(1);
}

static void tile_reminders(void)
{
  tile(2);
}

static void tile_mail(void)
{
  tile(3);
}

static void named_todo(void)
{
  strcpy(g_named, TODO);
}

static void test_warm_reopen(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, menu_two },
    { 30, enter },
    { 40, connect },
    { 50, open_app },
    { 100, show_inbox },
    { 150, todo_tile },
    { 160, connect_todo },
    { 170, open_todo },
    { 250, show_todo },
    { 260, named_notes },
    { 300, notes_tile },
    { 400, show_inbox },
    { 450, look },
    { 460, keep_states },
    { 500, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 15);
  assert_int_equal(g_starts, 2);          /* Notes, Todo; not Notes again */
  assert_int_equal(g_app.nevents, 2);     /* Welcome, Shown */
  assert_int_equal(g_app.events[1].which_kind, PNUT_UI_EVENT_SHOWN_TAG);
  assert_true(g_app2.up);                 /* Todo warm */
  assert_int_equal(g_states[1], HOST_WARM);
  assert_string_equal(g_busy, NOTES);
  assert_string_equal(g_top, "app");
  assert_string_equal(g_title, "Inbox");
}

/* Run the steps after these: Notes open and drawn, then Todo, which
 * leaves Notes warm
 */

#define NOTES_TODO 8

static void run_after(FAR const struct step_s *more, int n)
{
  static const struct step_s first[NOTES_TODO] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 120, tile_todo },
    { 130, connect_todo },
    { 140, open_todo },
    { 200, show_todo },
  };

  static struct step_s steps[NOTES_TODO + 8];

  memcpy(steps, first, sizeof(first));
  memcpy(&steps[NOTES_TODO], more, n * sizeof(*more));
  write_manifest(TODO, "reminders", 2, g_two);
  write_manifest(REMINDERS, "reminders", 2, g_two);
  run(steps, NOTES_TODO + n);
}

/****************************************************************************
 * Name: test_warm_then_tile
 *
 * Description:
 *   Notes brought back from warm, and another tile pressed before Notes
 *   draws: Notes is kept, never told to stop.
 ****************************************************************************/

static void test_warm_then_tile(void **state)
{
  static const struct step_s more[] =
  {
    { 300, tile_notes },
    { 310, tile_reminders },
    { 400, keep_states },
    { 500, NULL },
  };

  run_after(more, 4);
  assert_int_equal(g_app.nevents, 2);     /* Welcome, Shown: no Stop */

  /* Open: warm once Reminders opens */

  assert_int_equal(g_states[0], HOST_OPEN);
  assert_int_equal(g_states[2], HOST_OPENING);
  assert_int_equal(g_stops, 0);
}

/****************************************************************************
 * Name: test_opening_then_warm
 *
 * Description:
 *   Reminders opening, then the warm Notes' tile: Reminders is cancelled
 *   (stopped by Service states), and its Busy never drawn.
 ****************************************************************************/

static void test_opening_then_warm(void **state)
{
  static const struct step_s more[] =
  {
    { 300, tile_reminders },
    { 320, tile_notes },
    { 340, show_inbox },
    { 400 + T_OPENING, look },
    { 410 + T_OPENING, keep_states },
    { 500 + T_OPENING, NULL },
  };

  run_after(more, 6);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, REMINDERS);
  assert_int_equal(g_states[2], HOST_FREE);
  assert_int_equal(g_states[0], HOST_OPEN);
  assert_string_equal(g_top, "app");
  assert_string_equal(g_title, "Inbox");
}

/****************************************************************************
 * Name: test_tile_no_manifest
 *
 * Description:
 *   A tile whose app has no manifest while another app opens: the
 *   placeholder, and the app opening is left to open.
 ****************************************************************************/

static void test_tile_no_manifest(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, tile_mail },
    { 40, look2 },
    { 50, connect },
    { 60, open_app },
    { 200, NULL },
  };

  run(steps, 6);
  assert_int_equal(g_starts, 1);
  assert_int_equal(g_stops, 0);
  assert_string_equal(g_top2, "unavailable");
  assert_int_equal(g_app.nevents, 1);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
}

/****************************************************************************
 * Name: test_start_fails_late
 *
 * Description:
 *   Notes' Start fails late, after Todo, pressed meanwhile, took the slot
 *   Notes had: Todo is left to open.
 ****************************************************************************/

static void test_start_fails_late(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 40, tile_todo },
    { 250, keep_states },
    { 260, named_todo },
    { 270, connect },
    { 280, open_app },
    { 400, NULL },
  };

  g_start_fail_first = 150;
  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 7);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[1], HOST_OPENING);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
}

/****************************************************************************
 * Name: test_opening_under_pin
 *
 * Description:
 *   The SIM PIN is the system's (layer 5): the Opening Busy goes beneath
 *   it, and the app's first screen in its place, the PIN on top.
 ****************************************************************************/

static void sim_locked(void)
{
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN);
}

static void start_notes(void)
{
  assert_int_equal(host_start(&g_ui.host, NOTES), 0);
}

static void below_top(void)
{
  strlcpy(g_busy, g_ui.stack[g_ui.depth - 2]->cls->name, sizeof(g_busy));
}

static void test_opening_under_pin(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, sim_locked },
    { 40, start_notes },
    { 60 + T_OPENING, below_top },
    { 70 + T_OPENING, look },
    { 200, connect },
    { 210, open_app },
    { 300, show_inbox },
    { 400, look2 },
    { 500, NULL },
  };

  run(steps, 9);
  assert_string_equal(g_busy, "opening");  /* Beneath the PIN */
  assert_string_equal(g_top, "pin");
  assert_string_equal(g_top2, "pin");      /* Still on top after the Show */
  assert_int_equal(g_depth2, g_depth);     /* The Busy replaced */
}

/****************************************************************************
 * Name: test_home_keeps_warm
 *
 * Description:
 *   Alt and Backspace held: Home, the open app kept warm, still connected,
 *   never told to stop; its tile brings it back.
 ****************************************************************************/

static void home_down(void)
{
  input_key(&g_input, KEYMAP_ALT, true, true);
  input_key(&g_input, KEYMAP_BACKSPACE, true, true);
}

static void home_up(void)
{
  input_key(&g_input, KEYMAP_BACKSPACE, true, false);
  input_key(&g_input, KEYMAP_ALT, true, false);
}

static void test_home_keeps_warm(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, home_down },
    { 700, home_up },
    { 710, look },
    { 720, keep_states },
    { 800, tile_notes },
    { 850, show_inbox },
    { 900, look2 },
    { 1000, NULL },
  };

  run(steps, 12);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_depth, 1);
  assert_int_equal(g_states[0], HOST_WARM);
  assert_int_equal(g_app.nevents, 2);     /* Welcome, Shown */
  assert_int_equal(g_app.events[1].which_kind, PNUT_UI_EVENT_SHOWN_TAG);
  assert_string_equal(g_top2, "app");
  assert_int_equal(g_starts, 1);
  assert_int_equal(g_stops, 0);
}

/****************************************************************************
 * Name: test_one_warm
 *
 * Description:
 *   Notes, Home, Todo, Home: two would be warm; the one open longest ago,
 *   Notes, is told to stop, and, not stopping by its deadline, stopped by
 *   Service states, its slot free.
 ****************************************************************************/

static void home_now(void)
{
  ui_home(&g_ui);
}

static void test_one_warm(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, home_now },
    { 120, tile_todo },
    { 130, connect_todo },
    { 140, open_todo },
    { 200, show_todo },
    { 220, home_now },
    { 240, keep_states },
    { 260, look },
    { 300 + T_STOP, look2 },
    { 310 + T_STOP, keep_states },
    { 400 + T_STOP, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 15);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_app.events[g_app.nevents - 1].which_kind,
                   PNUT_UI_EVENT_STOP_TAG);
  assert_int_equal(g_app2.nevents, 1);    /* Todo: Welcome only */
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_int_equal(g_states[1], HOST_WARM);
}

/****************************************************************************
 * Name: test_stop_answered
 *
 * Description:
 *   An app told to stop that leaves before its deadline: its slot free,
 *   and Service states never asked.
 ****************************************************************************/

static void leave(void)
{
  app_leave(&g_app);
}

static void test_stop_answered(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, home_now },
    { 120, tile_todo },
    { 130, connect_todo },
    { 140, open_todo },
    { 200, show_todo },
    { 220, home_now },
    { 250, leave },
    { 300 + T_STOP, keep_states },
    { 400 + T_STOP, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 13);
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_FREE);
}

/****************************************************************************
 * Name: test_unstarted_deadline
 *
 * Description:
 *   An app the system UI did not start, told to stop, that does not:
 *   stopped by Service states at its deadline, its slot free.
 ****************************************************************************/

static void test_unstarted_deadline(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect },
    { 30, open_app },
    { 100, keep_states },
    { 200 + T_STOP, look2 },
    { 210 + T_STOP, keep_states },
    { 300 + T_STOP, NULL },
  };

  run(steps, 6);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_STOP_TAG);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_FREE);
}

/****************************************************************************
 * Name: test_home_while_opening
 *
 * Description:
 *   Home while an app opens, its Busy up: the app cancelled, Home on top.
 ****************************************************************************/

static void test_home_while_opening(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 60 + T_OPENING, home_now },
    { 200, look },
    { 210, keep_states },
    { 300, NULL },
  };

  run(steps, 5);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_stops, 1);
  assert_int_equal(g_states[0], HOST_FREE);
}

/****************************************************************************
 * Name: test_home_under_pin
 *
 * Description:
 *   Home while the SIM PIN is up does nothing: the PIN is answered first,
 *   the app beneath it stays open.
 ****************************************************************************/

static void test_home_under_pin(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, sim_locked },
    { 150, home_now },
    { 200, look },
    { 210, keep_states },
    { 300, NULL },
  };

  run(steps, 9);
  assert_string_equal(g_top, "pin");
  assert_int_equal(g_states[0], HOST_OPEN);
}

/****************************************************************************
 * Name: test_home_undrawn
 *
 * Description:
 *   Home after the app opened but before it drew: it is told to stop, and
 *   the UI's Busy never comes over Home.
 ****************************************************************************/

static void test_home_undrawn(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 60, home_now },
    { 100 + T_OPENING, look },
    { 110 + T_OPENING, keep_states },
    { 200 + T_OPENING, NULL },
  };

  run(steps, 7);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_states[0], HOST_STOPPING);
  assert_int_equal(g_app.events[g_app.nevents - 1].which_kind,
                   PNUT_UI_EVENT_STOP_TAG);
}

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test_setup_teardown(test_menu_starts, setup, teardown),
    cmocka_unit_test_setup_teardown(test_menu_not_native, setup, teardown),
    cmocka_unit_test_setup_teardown(test_opening_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(test_opening_cancel, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_deadline, setup, teardown),
    cmocka_unit_test_setup_teardown(test_start_twice, setup, teardown),
    cmocka_unit_test_setup_teardown(test_warm_reopen, setup, teardown),
    cmocka_unit_test_setup_teardown(test_warm_then_tile, setup, teardown),
    cmocka_unit_test_setup_teardown(test_opening_then_warm, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_tile_no_manifest, setup, teardown),
    cmocka_unit_test_setup_teardown(test_start_fails_late, setup, teardown),
    cmocka_unit_test_setup_teardown(test_opening_under_pin, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_home_keeps_warm, setup, teardown),
    cmocka_unit_test_setup_teardown(test_one_warm, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stop_answered, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unstarted_deadline, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_home_while_opening, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_home_under_pin, setup, teardown),
    cmocka_unit_test_setup_teardown(test_home_undrawn, setup, teardown),
  };

  return cmocka_run_group_tests_name("ui_life", tests, NULL, NULL);
}
