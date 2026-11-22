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
#define T_BUSY     300
#define T_HUNG     600
#define T_STALL    250
#define T_GATHER   300
#define T_SPACING  800

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
static char g_open_id[64];              /* The open app's */

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
  g_times.busy    = T_BUSY;
  g_times.hung    = T_HUNG;
  g_times.stall   = T_STALL;
  g_times.gather  = T_GATHER;
  g_times.spacing = T_SPACING;
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
  g_open_id[0] = '\0';
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

  strlcpy(g_open_id, app != NULL ? app->id : "", sizeof(g_open_id));
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
  assert_string_equal(g_open_id, NOTES);
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

/****************************************************************************
 * Name: test_resume_kept
 *
 * Description:
 *   Notes gives its resume token, goes warm, and is stopped behind (Todo
 *   warm after it); it exits.  Its tile starts it again, welcomed as
 *   resumed with its token, which is kept still: until the app gives
 *   another or clears it, leaves, or crashes.
 ****************************************************************************/

static bool g_token_kept;

static void resume_inbox(void)
{
  app_resume(&g_app, "inbox/3");
}

static void notes_exits(void)
{
  pnut_ui_disconnect(&g_app.client);
}

static void keep_token(void)
{
  g_token_kept = host_token(&g_ui.host, NOTES) != NULL;
}

static void test_resume_kept(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 90, resume_inbox },
    { 100, home_now },
    { 120, tile_todo },
    { 130, connect_todo },
    { 140, open_todo },
    { 200, show_todo },
    { 220, home_now },                    /* Notes stopped */
    { 260, notes_exits },
    { 300, named_notes },
    { 310, tile_notes },
    { 320, connect },
    { 330, open_app },
    { 380, keep_token },
    { 400, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  g_token_kept = false;
  run(steps, 18);
  assert_true(g_token_kept);
  assert_int_equal(g_starts, 3);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_RESUMED);
  assert_int_equal(g_app.events[0].kind.welcome.token.size, 7);
  assert_memory_equal(g_app.events[0].kind.welcome.token.bytes, "inbox/3",
                      7);
}

/****************************************************************************
 * Name: test_resume_left
 *
 * Description:
 *   An app that leaves of itself is done: its token forgotten, its next
 *   start an ordinary opening.  So is an empty token.
 ****************************************************************************/

static void leave_and_exit(void)
{
  app_leave(&g_app);
}

static void resume_empty(void)
{
  app_resume(&g_app, "");
}

static int g_reason;

static void keep_reason(void)
{
  g_reason = g_app.events[0].kind.welcome.reason;
}

static void test_resume_left(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 90, resume_inbox },
    { 100, leave_and_exit },
    { 150, notes_exits },
    { 200, tile_notes },
    { 210, connect },
    { 220, open_app },
    { 250, keep_reason },
    { 300, show_inbox },
    { 310, resume_inbox },
    { 320, resume_empty },
    { 330, home_now },
    { 340, tile_todo },
    { 350, connect_todo },
    { 360, open_todo },
    { 400, show_todo },
    { 410, home_now },                    /* Notes stopped */
    { 450, notes_exits },
    { 460, named_notes },
    { 470, tile_notes },
    { 480, connect },
    { 490, open_app },
    { 600, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 26);
  assert_int_equal(g_reason, PNUT_UI_WELCOME_REASON_OPENED);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_OPENED);
  assert_int_equal(g_app.events[0].kind.welcome.token.size, 0);
}

/****************************************************************************
 * Name: test_back_waiting
 *
 * Description:
 *   Back asked of the app, unanswered by 3 s: the UI's Busy "Waiting for
 *   Notes…"; the app's Show then takes it off, its screen on top.
 ****************************************************************************/

static void key_back(void)
{
  key(KEYMAP_BACKSPACE, true);
}

static void soft_left(void)
{
  key(KEYMAP_F1, true);
}

static void soft_right(void)
{
  key(KEYMAP_F3, true);
}

static void img_waiting(void)
{
  save("app-waiting");
}

static void img_hung(void)
{
  save("app-hung");
}

static void test_back_waiting(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, key_back },
    { 150 + T_BUSY, look_busy },
    { 160 + T_BUSY, img_waiting },
    { 200 + T_BUSY, show_inbox },
    { 300 + T_BUSY, look2 },
    { 400 + T_BUSY, NULL },
  };

  run(steps, 10);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");
  assert_string_equal(g_top2, "app");
  assert_int_equal(g_depth2, g_depth - 1);
}

/****************************************************************************
 * Name: test_hung_close
 *
 * Description:
 *   Unanswered by 10 s: "Notes isn't responding", Wait or Close; Close
 *   tells the app to stop, its screen off, and forgets its token.
 ****************************************************************************/

static void test_hung_close(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 90, resume_inbox },
    { 100, key_back },
    { 150 + T_HUNG, look_busy },
    { 160 + T_HUNG, img_hung },
    { 170 + T_HUNG, soft_right },
    { 180 + T_HUNG, resume_inbox },       /* Saving as it stops */
    { 200 + T_HUNG, look2 },
    { 210 + T_HUNG, keep_states },
    { 300 + T_HUNG, NULL },
  };

  run(steps, 13);
  assert_string_equal(g_busy, "Notes isn\xe2\x80\x99t responding");
  assert_string_equal(g_top2, "menu");
  assert_int_equal(g_states[0], HOST_STOPPING);
  assert_int_equal(g_app.events[g_app.nevents - 1].which_kind,
                   PNUT_UI_EVENT_STOP_TAG);
  assert_null(host_token(&g_ui.host, NOTES));
}

/****************************************************************************
 * Name: test_hung_wait
 *
 * Description:
 *   Wait: "Waiting" again, and another 10 s before asking again.
 ****************************************************************************/

static char g_busy2[64];

static void look_busy2(void)
{
  look_busy();
  strlcpy(g_busy2, g_busy, sizeof(g_busy2));
}

static void test_hung_wait(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, key_back },
    { 150 + T_HUNG, soft_left },
    { 200 + T_HUNG, look_busy2 },
    { 200 + 2 * T_HUNG, look_busy },
    { 300 + 2 * T_HUNG, NULL },
  };

  run(steps, 9);
  assert_string_equal(g_busy2, "Waiting for Notes\xe2\x80\xa6");
  assert_string_equal(g_busy, "Notes isn\xe2\x80\x99t responding");
  assert_int_equal(g_stops, 0);
}

/****************************************************************************
 * Name: test_hung_long_name
 *
 * Description:
 *   An app's name as long as a manifest's may be: "<app> isn't
 *   responding" whole, not cut at a Busy heading's limit.
 ****************************************************************************/

#define LONG_NAME  "Notes from the long trip, part 2"   /* 32 bytes */

static void img_hung_long(void)
{
  save("app-hung-long");
}

static void test_hung_long_name(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, key_back },
    { 150 + T_HUNG, look_busy },
    { 160 + T_HUNG, img_hung_long },
    { 200 + T_HUNG, NULL },
  };

  g_manifest_name = LONG_NAME;
  write_manifest(NOTES, "notes", 1, g_list);
  run(steps, 8);
  assert_string_equal(g_busy, LONG_NAME " isn\xe2\x80\x99t responding");
}

/****************************************************************************
 * Name: test_home_clears_owed
 *
 * Description:
 *   Back, then Home before the app answers: nothing comes over Home later.
 ****************************************************************************/

static void test_home_clears_owed(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, key_back },
    { 150, home_now },
    { 200 + T_HUNG, look },
    { 300 + T_HUNG, NULL },
  };

  run(steps, 8);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_depth, 1);
}

/****************************************************************************
 * Name: test_first_screen_hung
 *
 * Description:
 *   An app that opens and never draws: its Opening Busy, then "isn't
 *   responding" at 10 s; a late first screen still takes its place.
 ****************************************************************************/

static void test_first_screen_hung(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 150 + T_HUNG, look_busy },
    { 200 + T_HUNG, show_inbox },
    { 300 + T_HUNG, look2 },
    { 400 + T_HUNG, NULL },
  };

  run(steps, 7);
  assert_string_equal(g_busy, "Notes isn\xe2\x80\x99t responding");
  assert_string_equal(g_top2, "app");
  assert_int_equal(g_depth2, 3);          /* Home, Menu, the app */
}

/****************************************************************************
 * Name: test_bad_show_owed
 *
 * Description:
 *   A first Show refused (invalid) answers nothing: the first screen is
 *   still owed, and "isn't responding" comes at 10 s.
 ****************************************************************************/

static void show_invalid(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = 0;                          /* Never 0 */
  strcpy(in.title, "Inbox");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  app_send_show(&g_app, &in);
}

static void test_bad_show_owed(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 60, show_invalid },
    { 150 + T_HUNG, look_busy },
    { 250 + T_HUNG, NULL },
  };

  run(steps, 6);
  assert_int_equal(g_app.status, PNUT_STATUS_INVALID);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Notes isn\xe2\x80\x99t responding");
}

/****************************************************************************
 * Name: test_shown_waiting
 *
 * Description:
 *   A warm app brought back that does not answer Shown: "Waiting".
 ****************************************************************************/

static void test_shown_waiting(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, home_now },
    { 120, tile_notes },
    { 170 + T_BUSY, look_busy },
    { 250 + T_BUSY, NULL },
  };

  run(steps, 8);
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");
}

/****************************************************************************
 * Name: test_owed_afresh
 *
 * Description:
 *   Back, Home, then the app brought back: its 3 s run from Shown, not
 *   from the Back before Home.
 ****************************************************************************/

static void test_owed_afresh(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, key_back },
    { 150, home_now },
    { 250, tile_notes },
    { 150 + T_BUSY, look2 },              /* 3 s after Back, not Shown */
    { 300 + T_BUSY, look_busy },          /* ... after Shown */
    { 400 + T_BUSY, NULL },
  };

  run(steps, 10);
  assert_string_equal(g_top2, "menu");
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");
}

/****************************************************************************
 * Name: test_stall
 *
 * Description:
 *   An app that reads no events for 3 s (no credit, Welcome waiting) is
 *   closed: its stream ended, its task stopped by Service states, its Busy
 *   off.  Credit given in time keeps it.
 ****************************************************************************/

static void open_no_credit(void)
{
  app_open(&g_app, NULL, 0);
}

static void credit(void)
{
  assert_int_equal(pnut_ui_open_credit(&g_app.client, g_app.stream, 8), 0);
}

static void test_stall(void **state)
{
  static const struct step_s stalled[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_no_credit },
    { 100 + T_STALL, keep_states },
    { 110 + T_STALL, look },
    { 200 + T_STALL, NULL },
  };

  static const struct step_s paced[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_no_credit },
    { 50 + T_STALL / 2, credit },
    { 100 + T_STALL, keep_states },
    { 200 + T_STALL, NULL },
  };

  run(stalled, 6);
  assert_int_equal(g_app.ended, PNUT_STATUS_BUSY);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_string_equal(g_top, "menu");

  g_stops = 0;
  run(paced, 6);
  assert_int_equal(g_app.nevents, 1);     /* Welcome */
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_OPEN);
}

/****************************************************************************
 * Name: test_crashes
 *
 * Description:
 *   An app whose connection closes three times in a row, never told to
 *   stop and never leaving, is disabled: its tile asks "Notes stopped
 *   working", and Try again starts it.  Its resume token goes with the
 *   first crash.
 ****************************************************************************/

static void crash_cycle(void)
{
  tile(0);
  app_connect_unstarted(&g_app);
}

static void crash(void)
{
  pnut_ui_disconnect(&g_app.client);
}

static void look_stopped(void)
{
  FAR struct screen_s *s = ui_top(&g_ui);
  FAR struct tpl_busy_s *t = s->state;

  look();
  if (strcmp(g_top, "stopped") == 0)
    {
      strlcpy(g_busy, lv_label_get_text(t->heading), sizeof(g_busy));
    }
}

static void img_stopped(void)
{
  save("app-stopped");
}

static void test_crashes(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, crash_cycle },
    { 30, open_app },
    { 80, show_inbox },
    { 90, resume_inbox },
    { 100, crash },
    { 120, crash_cycle },
    { 130, open_app },
    { 180, keep_reason },
    { 200, crash },
    { 220, crash_cycle },
    { 230, open_app },
    { 300, crash },
    { 320, tile_notes },
    { 340, look_stopped },
    { 345, img_stopped },
    { 350, enter },
    { 400, look2 },
    { 500, NULL },
  };

  run(steps, 18);
  assert_int_equal(g_reason, PNUT_UI_WELCOME_REASON_OPENED);
  assert_string_equal(g_top, "stopped");
  assert_string_equal(g_busy, "Notes stopped working");
  assert_int_equal(g_starts, 4);          /* Three, then Try again */
  assert_string_equal(g_top2, "menu");
}

/****************************************************************************
 * Name: test_crashes_reset
 *
 * Description:
 *   A Leave between crashes starts the count again.
 ****************************************************************************/

static void test_crashes_reset(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, crash_cycle },
    { 30, open_app },
    { 80, crash },
    { 100, crash_cycle },
    { 110, open_app },
    { 160, crash },
    { 180, crash_cycle },
    { 190, open_app },
    { 240, leave_and_exit },
    { 260, crash },
    { 280, crash_cycle },
    { 290, open_app },
    { 340, crash },
    { 360, tile_notes },
    { 380, look },
    { 500, NULL },
  };

  run(steps, 16);
  assert_int_equal(g_starts, 5);
  assert_string_equal(g_top, "menu");
}

/****************************************************************************
 * Name: test_stall_slow_reader
 *
 * Description:
 *   An app that reads slowly, one event at a time, but keeps reading, is
 *   kept: the stall is no event read for 3 s, not events waiting.
 ****************************************************************************/

static void backs(void)
{
  key(KEYMAP_BACKSPACE, true);
  key(KEYMAP_BACKSPACE, true);
  key(KEYMAP_BACKSPACE, true);
}

static void credit1(void)
{
  assert_int_equal(pnut_ui_open_credit(&g_app.client, g_app.stream, 1), 0);
}

static void test_stall_slow_reader(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_no_credit },
    { 60, show_inbox },
    { 70, backs },
    { 100, credit1 },
    { 180, credit1 },
    { 260, credit1 },
    { 340, credit1 },
    { 400, keep_states },
    { 450, NULL },
  };

  run(steps, 11);
  assert_int_equal(g_app.nevents, 4);     /* Welcome, three Backs */
  assert_int_equal(g_app.ended, -1);
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_OPEN);
}

/****************************************************************************
 * Name: test_overflow_stops
 *
 * Description:
 *   An app taken off for HOST_EVENTS events waiting has its task stopped
 *   by Service states, as the stall's rule does; a shell's program's is
 *   never stopped by its name, which is no service's.
 ****************************************************************************/

#define DEV  "org.example.dev"            /* A shell's program, no manifest */

static void backs17(void)
{
  int i;

  for (i = 0; i < HOST_EVENTS + 1; i++)
    {
      key(KEYMAP_BACKSPACE, true);
    }
}

static void open_one(void)
{
  app_open(&g_app, NULL, 1);
}

static void shell_open_one(void)
{
  g_who = WHO_SHELL;
  app_open(&g_app, DEV, 1);
}

static void show_dev(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Dev");
  in.which_content = PNUT_UI_SHOW_REQUEST_EMPTY_TAG;
  strcpy(in.content.empty.heading, "Nothing");
  app_send_show(&g_app, &in);
}

static void test_overflow_stops(void **state)
{
  static const struct step_s named[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_one },
    { 80, show_inbox },
    { 120, backs17 },
    { 200, keep_states },
    { 220, NULL },
  };

  static const struct step_s shell[] =
  {
    { 30, connect },
    { 40, shell_open_one },
    { 80, show_dev },
    { 120, backs17 },
    { 220, NULL },
  };

  run(named, 7);
  assert_int_equal(g_app.ended, PNUT_STATUS_BUSY);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_FREE);

  g_stops = 0;
  run(shell, 5);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.ended, PNUT_STATUS_BUSY);
  assert_int_equal(g_stops, 0);
}

/****************************************************************************
 * Name: test_resume_on_stop
 *
 * Description:
 *   An app told to stop behind saves its place as it stops: the token it
 *   gives then is kept, and given back when it is started again.
 ****************************************************************************/

static void resume_draft(void)
{
  app_resume(&g_app, "draft/9");
}

static void test_resume_on_stop(void **state)
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
    { 220, home_now },                    /* Notes stopped */
    { 230, resume_draft },
    { 260, notes_exits },
    { 300, named_notes },
    { 310, tile_notes },
    { 320, connect },
    { 330, open_app },
    { 400, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 17);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_RESUMED);
  assert_int_equal(g_app.events[0].kind.welcome.token.size, 7);
  assert_memory_equal(g_app.events[0].kind.welcome.token.bytes, "draft/9",
                      7);
}

/****************************************************************************
 * Name: test_updates_gathered
 *
 * Description:
 *   The e-paper rule for an app's updates nobody's key caused: gathered
 *   for 1 s, the app's screen drawn at most every 2 s; now at once; quiet
 *   with the next redraw.
 ****************************************************************************/

static int g_marks[8];
static int g_nmarks;

static void mark(void)
{
  g_marks[g_nmarks++] = g_updates;
}

static void update_title(bool now, bool quiet)
{
  static pnut_ui_update_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  in.which_change = PNUT_UI_UPDATE_REQUEST_TITLE_TAG;
  snprintf(in.change.title, sizeof(in.change.title), "Inbox %d",
           g_updates);
  in.now = now;
  in.quiet = quiet;
  app_send_update(&g_app, &in);
}

static void update(void)
{
  update_title(false, false);
}

static void update_now(void)
{
  update_title(true, false);
}

static void update_quiet(void)
{
  update_title(false, true);
}

static void test_updates_gathered(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },                   /* In the tile's key window */
    { 700, mark },                        /* 0 */
    { 800, update },
    { 850, update },
    { 1050, mark },                       /* 1: not yet */
    { 1150, mark },                       /* 2: one, at 1100 */
    { 1200, update },
    { 1800, mark },                       /* 3: spaced to 1900 */
    { 2000, mark },                       /* 4: one */
    { 2100, update_now },
    { 2120, mark },                       /* 5: at once */
    { 2200, update_quiet },
    { 2900, mark },                       /* 6: waits */
    { 3000, NULL },
  };

  g_nmarks = 0;
  run(steps, 17);
  assert_int_equal(g_marks[1], g_marks[0]);
  assert_int_equal(g_marks[2], g_marks[0] + 1);
  assert_int_equal(g_marks[3], g_marks[2]);
  assert_int_equal(g_marks[4], g_marks[2] + 1);
  assert_int_equal(g_marks[5], g_marks[4] + 1);
  assert_int_equal(g_marks[6], g_marks[5]);
}

/****************************************************************************
 * Name: test_switch_full
 *
 * Description:
 *   Switching to an app, and going Home, take a full refresh.
 ****************************************************************************/

static int g_fulls_at[4];

static void mark_fulls(void)
{
  g_fulls_at[g_nmarks++] = g_fulls;
}

static void test_switch_full(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, mark_fulls },
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },                   /* Drawn with the tile's key */
    { 700, mark_fulls },
    { 800, home_now },
    { 1000, mark_fulls },
    { 1100, NULL },
  };

  g_nmarks = 0;
  run(steps, 9);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0] + 1);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1] + 1);
}

/****************************************************************************
 * Name: test_switch_on_screen
 *
 * Description:
 *   The full refresh is the app's first screen's (the owner: on the app's
 *   first screen or Home): not a redraw of Menu after its Open, nor the
 *   UI's Busy; none at all for an app cancelled before it drew, or gone
 *   before its screen reached the glass.  A warm app brought back takes
 *   it with the Show that answers Shown.
 ****************************************************************************/

static void times_long(void)
{
  g_times.opening = 5000;
  g_times.busy    = 5000;
  g_times.hung    = 9000;
}

static void cancel_open(void)
{
  host_cancel(host_app(&g_ui.host));
}

static void test_switch_on_screen(void **state)
{
  static const struct step_s focus[] =
  {
    { 10, menu_two },
    { 600, mark_fulls },
    { 620, enter },
    { 630, connect },
    { 640, open_app },
    { 1200, mark_fulls },
    { 1300, right },                      /* Menu's focus moves */
    { 1900, mark_fulls },                 /* Menu redrawn: not full */
    { 2000, show_inbox },
    { 2600, mark_fulls },                 /* The app's screen: full */
    { 2700, NULL },
  };

  static const struct step_s cancelled[] =
  {
    { 10, menu_two },
    { 600, mark_fulls },
    { 620, enter },
    { 630, connect },
    { 640, open_app },
    { 1200, cancel_open },
    { 1800, mark_fulls },
    { 1900, right },
    { 2500, mark_fulls },                 /* Not full */
    { 2600, NULL },
  };

  static const struct step_s busy[] =
  {
    { 10, menu_two },
    { 600, mark_fulls },
    { 620, enter },
    { 630, connect },
    { 640, open_app },
    { 1300, mark_fulls },                 /* The Busy: not full */
    { 1400, show_inbox },
    { 1900, mark_fulls },                 /* The app's screen: full */
    { 2000, NULL },
  };

  static const struct step_s gone[] =
  {
    { 10, mark_fulls },
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },                   /* In the tile's key window */
    { 120, crash },                       /* Gone before it was drawn */
    { 700, mark_fulls },
    { 800, NULL },
  };

  static const struct step_s shown[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 700, home_now },
    { 1300, mark_fulls },
    { 1400, tile_notes },                 /* Warm: Shown */
    { 2000, mark_fulls },                 /* Menu drawn: not full */
    { 2100, show_inbox },
    { 2700, mark_fulls },                 /* Its Show: full */
    { 2800, NULL },
  };

  times_long();
  g_nmarks = 0;
  run(focus, 11);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1]);
  assert_int_equal(g_fulls_at[3], g_fulls_at[2] + 1);

  g_nmarks = 0;
  run(cancelled, 10);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1]);

  g_times.busy = 5000;
  g_times.opening = T_OPENING;
  g_nmarks = 0;
  run(busy, 9);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1] + 1);

  times_long();
  g_nmarks = 0;
  run(gone, 8);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);

  g_nmarks = 0;
  run(shown, 11);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1] + 1);
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
    cmocka_unit_test_setup_teardown(test_resume_kept, setup, teardown),
    cmocka_unit_test_setup_teardown(test_resume_left, setup, teardown),
    cmocka_unit_test_setup_teardown(test_resume_on_stop, setup, teardown),
    cmocka_unit_test_setup_teardown(test_overflow_stops, setup, teardown),
    cmocka_unit_test_setup_teardown(test_back_waiting, setup, teardown),
    cmocka_unit_test_setup_teardown(test_hung_close, setup, teardown),
    cmocka_unit_test_setup_teardown(test_hung_wait, setup, teardown),
    cmocka_unit_test_setup_teardown(test_hung_long_name, setup, teardown),
    cmocka_unit_test_setup_teardown(test_home_clears_owed, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_first_screen_hung, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_bad_show_owed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_shown_waiting, setup, teardown),
    cmocka_unit_test_setup_teardown(test_owed_afresh, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stall, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stall_slow_reader, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_crashes, setup, teardown),
    cmocka_unit_test_setup_teardown(test_crashes_reset, setup, teardown),
    cmocka_unit_test_setup_teardown(test_updates_gathered, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_switch_full, setup, teardown),
    cmocka_unit_test_setup_teardown(test_switch_on_screen, setup,
                                    teardown),
  };

  return cmocka_run_group_tests_name("ui_life", tests, NULL, NULL);
}
