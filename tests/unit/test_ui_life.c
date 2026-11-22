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

#include "app.h"
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
#define PHOTOS     "org.example.photos"
#define MAPS       "org.example.maps"

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

/* Six, for every slot taken */

static const struct ui_app_s g_six_apps[6] =
{
  { "Notes",     UI_ICON_NOTES,     NULL, false, NOTES },
  { "Todo",      UI_ICON_REMINDERS, NULL, false, TODO },
  { "Reminders", UI_ICON_REMINDERS, NULL, false, REMINDERS },
  { "Photos",    UI_ICON_NOTES,     NULL, false, PHOTOS },
  { "Maps",      UI_ICON_NOTES,     NULL, false, MAPS },
  { "Mail",      UI_ICON_EMAIL,     NULL, false, MAIL },
};

static const struct ui_folder_s g_six_menu =
{
  "Menu", UI_ICON_MENU, g_six_apps, 6
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

/* The log caught (catch_start(), catch_end()) */

static char g_out[64 * 1024];
static FAR FILE *g_catch;
static int g_saved = -1;

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

/* The log, which on the computer is the standard error, caught in a
 * file while a test runs
 */

static void catch_start(void)
{
  fflush(stderr);
  g_catch = tmpfile();
  assert_non_null(g_catch);
  g_saved = dup(STDERR_FILENO);
  assert_true(g_saved >= 0);
  assert_true(dup2(fileno(g_catch), STDERR_FILENO) >= 0);
}

/* The lines caught, in g_out; the standard error as it was */

static FAR const char *catch_end(void)
{
  size_t n;

  fflush(stderr);
  dup2(g_saved, STDERR_FILENO);
  close(g_saved);
  g_saved = -1;
  rewind(g_catch);
  n = fread(g_out, 1, sizeof(g_out) - 1, g_catch);
  g_out[n] = '\0';
  fclose(g_catch);
  g_catch = NULL;
  return g_out;
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
  if (g_saved >= 0)
    {
      catch_end();                /* A test failed while catching */
    }

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

  static struct step_s steps[NOTES_TODO + 12];

  memcpy(steps, first, sizeof(first));
  memcpy(&steps[NOTES_TODO], more, n * sizeof(*more));
  strcpy(g_named, NOTES);         /* Who names Notes first */
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

/* It leaves, and its task exits */

static void leave(void)
{
  app_leave(&g_app);
  task_exits(NOTES);
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

/* Its task exits: its connection closes, and the services topic tells */

static void notes_exits(void)
{
  pnut_ui_disconnect(&g_app.client);
  task_exits(NOTES);
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
  task_exits(NOTES);
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

/****************************************************************************
 * Name: test_sheet_gathered
 *
 * Description:
 *   A sheet the app opens on its own, nobody's key causing it, is
 *   gathered and spaced, whether it replaces a sheet open or shuts the
 *   options: what it takes off is drawn with it, not at once.
 ****************************************************************************/

static const int g_list_confirm[2] =
{
  PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST, PNUT_UI_TEMPLATE_UI_TEMPLATE_CONFIRM
};

static void sheet(uint32_t id, FAR const char *question)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = id;
  in.sheet = true;
  in.which_content = PNUT_UI_SHOW_REQUEST_CONFIRM_TAG;
  strcpy(in.content.confirm.question, question);
  app_send_show(&g_app, &in);
}

static void sheet2(void)
{
  sheet(2, "Delete?");
}

static void sheet3(void)
{
  sheet(3, "Really?");
}

static void show_inbox_options(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Inbox");
  in.options_count = 1;
  in.options[0].id = 1;
  strcpy(in.options[0].label, "Delete all");
  strcpy(in.options[0].letter, "x");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  in.content.list.rows[0].id = 1;
  strcpy(in.content.list.rows[0].title, "Shopping");
  app_send_show(&g_app, &in);
}

static void options_key(void)
{
  input_key(&g_input, KEYMAP_ALT, true, true);
  key(KEYMAP_ENTER, true);
  input_key(&g_input, KEYMAP_ALT, true, false);
}

static void test_sheet_gathered(void **state)
{
  static const struct step_s replaced[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 700, mark },                        /* 0 */
    { 800, sheet2 },                      /* Gathered: drawn at 1100 */
    { 900, mark },                        /* 1: not yet */
    { 1300, mark },                       /* 2: drawn */
    { 3000, sheet3 },                     /* Gathered: due 3300 */
    { 3100, mark },                       /* 3: not yet */
    { 3500, mark },                       /* 4: drawn */
    { 3600, NULL },
  };

  static const struct step_s options[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox_options },
    { 700, options_key },                 /* Drawn at 1200 */
    { 1300, mark },                       /* 0 */
    { 3000, sheet2 },                     /* Gathered: due 3300 */
    { 3100, mark },                       /* 1: not yet */
    { 3500, mark },                       /* 2: drawn */
    { 3600, NULL },
  };

  write_manifest(NOTES, "notes", 2, g_list_confirm);
  g_nmarks = 0;
  run(replaced, 12);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_int_equal(g_marks[1], g_marks[0]);
  assert_int_equal(g_marks[2], g_marks[0] + 1);
  assert_int_equal(g_marks[3], g_marks[2]);
  assert_int_equal(g_marks[4], g_marks[2] + 1);

  g_nmarks = 0;
  run(options, 10);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_int_equal(g_marks[1], g_marks[0]);
  assert_int_equal(g_marks[2], g_marks[0] + 1);
}

/****************************************************************************
 * Name: test_shell_app_id
 *
 * Description:
 *   A shell's program may not take an app's id (one with a manifest): it
 *   is refused, and Notes' resume token and crash count stay as they
 *   were: Notes, disabled, is still disabled (the owner, 2026-11-22).
 ****************************************************************************/

static int g_crash_count;
static uint32_t g_open_code;

static void keep_open_code2(void)
{
  g_open_code = g_app2.code;
}

static void shell_connect(void)
{
  app_connect_unstarted(&g_app2);
}

static void shell_as_notes(void)
{
  g_who = WHO_SHELL;
  app_open(&g_app2, NOTES, 4);
}

static void shell_resume(void)
{
  app_resume(&g_app2, "evil");
}

static void shell_leave(void)
{
  app_leave(&g_app2);
}

static void shell_gone(void)
{
  pnut_ui_disconnect(&g_app2.client);
  g_who = WHO_NAMED;
}

static int crash_count(FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_CRASHES; i++)
    {
      if (strcmp(g_ui.host.crashes[i].id, id) == 0)
        {
          return g_ui.host.crashes[i].count;
        }
    }

  return 0;
}

/* A token and crashes kept for an id, as the tables hold them */

static void plant_for(FAR const char *id, uint8_t crashes)
{
  FAR struct host_s *h = &g_ui.host;

  strlcpy(h->resumes[0].id, id, sizeof(h->resumes[0].id));
  h->resumes[0].len = 3;
  memcpy(h->resumes[0].token, "old", 3);
  h->resumes[0].order = ++h->resume_orders;
  strlcpy(h->crashes[0].id, id, sizeof(h->crashes[0].id));
  h->crashes[0].count = crashes;
}

static void plant_notes(void)
{
  plant_for(NOTES, HOST_CRASHED);
}

static void keep_crashes(void)
{
  FAR const struct host_resume_s *r = host_token(&g_ui.host, NOTES);

  g_crash_count = crash_count(NOTES);
  g_token_kept = r != NULL && r->len == 3 &&
                 memcmp(r->token, "old", 3) == 0;
}

static void test_shell_app_id(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, plant_notes },
    { 20, shell_connect },
    { 30, shell_as_notes },
    { 70, keep_open_code2 },
    { 80, shell_resume },
    { 90, shell_leave },
    { 100, shell_gone },
    { 150, keep_crashes },
    { 200, tile_notes },
    { 250, look_stopped },
    { 300, NULL },
  };

  run(steps, 11);
  assert_int_equal(g_app2.opened, PNUT_STATUS_DENIED);
  assert_int_equal(g_open_code, PNUT_UI_ERROR_CODE_BAD_ID);
  assert_true(g_token_kept);
  assert_int_equal(g_crash_count, HOST_CRASHED);
  assert_string_equal(g_top, "stopped");
  assert_int_equal(g_starts, 0);
}

/****************************************************************************
 * Name: test_shell_tables
 *
 * Description:
 *   A shell's program never touches the resume or crash tables: its
 *   Resume keeps nothing, its Leave forgets and clears nothing, its
 *   Welcome gives no token, the owner's Close forgets nothing of its id.
 ****************************************************************************/

static void plant(void)
{
  plant_for(DEV, 2);
}

static void shell_dev(void)
{
  g_who = WHO_SHELL;
  app_open(&g_app, DEV, 4);
}

static void dev_resume(void)
{
  app_resume(&g_app, "new");
}

static void dev_leave(void)
{
  app_leave(&g_app);
}

static FAR const struct host_resume_s *g_dev_token;
static int g_dev_crashes;

static void keep_dev(void)
{
  g_dev_token = host_token(&g_ui.host, DEV);
  g_dev_crashes = crash_count(DEV);
  if (g_dev_token != NULL)
    {
      assert_memory_equal(g_dev_token->token, "old", 3);
    }
}

static void test_shell_tables(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, plant },
    { 20, connect },
    { 30, shell_dev },
    { 80, show_dev },
    { 90, dev_resume },
    { 100, dev_leave },
    { 150, keep_dev },
    { 200, NULL },
  };

  static const struct step_s closed[] =
  {
    { 10, plant },
    { 20, connect },
    { 30, shell_dev },
    { 80, show_dev },
    { 100, key_back },
    { 150 + T_HUNG, soft_right },         /* Close */
    { 200 + T_HUNG, keep_dev },
    { 250 + T_HUNG + T_STOP, NULL },      /* Its deadline passed */
  };

  run(steps, 8);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_OPENED);
  assert_non_null(g_dev_token);
  assert_int_equal(g_dev_crashes, 2);

  g_dev_token = NULL;
  run(closed, 8);
  assert_int_equal(g_app.events[g_app.nevents - 1].which_kind,
                   PNUT_UI_EVENT_STOP_TAG);
  assert_non_null(g_dev_token);
  assert_int_equal(g_stops, 0);           /* Never stopped by its name */
}

/****************************************************************************
 * Name: test_dies_before_open
 *
 * Description:
 *   An app whose task exits before it opens (the services topic tells):
 *   its Busy off at once, and a crash (the owner, 2026-11-22); three in a
 *   row disable it.  The end of a task its start was not seen to run (an
 *   older one's, even told as stopping first) changes nothing.
 ****************************************************************************/

static void notes_task_exits(void)
{
  task_exits(NOTES);
}

/* An older task of Notes' slow to stop: signalled, then gone */

static void old_task_stopping(void)
{
  publish_service(NOTES, PNUT_SERVICE_STATE_STATE_STOPPING, FAKE_PID - 1);
}

static void keep_count(void)
{
  g_crash_count = crash_count(NOTES);
  keep_states();
}

static void test_dies_before_open(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 50, notes_task_exits },             /* Before its Busy */
    { 60, keep_count },
    { 100, tile_notes },
    { 120 + T_OPENING, look_busy },
    { 140 + T_OPENING, notes_task_exits },  /* Its Busy up */
    { 150 + T_OPENING, look2 },
    { 300, tile_notes },
    { 320, notes_task_exits },
    { 400, tile_notes },
    { 420, look_stopped },
    { 500, NULL },
  };

  static const struct step_s stale[] =
  {
    { 20, tile_notes },
    { 40, old_task_stopping },
    { 50, notes_task_exits },             /* Not this start's task */
    { 60, keep_count },
    { 100, NULL },
  };

  run(steps, 12);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_string_equal(g_top, "stopped");  /* Disabled */
  assert_string_equal(g_top2, "menu");    /* The Busy off at once */
  assert_int_equal(g_starts, 3);

  g_start_quiet = true;
  g_starts = 0;
  run(stale, 5);
  assert_int_equal(g_crash_count, 0);
  assert_int_equal(g_states[0], HOST_OPENING);
}

/****************************************************************************
 * Name: test_slot_until_gone
 *
 * Description:
 *   An app done with the UI (here, crashed) keeps its slot, stopping,
 *   until its task is gone: the services topic tells, or Service states
 *   stops it at Stop's deadline.  Its screen is off at once.
 ****************************************************************************/

static void drop_connection(void)
{
  pnut_ui_disconnect(&g_app.client);
}

/* Its connection gone, a key sent to it before the UI has read that: the
 * push fails, then its task exits
 */

static void drop_and_key(void)
{
  pnut_ui_disconnect(&g_app.client);
  key(KEYMAP_BACKSPACE, true);
  task_exits(NOTES);
}

static void test_slot_until_gone(void **state)
{
  static const struct step_s told[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, drop_connection },
    { 120, keep_count },
    { 130, look },
    { 150, notes_task_exits },
    { 170, look2 },
    { 180, keep_states },
    { 300, NULL },
  };

  static const struct step_s never[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, drop_connection },
    { 150 + T_STOP, keep_states },
    { 200 + T_STOP, NULL },
  };

  static const struct step_s topic_first[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, notes_task_exits },            /* Told before it closes */
    { 120, drop_connection },
    { 140, keep_states },
    { 200 + T_STOP, NULL },
  };

  static const struct step_s pushed[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 600, drop_and_key },
    { 700, keep_count },
    { 800 + T_STOP, NULL },
  };

  run(told, 11);
  assert_int_equal(g_crash_count, 1);
  assert_string_equal(g_top, "menu");
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_FREE);

  run(topic_first, 8);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_int_equal(g_stops, 0);

  run(pushed, 7);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_int_equal(g_stops, 0);

  g_stop_stays = true;
  run(never, 7);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_FREE);
}

/****************************************************************************
 * Name: test_retap_after_cancel
 *
 * Description:
 *   An app cancelled before it opened, its tile pressed again: its old
 *   task's late Open is told to stop, in the old task's slot, never taken
 *   for the new start's, and its end is not a crash.  The UI's Busy is up
 *   at once; the app is started again once the old task is gone.
 ****************************************************************************/

static int g_notes_slots;

static void count_notes_slots(void)
{
  int i;

  g_notes_slots = 0;
  for (i = 0; i < HOST_APPS; i++)
    {
      if (g_ui.host.slots[i].state != HOST_FREE &&
          strcmp(g_ui.host.slots[i].id, NOTES) == 0)
        {
          g_notes_slots++;
        }
    }
}

static void test_retap_after_cancel(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },                   /* Task 1 */
    { 30, connect },
    { 40, home_now },                     /* Cancelled: stopped */
    { 50, tile_notes },
    { 55, look_busy },                    /* "Opening" at once */
    { 60, open_app },                     /* Task 1's Open, late */
    { 85, count_notes_slots },
    { 90, keep_states },
    { 100, crash },                       /* Task 1 gone */
    { 120, keep_count },
    { 150, connect },                     /* Task 2 */
    { 160, open_app },
    { 200, show_inbox },
    { 250, look2 },
    { 300, NULL },
  };

  g_stop_stays = true;
  run(steps, 15);
  assert_int_equal(g_notes_slots, 1);     /* Its own slot, told to stop */
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Opening Notes\xe2\x80\xa6");
  assert_int_equal(g_crash_count, 0);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[0], HOST_OPENING);  /* Task 2's start */
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  assert_string_equal(g_top2, "app");
  assert_int_equal(g_depth2, 3);          /* Home, Menu, the app */
}

/****************************************************************************
 * Name: test_wait_stopping
 *
 * Description:
 *   The tile of an app still stopping (told to stop behind): "Opening" at
 *   once, the app started as soon as its old task is gone, its first
 *   screen in the Busy's place.  Home, or another app's tile (the newer
 *   tap wins), while it waits forgets it.
 ****************************************************************************/

static int g_starts_kept;
static bool g_opening_left;

static void keep_starts(void)
{
  g_starts_kept = g_starts;
}

static void keep_opening(void)
{
  g_opening_left = app_opening_screen(&g_ui, NULL) != NULL;
}

static void test_wait_stopping(void **state)
{
  static const struct step_s more[] =
  {
    { 220, home_now },                    /* Notes told to stop */
    { 260, tile_notes },
    { 265, look_busy },
    { 270, keep_starts },
    { 300, notes_exits },
    { 320, named_notes },
    { 330, connect },
    { 340, open_app },
    { 400, show_inbox },
    { 450, look2 },
    { 500, NULL },
  };

  static const struct step_s newer[] =
  {
    { 220, home_now },
    { 260, tile_notes },                  /* Waits */
    { 270, tile_todo },                   /* Todo, warm: Notes forgotten */
    { 290, show_todo },
    { 300, notes_exits },
    { 350, look },
    { 360, keep_opening },
    { 400, keep_starts },
    { 500, NULL },
  };

  static const struct step_s home[] =
  {
    { 220, home_now },
    { 260, tile_notes },
    { 270, home_now },                    /* Forgotten */
    { 280, look },
    { 300, notes_exits },
    { 400, keep_starts },
    { 500, NULL },
  };

  run_after(more, 11);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Opening Notes\xe2\x80\xa6");
  assert_int_equal(g_starts_kept, 2);     /* Notes waits */
  assert_int_equal(g_starts, 3);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  assert_string_equal(g_top2, "app");
  assert_string_equal(g_title, "Notes");

  g_starts = 0;
  run_after(home, 7);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_starts_kept, 2);

  g_starts = 0;
  run_after(newer, 9);
  assert_int_equal(g_starts_kept, 2);
  assert_string_equal(g_top, "app");
  assert_string_equal(g_title, "Today");
  assert_false(g_opening_left);
}

/****************************************************************************
 * Name: test_wait_no_slot
 *
 * Description:
 *   No slot free (one open, one warm, two still stopping): the tile's app
 *   shows "Opening" at once, and is started in the first slot freed.
 ****************************************************************************/

static void tile6(int n)
{
  int i;

  ui_push(&g_ui, &g_scr_menu, &g_six_menu);
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

static void tile_photos(void)
{
  tile6(3);
}

static void tile_maps(void)
{
  tile6(4);
}

static void reminders_exits(void)
{
  task_exits(REMINDERS);
}

static char g_started_kept[64];

static void keep_started(void)
{
  g_starts_kept = g_starts;
  strlcpy(g_started_kept, g_started, sizeof(g_started_kept));
}

static void test_wait_no_slot(void **state)
{
  static const struct step_s more[] =
  {
    { 220, tile_reminders },              /* Opening, not connected */
    { 240, tile_photos },                 /* Reminders cancelled */
    { 260, tile_maps },                   /* Photos too: no slot */
    { 270, look_busy },
    { 280, keep_started },
    { 300, reminders_exits },             /* A slot free */
    { 350, keep_states },
    { 400, NULL },
  };

  g_stop_stays = true;
  g_times.stop = 5000;
  write_manifest(PHOTOS, "notes", 1, g_list);
  write_manifest(MAPS, "notes", 1, g_list);
  run_after(more, 8);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_title, "Notes");  /* The manifests' name */
  assert_int_equal(g_starts_kept, 4);     /* Notes, Todo, Reminders, Photos */
  assert_string_equal(g_started_kept, PHOTOS);
  assert_int_equal(g_starts, 5);
  assert_string_equal(g_started, MAPS);
  assert_int_equal(g_states[2], HOST_FREE);
}

/****************************************************************************
 * Name: test_twin_not_stopped
 *
 * Description:
 *   Another task of an app open (one Service states names Notes, which the
 *   UI did not start) is told to stop; at its deadline, or taken off for
 *   events it never reads, Service states is not asked to stop Notes by
 *   name, which would stop the open one.  Two tasks told to stop are no
 *   twins of each other: the first's deadline stops the name.
 ****************************************************************************/

static void second_task(void)
{
  app_connect_unstarted(&g_app2);
}

static void second_opens(void)
{
  app_open(&g_app2, NULL, 4);
}

static void second_opens_stuck(void)
{
  app_open(&g_app2, NULL, 0);
}

static int g_stops_kept;

static void keep_stops(void)
{
  g_stops_kept = g_stops;
}

static void test_twin_not_stopped(void **state)
{
  static const struct step_s deadline[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, second_task },
    { 110, second_opens },                /* Told to stop */
    { 200 + T_STOP, keep_states },
    { 250 + T_STOP, NULL },
  };

  static const struct step_s stalled[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, second_task },
    { 110, second_opens_stuck },          /* Its Stop never read */
    { 200 + T_STALL, keep_states },
    { 250 + T_STALL, NULL },
  };

  static const struct step_s both[] =
  {
    { 20, connect },
    { 30, open_app },                     /* Not started here */
    { 100, second_task },
    { 110, second_opens },                /* Nor this one */
    { 60 + T_STOP, keep_stops },          /* The first's deadline */
    { 200 + T_STOP, NULL },
  };

  run(deadline, 8);
  assert_int_equal(g_app2.events[0].which_kind, PNUT_UI_EVENT_STOP_TAG);
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_OPEN);

  g_times.stop = 5000;
  run(stalled, 8);
  assert_int_equal(g_app2.ended, PNUT_STATUS_BUSY);
  assert_int_equal(g_stops, 0);
  assert_int_equal(g_states[0], HOST_OPEN);

  g_times.stop = T_STOP;
  g_stop_stays = true;
  run(both, 6);
  assert_int_equal(g_stops_kept, 1);
  assert_string_equal(g_stopped, NOTES);
}

/****************************************************************************
 * Name: test_shell_kept
 *
 * Description:
 *   A shell's program is the developer's: a newer tap does not cancel it
 *   while it has drawn nothing, Home keeps it warm undrawn, and its
 *   connection closing is no crash.
 ****************************************************************************/

static int g_dev_state;

static void keep_dev_state(void)
{
  int i;

  g_dev_state = HOST_FREE;
  for (i = 0; i < HOST_APPS; i++)
    {
      if (g_ui.host.slots[i].state != HOST_FREE &&
          strcmp(g_ui.host.slots[i].id, DEV) == 0)
        {
          g_dev_state = g_ui.host.slots[i].state;
        }
    }

  g_dev_crashes = crash_count(DEV);
}

static void test_shell_kept(void **state)
{
  static const struct step_s tapped[] =
  {
    { 20, connect },
    { 30, shell_dev },                    /* Open, nothing drawn */
    { 70, tile_todo },
    { 150, keep_dev_state },
    { 200, NULL },
  };

  static const struct step_s home[] =
  {
    { 20, connect },
    { 30, shell_dev },
    { 60, home_now },
    { 100, keep_dev_state },
    { 150, NULL },
  };

  static const struct step_s closed[] =
  {
    { 10, plant },
    { 20, connect },
    { 30, shell_dev },
    { 80, show_dev },
    { 100, drop_connection },
    { 150, keep_dev_state },
    { 200, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(tapped, 5);
  assert_int_equal(g_app.nevents, 1);     /* Welcome: never Stop */
  assert_int_equal(g_dev_state, HOST_OPEN);

  run(home, 5);
  assert_int_equal(g_app.nevents, 1);
  assert_int_equal(g_dev_state, HOST_WARM);

  run(closed, 7);
  assert_int_equal(g_dev_state, HOST_FREE);
  assert_int_equal(g_dev_crashes, 2);     /* As planted */
}

/****************************************************************************
 * Name: test_resume_leave_stop
 *
 * Description:
 *   An app told to stop behind that answers with Leave is coming back:
 *   its token is kept, and given back when it is started again.
 ****************************************************************************/

static void test_resume_leave_stop(void **state)
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
    { 240, leave },                       /* Its answer, then it exits */
    { 260, notes_exits },
    { 300, named_notes },
    { 310, tile_notes },
    { 320, connect },
    { 330, open_app },
    { 400, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 18);
  assert_int_equal(g_app.events[0].kind.welcome.reason,
                   PNUT_UI_WELCOME_REASON_RESUMED);
  assert_int_equal(g_app.events[0].kind.welcome.token.size, 7);
  assert_memory_equal(g_app.events[0].kind.welcome.token.bytes, "inbox/3",
                      7);
}

/****************************************************************************
 * Name: test_owed_drawn_now
 *
 * Description:
 *   An answer owed (here to Back), coming after the key's window, is
 *   drawn at once: not gathered as an app's change nobody's key caused.
 ****************************************************************************/

static void show_archive(void)
{
  static pnut_ui_show_request_t in;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX + 1;
  strcpy(in.title, "Archive");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 1;
  in.content.list.rows_count = 1;
  in.content.list.rows[0].id = 1;
  strcpy(in.content.list.rows[0].title, "Old");
  app_send_show(&g_app, &in);
}

static void test_owed_drawn_now(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 1000, key_back },                   /* Its window ends at 1500 */
    { 1600, mark },                       /* 0 */
    { 1700, show_archive },               /* Late, but owed */
    { 1800, mark },                       /* 1: drawn */
    { 1900, NULL },
  };

  times_long();
  g_nmarks = 0;
  run(steps, 9);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_int_equal(g_marks[1], g_marks[0] + 1);
}

/****************************************************************************
 * Name: test_page_owed
 *
 * Description:
 *   A page of rows the UI asked for is owed: unanswered by 3 s, "Waiting
 *   for Notes…"; answered with Rows in time, nothing.
 ****************************************************************************/

static void show_long(void)
{
  static pnut_ui_show_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = INBOX;
  strcpy(in.title, "Inbox");
  in.which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in.content.list.count = 40;
  in.content.list.rows_count = 20;
  for (i = 0; i < 20; i++)
    {
      in.content.list.rows[i].id = i + 1;
      snprintf(in.content.list.rows[i].title,
               sizeof(in.content.list.rows[i].title), "Row %d", i);
    }

  app_send_show(&g_app, &in);
}

static void down14(void)
{
  int i;

  for (i = 0; i < 14; i++)
    {
      key('s', false);
    }
}

/* A page of 20 rows from first, for a screen */

static void rows_send(uint32_t screen, uint32_t first)
{
  static pnut_ui_rows_request_t in;
  int i;

  memset(&in, 0, sizeof(in));
  in.screen = screen;
  in.first = first;
  in.rows_count = 20;
  for (i = 0; i < 20; i++)
    {
      in.rows[i].id = first + 1 + i;
      snprintf(in.rows[i].title, sizeof(in.rows[i].title), "Row %d",
               (int)first + i);
    }

  app_send_rows(&g_app, &in);
}

static void rows_page(void)
{
  rows_send(INBOX, 20);
}

static void test_page_owed(void **state)
{
  static const struct step_s unanswered[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_long },
    { 700, down14 },                      /* A page asked for */
    { 750 + T_BUSY, look_busy },
    { 800 + T_BUSY, NULL },
  };

  static const struct step_s answered[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_long },
    { 700, down14 },
    { 800, rows_page },
    { 750 + T_BUSY, look },
    { 800 + T_BUSY, NULL },
  };

  run(unanswered, 7);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");

  run(answered, 8);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top, "app");
}

/****************************************************************************
 * Name: test_rows_owed
 *
 * Description:
 *   Only a valid Rows for the screen on top answers the page owed (the
 *   owner, L24, as a refused Show answers nothing): one for another
 *   screen, or not from the row asked, is dropped as before, answered OK,
 *   and the page is still owed: "Waiting for Notes…" comes.  A valid one
 *   after it takes it off.
 ****************************************************************************/

static void rows_other(void)
{
  rows_send(INBOX + 1, 20);
}

static void rows_unasked(void)
{
  rows_send(INBOX, 10);
}

static void test_rows_owed(void **state)
{
  static const struct step_s other[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_long },
    { 700, down14 },                      /* A page asked for */
    { 800, rows_other },                  /* Another screen's */
    { 750 + T_BUSY, look_busy },
    { 800 + T_BUSY, NULL },
  };

  static const struct step_s unasked[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_long },
    { 700, down14 },
    { 800, rows_unasked },                /* Not from the row asked */
    { 750 + T_BUSY, look_busy },
    { 800 + T_BUSY, NULL },
  };

  static const struct step_s late[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_long },
    { 700, down14 },
    { 800 + T_BUSY, look_busy },          /* "Waiting for Notes…" */
    { 900 + T_BUSY, rows_page },          /* The page, late */
    { 1500 + T_BUSY, look2 },
    { 1600 + T_BUSY, NULL },
  };

  run(other, 8);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");

  g_busy[0] = '\0';
  run(unasked, 8);
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");

  g_busy[0] = '\0';
  run(late, 9);
  assert_string_equal(g_busy, "Waiting for Notes\xe2\x80\xa6");
  assert_int_equal(g_app.status, PNUT_STATUS_OK);
  assert_string_equal(g_top2, "app");
}

/****************************************************************************
 * Name: test_crash_table_full
 *
 * Description:
 *   With every crash count held by a disabled app, another app's crash
 *   is not counted: a disabled app is never forgotten to make room.
 ****************************************************************************/

static void plant_disabled(void)
{
  static const char *ids[HOST_CRASHES] =
  {
    NOTES, "org.example.a", "org.example.b", "org.example.c"
  };

  FAR struct host_s *h = &g_ui.host;
  int i;

  for (i = 0; i < HOST_CRASHES; i++)
    {
      strlcpy(h->crashes[i].id, ids[i], sizeof(h->crashes[i].id));
      h->crashes[i].count = HOST_CRASHED;
    }
}

static void todo_crashes(void)
{
  pnut_ui_disconnect(&g_app2.client);
  task_exits(TODO);
}

static int g_todo_crashes;

static void keep_todo_crashes(void)
{
  g_todo_crashes = crash_count(TODO);
}

static void test_crash_table_full(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, plant_disabled },
    { 20, tile_todo },
    { 30, connect_todo },
    { 40, open_todo },
    { 80, show_todo },
    { 100, todo_crashes },
    { 150, keep_todo_crashes },
    { 200, tile_notes },
    { 250, look_stopped },
    { 300, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  run(steps, 10);
  assert_int_equal(g_todo_crashes, 0);
  assert_string_equal(g_top, "stopped");
}

/****************************************************************************
 * Name: test_resume_table_full
 *
 * Description:
 *   With every token kept, a new one takes the place of the one given
 *   longest ago.
 ****************************************************************************/

static FAR const char *g_resume_ids[HOST_RESUMES] =
{
  "org.example.a", "org.example.b", "org.example.c", "org.example.d"
};

static bool g_kept[HOST_RESUMES + 1];

static void plant_tokens(void)
{
  FAR struct host_s *h = &g_ui.host;
  int i;

  for (i = 0; i < HOST_RESUMES; i++)
    {
      strlcpy(h->resumes[i].id, g_resume_ids[i], sizeof(h->resumes[i].id));
      h->resumes[i].len = 1;
      h->resumes[i].token[0] = 'x';
      h->resumes[i].order = ++h->resume_orders;
    }
}

static void keep_tokens(void)
{
  int i;

  for (i = 0; i < HOST_RESUMES; i++)
    {
      g_kept[i] = host_token(&g_ui.host, g_resume_ids[i]) != NULL;
    }

  g_kept[HOST_RESUMES] = host_token(&g_ui.host, NOTES) != NULL;
}

static void test_resume_table_full(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, plant_tokens },
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 90, resume_inbox },
    { 150, keep_tokens },
    { 200, NULL },
  };

  run(steps, 8);
  assert_false(g_kept[0]);                /* Given longest ago */
  assert_true(g_kept[1]);
  assert_true(g_kept[2]);
  assert_true(g_kept[3]);
  assert_true(g_kept[HOST_RESUMES]);
}

/****************************************************************************
 * Name: test_home_closes_options
 *
 * Description:
 *   Home with the app's options open closes them with its screen.
 ****************************************************************************/

static bool g_options_were;
static bool g_options_are;

static void keep_options_were(void)
{
  g_options_were = ui_options_open(&g_ui);
}

static void keep_options_are(void)
{
  g_options_are = ui_options_open(&g_ui);
}

static void test_home_closes_options(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox_options },
    { 700, options_key },
    { 750, keep_options_were },
    { 800, home_now },
    { 850, keep_options_are },
    { 900, NULL },
  };

  run(steps, 9);
  assert_true(g_options_were);
  assert_false(g_options_are);
}

/****************************************************************************
 * Name: test_switch_once
 *
 * Description:
 *   Only the app's first screen after a switch takes the full refresh
 *   (the owner: on the app's first screen): another of its screens is
 *   drawn as any other change.
 ****************************************************************************/

/* The updates and the full ones among them */

static void mark_both(void)
{
  g_marks[g_nmarks] = g_updates;
  g_fulls_at[g_nmarks++] = g_fulls;
}

static void test_switch_once(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },                   /* Its first screen: full */
    { 700, mark_both },
    { 800, show_archive },                /* Another of its screens */
    { 1900, mark_both },                  /* Drawn, not full */
    { 1910, look },
    { 2000, NULL },
  };

  times_long();
  g_nmarks = 0;
  run(steps, 9);
  assert_string_equal(g_title, "Archive");
  assert_true(g_fulls_at[0] > 0);
  assert_true(g_marks[1] > g_marks[0]);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
}

/****************************************************************************
 * Name: test_other_service
 *
 * Description:
 *   Another service's change on the services topic is not the app's:
 *   Telephony stopping while Notes opens is no crash of Notes, whose Busy
 *   stays.
 ****************************************************************************/

static void telephony_stops(void)
{
  task_exits("telephony");
}

static void test_other_service(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },                   /* Its task told starting */
    { 60 + T_OPENING, telephony_stops },  /* Its Busy up */
    { 80 + T_OPENING, look_busy },
    { 90 + T_OPENING, keep_count },
    { 200 + T_OPENING, NULL },
  };

  run(steps, 5);
  assert_int_equal(g_crash_count, 0);
  assert_int_equal(g_states[0], HOST_OPENING);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Opening Notes\xe2\x80\xa6");
}

/****************************************************************************
 * Name: test_shell_id_taken
 *
 * Description:
 *   A shell's program may not take an id a slot holds (the owner,
 *   2026-11-22: an app's, or a slot's): a second program opening as the
 *   first is refused, and the first stays open.
 ****************************************************************************/

static void second_as_dev(void)
{
  app_open(&g_app2, DEV, 4);
}

static void test_shell_id_taken(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, connect },
    { 30, shell_dev },                    /* Open as DEV */
    { 80, show_dev },
    { 100, shell_connect },
    { 110, second_as_dev },               /* Another program, as DEV */
    { 150, keep_open_code2 },
    { 160, keep_dev_state },
    { 200, NULL },
  };

  run(steps, 8);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_app2.opened, PNUT_STATUS_BUSY);
  assert_int_equal(g_open_code, PNUT_UI_ERROR_CODE_ANOTHER_APP);
  assert_int_equal(g_dev_state, HOST_OPEN);
  assert_int_equal(g_app.nevents, 1);     /* Welcome: never Stop */
}

/****************************************************************************
 * Name: test_wait_cancel
 *
 * Description:
 *   Cancel on the "Opening" of an app waiting to start (its old task still
 *   stopping) forgets it: it is not started when the old task is gone.
 *   Cancel once it has been started in a slot freed cancels that start:
 *   Service states stops it.  Either way a Cancel, not a Close: its
 *   resume token is kept (the owner: kept until it gives another, leaves
 *   or crashes).
 ****************************************************************************/

static void plant_notes_token(void)
{
  plant_for(NOTES, 0);
}

static void test_wait_cancel(void **state)
{
  static const struct step_s waiting[] =
  {
    { 210, plant_notes_token },
    { 220, home_now },                    /* Notes told to stop */
    { 260, tile_notes },                  /* Waits */
    { 270, soft_right },                  /* Cancel */
    { 280, look },
    { 300, notes_exits },
    { 350, keep_crashes },
    { 360, keep_starts },
    { 400, NULL },
  };

  static const struct step_s started[] =
  {
    { 210, plant_notes_token },
    { 220, home_now },
    { 260, tile_notes },
    { 300, notes_exits },                 /* Started in the slot freed */
    { 310, keep_starts },
    { 320, soft_right },                  /* Cancel, before its Busy time */
    { 330, look },
    { 340, keep_opening },
    { 350, keep_crashes },
    { 360, keep_states },
    { 400, NULL },
  };

  run_after(waiting, 9);
  assert_string_equal(g_top, "menu");
  assert_true(g_token_kept);
  assert_int_equal(g_starts_kept, 2);     /* Notes, Todo: not again */
  assert_int_equal(g_stops, 0);

  g_starts = 0;
  g_stop_stays = true;
  run_after(started, 11);
  assert_int_equal(g_starts_kept, 3);     /* Notes, Todo, Notes */
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);
  assert_int_equal(g_states[0], HOST_STOPPING);
  assert_false(g_opening_left);
  assert_string_equal(g_top, "menu");
  assert_true(g_token_kept);
}

/****************************************************************************
 * Name: test_stop_asked_once
 *
 * Description:
 *   Service states is asked to stop an app's task once for its slot: an
 *   app cancelled before it opened, or that never opened, whose task
 *   does not exit, is not asked again at Stop's deadline.
 ****************************************************************************/

static void test_stop_asked_once(void **state)
{
  static const struct step_s cancelled[] =
  {
    { 20, tile_notes },
    { 40, home_now },                     /* Cancelled: stopped */
    { 100 + T_STOP, NULL },               /* Past its deadline */
  };

  static const struct step_s unopened[] =
  {
    { 20, tile_notes },
    { 100 + T_OPEN + T_STOP, NULL },      /* No Open, then its deadline */
  };

  g_stop_stays = true;
  run(cancelled, 3);
  assert_int_equal(g_stops, 1);
  assert_string_equal(g_stopped, NOTES);

  g_stops = 0;
  run(unopened, 2);
  assert_int_equal(g_stops, 1);
}

/****************************************************************************
 * Name: test_no_topic
 *
 * Description:
 *   Without the services topic, an app done with the UI has its slot free
 *   at once: nothing would tell when its task is gone.  Nor does a new
 *   start wait for a task that outlived Stop's deadline.
 ****************************************************************************/

static void test_no_topic(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, drop_connection },             /* It crashed */
    { 120, keep_states },
    { 200 + T_STOP, NULL },
  };

  static const struct step_s told[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, cancel_open },                 /* Told to stop: deadline 300 */
    { 400, tile_notes },                  /* Past it, its task running */
    { 420, keep_starts },
    { 500, NULL },
  };

  g_svc_none = true;
  g_stop_stays = true;
  run(steps, 7);
  assert_int_equal(g_app.opened, PNUT_STATUS_OK);
  assert_int_equal(g_states[0], HOST_FREE);
  assert_int_equal(g_stops, 0);

  /* Nothing would tell when the task is gone: the app starts at once */

  g_starts = 0;
  run(told, 8);
  assert_int_equal(g_starts_kept, 2);
}

/****************************************************************************
 * Name: test_unsent_stops
 *
 * Description:
 *   An event that cannot be sent (one that does not encode; a send that
 *   fails closes the connection first) takes the app off: its stream
 *   ended, and its task stopped by Service states at once, as for events
 *   left waiting.
 ****************************************************************************/

/* A Welcome whose region has no end, as no event the UI makes should */

static void unsendable(void)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_WELCOME_TAG;
  memset(ev.kind.welcome.region, 'x', sizeof(ev.kind.welcome.region));
  host_event(&g_ui.host, &ev);
}

static void test_unsent_stops(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 600, unsendable },
    { 620, keep_stops },                  /* Before Stop's deadline */
    { 700, NULL },
  };

  g_stop_stays = true;
  run(steps, 7);
  assert_int_equal(g_app.ended, PNUT_STATUS_INTERNAL);
  assert_int_equal(g_stops_kept, 1);
  assert_string_equal(g_stopped, NOTES);
}

/****************************************************************************
 * Name: test_restarting_gone
 *
 * Description:
 *   RESTARTING on the services topic means the app's task is gone (the
 *   owner, L22): before the app opened, a crash, its Busy off; for an
 *   app cancelled and still stopping, its slot free before Stop's
 *   deadline.
 ****************************************************************************/

static int g_states_kept;

static void notes_restarting(void)
{
  publish_service(NOTES, PNUT_SERVICE_STATE_STATE_RESTARTING, 0);
}

static void keep_notes_state(void)
{
  g_states_kept = slot_state(NOTES);
}

static void test_restarting_gone(void **state)
{
  static const struct step_s opening[] =
  {
    { 20, tile_notes },                   /* Seen starting */
    { 50, notes_restarting },
    { 60, keep_count },
    { 100, NULL },
  };

  static const struct step_s stopping[] =
  {
    { 20, tile_notes },
    { 40, home_now },                     /* Cancelled: stopping */
    { 60, keep_notes_state },
    { 80, notes_restarting },
    { 100, keep_count },                  /* Before Stop's deadline */
    { 150, NULL },
  };

  run(opening, 4);
  assert_int_equal(g_crash_count, 1);
  assert_int_equal(g_states[0], HOST_FREE);

  g_stop_stays = true;
  g_times.stop = 5000;
  run(stopping, 6);
  assert_int_equal(g_states_kept, HOST_STOPPING);
  assert_int_equal(g_crash_count, 0);
  assert_int_equal(g_states[0], HOST_FREE);
}

/****************************************************************************
 * Name: test_still_running
 *
 * Description:
 *   At Stop's deadline, the log says what became of the task (the owner,
 *   L14): an app that left of itself, or whose connection closed, was
 *   never told to stop, and its task is "still running"; one told to
 *   stop "did not stop".  Either way Service states stops it then.
 ****************************************************************************/

/* It leaves, its task left running */

static void leave_running(void)
{
  app_leave(&g_app);
}

static void test_still_running(void **state)
{
  static const struct step_s left[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, leave_running },               /* Of itself */
    { 200 + T_STOP, NULL },
  };

  static const struct step_s closed[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, drop_connection },             /* Its task left running */
    { 200 + T_STOP, NULL },
  };

  static const struct step_s told[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, cancel_open },                 /* Told to stop */
    { 200 + T_STOP, NULL },
  };

  FAR const char *out;

  catch_start();
  run(left, 6);
  out = catch_end();
  assert_non_null(strstr(out, "app " NOTES " still running"));
  assert_null(strstr(out, "did not stop"));
  assert_int_equal(g_stops, 1);

  g_stops = 0;
  catch_start();
  run(closed, 6);
  out = catch_end();
  assert_non_null(strstr(out, "app " NOTES " still running"));
  assert_null(strstr(out, "did not stop"));
  assert_int_equal(g_stops, 1);

  g_stops = 0;
  g_stop_stays = true;
  catch_start();
  run(told, 6);
  out = catch_end();
  assert_non_null(strstr(out, "app " NOTES " did not stop"));
  assert_null(strstr(out, "still running"));
  assert_int_equal(g_stops, 1);
}

/****************************************************************************
 * Name: test_stopping_closes
 *
 * Description:
 *   An app told to stop whose connection then closes, its task still
 *   running, keeps the deadline it was given: Service states is asked to
 *   stop the task when that passes, not at one started anew at the close.
 ****************************************************************************/

static void test_stopping_closes(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, cancel_open },                 /* Told: its deadline at 1100 */
    { 600, drop_connection },             /* Its task still runs */
    { 1350, keep_stops },
    { 1400, NULL },
  };

  g_stop_stays = true;
  g_times.stop = 1000;
  run(steps, 8);
  assert_int_equal(g_stops_kept, 1);
}

/****************************************************************************
 * Name: test_second_task_stopping
 *
 * Description:
 *   A second task of an app told to stop, and still connected, opens: it
 *   is told to stop in a slot of its own, never in the connected one's,
 *   whose stream goes on.
 ****************************************************************************/

static void open_second(void)
{
  app_open(&g_app2, NULL, 4);             /* Who names Notes again */
}

static void test_second_task_stopping(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, cancel_open },                 /* Told to stop, connected */
    { 120, connect_todo },
    { 130, open_second },
    { 200, count_notes_slots },
    { 300, NULL },
  };

  g_stop_stays = true;
  g_times.stop = 5000;
  run(steps, 9);
  assert_int_equal(g_app2.opened, PNUT_STATUS_OK);
  assert_true(g_app2.nevents >= 1);
  assert_int_equal(g_app2.events[0].which_kind, PNUT_UI_EVENT_STOP_TAG);
  assert_int_equal(g_notes_slots, 2);
  assert_int_equal(g_app.ended, -1);      /* The first's stream goes on */
}

/****************************************************************************
 * Name: test_accept_fails
 *
 * Description:
 *   An app whose connection goes while Service states names it: its Open
 *   cannot be accepted, and its task is stopped at once, not at Stop's
 *   deadline.
 ****************************************************************************/

static void test_accept_fails(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },                     /* Named at 240 */
    { 100, drop_connection },
    { 500, keep_stops },
    { 600, NULL },
  };

  g_who_late = 200;
  g_stop_stays = true;
  g_times.stop = 3000;
  run(steps, 6);
  assert_int_equal(g_app.opened, -1);
  assert_int_equal(g_stops_kept, 1);
  assert_string_equal(g_stopped, NOTES);
}

/****************************************************************************
 * Name: test_open_deadline_busy
 *
 * Description:
 *   An app with no Open by its deadline, whose task does not exit when
 *   stopped: its Busy is off at once, though its slot waits for the task.
 ****************************************************************************/

static void test_open_deadline_busy(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 70 + T_OPENING, look_busy },        /* "Opening" */
    { 220 + T_OPEN, look2 },              /* Past the Open's deadline */
    { 230 + T_OPEN, keep_notes_state },
    { 300 + T_OPEN, NULL },
  };

  g_stop_stays = true;
  g_times.stop = 3000;
  run(steps, 5);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_top2, "menu");
  assert_int_equal(g_states_kept, HOST_STOPPING);
}

/****************************************************************************
 * Name: test_shell_broken_id
 *
 * Description:
 *   A shell's program under the id of an app whose manifest does not
 *   hold together is refused as under any app's id (the owner, L11 and
 *   L20): DENIED, BAD_ID.
 ****************************************************************************/

#define BROKEN  "org.example.broken"

static void shell_as_broken(void)
{
  g_who = WHO_SHELL;
  app_open(&g_app2, BROKEN, 4);
}

static void test_shell_broken_id(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, shell_connect },
    { 30, shell_as_broken },
    { 70, keep_open_code2 },
    { 100, NULL },
  };

  char path[160];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s.manifest", g_apps, BROKEN);
  f = fopen(path, "wb");
  assert_non_null(f);
  fputs("\xff\xff\xff\xff", f);
  fclose(f);

  run(steps, 4);
  assert_int_equal(g_app2.opened, PNUT_STATUS_DENIED);
  assert_int_equal(g_open_code, PNUT_UI_ERROR_CODE_BAD_ID);
}

/****************************************************************************
 * Name: test_switch_in_field
 *
 * Description:
 *   The draw that switches to an app takes its full refresh even when a
 *   text field has the keys on the app's screen (the owner, L21): its
 *   first screen, or a warm app's screen answering Shown.  No template
 *   here has a field: the step gives the screen the keys as one would.
 ****************************************************************************/

static void field_keys(void)
{
  ui_typing(ui_top(&g_ui), true);
}

static void test_switch_in_field(void **state)
{
  static const struct step_s first[] =
  {
    { 10, mark_fulls },
    { 20, tile_notes },                   /* The tile's key window */
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, field_keys },
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
    { 1450, show_inbox },
    { 1470, field_keys },
    { 2100, mark_fulls },
    { 2200, NULL },
  };

  times_long();
  g_nmarks = 0;
  run(first, 8);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0] + 1);

  g_nmarks = 0;
  run(shown, 11);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0] + 1);
}

/****************************************************************************
 * Name: test_switch_under_pin
 *
 * Description:
 *   An app's first screen drawn beneath the SIM PIN takes its full
 *   refresh when the PIN goes, not while the PIN is typed into (as built,
 *   the owner's L21).
 ****************************************************************************/

static void sim_ready(void)
{
  publish_sim(PNUT_TELEPHONY_INFO_SIM_READY);
}

static void pin_digit(void)
{
  key('1', false);
}

static void test_switch_under_pin(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, sim_locked },
    { 40, start_notes },
    { 200, connect },
    { 210, open_app },
    { 300, show_inbox },                  /* Beneath the PIN */
    { 400, look },
    { 500, mark_both },
    { 600, pin_digit },
    { 1300, mark_both },                  /* The PIN typed into */
    { 1400, sim_ready },                  /* The PIN goes */
    { 2000, look2 },
    { 2100, mark_both },
    { 2200, NULL },
  };

  times_long();
  g_nmarks = 0;
  run(steps, 13);
  assert_string_equal(g_top, "pin");
  assert_string_equal(g_top2, "app");
  assert_true(g_marks[1] > g_marks[0]);
  assert_int_equal(g_fulls_at[1], g_fulls_at[0]);
  assert_int_equal(g_fulls_at[2], g_fulls_at[1] + 1);
}

/****************************************************************************
 * Name: test_wait_past_deadline
 *
 * Description:
 *   An app cancelled before it opened, whose task outlives Stop's
 *   deadline, its tile pressed again: at the deadline its slot is free for
 *   other apps, but the new start waits, "Opening" up, until the services
 *   topic says the app's service stopped (the owner, L13).  The old task's
 *   late Open is told to stop, never taken for the new start's, and its
 *   end is no crash; the new start runs once the old task is gone.
 *   Without a late Open, the topic's STOPPED alone starts it.
 ****************************************************************************/

static int g_first_kind;

static void keep_first_kind(void)
{
  g_first_kind = g_app.nevents > 0 ? g_app.events[0].which_kind : -1;
  count_notes_slots();
}

static void test_wait_past_deadline(void **state)
{
  static const struct step_s late[] =
  {
    { 20, tile_notes },                   /* Task 1 */
    { 30, connect },
    { 40, home_now },                     /* Cancelled: its deadline 240 */
    { 50, tile_notes },                   /* Waits */
    { 55, look_busy },
    { 300, keep_starts },                 /* Past the deadline: waits */
    { 310, keep_opening },
    { 320, open_app },                    /* Task 1's Open, late */
    { 400, keep_first_kind },
    { 450, crash },                       /* Task 1 gone */
    { 480, keep_count },
    { 500, connect },                     /* Task 2 */
    { 510, open_app },
    { 550, show_inbox },
    { 600, look2 },
    { 700, NULL },
  };

  static const struct step_s topic[] =
  {
    { 20, tile_notes },
    { 40, home_now },
    { 50, tile_notes },
    { 300, keep_starts },
    { 350, notes_task_exits },            /* The topic: stopped */
    { 400, keep_count },
    { 500, NULL },
  };

  g_stop_stays = true;
  run(late, 16);
  assert_string_equal(g_top, "opening");
  assert_string_equal(g_busy, "Opening Notes\xe2\x80\xa6");
  assert_int_equal(g_starts_kept, 1);     /* Not at the deadline */
  assert_true(g_opening_left);
  assert_int_equal(g_first_kind, PNUT_UI_EVENT_STOP_TAG);
  assert_int_equal(g_notes_slots, 1);     /* Its own, told to stop */
  assert_int_equal(g_crash_count, 0);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_app.events[0].which_kind, PNUT_UI_EVENT_WELCOME_TAG);
  assert_string_equal(g_top2, "app");

  g_starts = 0;
  run(topic, 7);
  assert_int_equal(g_starts_kept, 1);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[0], HOST_OPENING);
}

/****************************************************************************
 * Name: test_missed_stopped
 *
 * Description:
 *   Where the services topic may have missed the old task's STOPPED,
 *   Service states' Get is asked: at a gap in the topic's numbers while
 *   the app waits, and at a tap of an app whose old task was last seen
 *   exiting, or of one waiting already.  Get saying it stopped starts the
 *   app.  With Get never answering, "Opening" stays until Cancel or Home
 *   (the owner, L13).
 ****************************************************************************/

static void notes_exits_missed(void)
{
  task_exits_missed(NOTES);
}

static void other_change(void)
{
  publish_service("telephony", PNUT_SERVICE_STATE_STATE_READY, 77);
}

static void test_missed_stopped(void **state)
{
  static const struct step_s gap[] =
  {
    { 20, tile_notes },
    { 40, home_now },                     /* Its deadline 240 */
    { 50, tile_notes },                   /* Waits */
    { 300, notes_exits_missed },
    { 330, keep_starts },
    { 350, other_change },                /* The gap seen */
    { 400, keep_count },
    { 500, NULL },
  };

  static const struct step_s tap[] =
  {
    { 20, tile_notes },
    { 40, home_now },
    { 300, notes_exits_missed },
    { 350, tile_notes },                  /* Past the deadline */
    { 400, keep_count },
    { 410, look_busy },
    { 500, NULL },
  };

  static const struct step_s retap[] =
  {
    { 20, tile_notes },
    { 40, home_now },                     /* Its deadline 240 */
    { 50, tile_notes },                   /* Waits; Get at 240 */
    { 300, notes_exits_missed },          /* No message after it */
    { 330, keep_starts },
    { 350, tile_notes },                  /* Tapped again: Get again */
    { 400, keep_count },
    { 500, NULL },
  };

  static const struct step_s silent[] =
  {
    { 20, tile_notes },
    { 40, home_now },
    { 300, notes_exits_missed },
    { 350, tile_notes },
    { 1500, keep_starts },                /* Past Get's time too */
    { 1510, keep_opening },
    { 1600, home_now },
    { 1650, look },
    { 1700, NULL },
  };

  g_stop_stays = true;
  run(gap, 8);
  assert_int_equal(g_starts_kept, 1);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[0], HOST_OPENING);

  g_starts = 0;
  g_gets = 0;
  run(tap, 7);
  assert_true(g_gets >= 1);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[0], HOST_OPENING);
  assert_string_equal(g_top, "opening");

  g_starts = 0;
  run(retap, 8);
  assert_int_equal(g_starts_kept, 1);
  assert_int_equal(g_starts, 2);
  assert_int_equal(g_states[0], HOST_OPENING);

  g_starts = 0;
  g_get_silent = true;
  run(silent, 9);
  assert_int_equal(g_starts_kept, 1);
  assert_true(g_opening_left);
  assert_string_equal(g_top, "home");
  assert_int_equal(g_starts, 1);
}

/****************************************************************************
 * Name: test_exiting_gone
 *
 * Description:
 *   An app told to stop whose task the topic saw exit, its connection
 *   still open at Stop's deadline: nothing is waited for, and its tile
 *   starts it at once, Service states not asked.
 ****************************************************************************/

static void test_exiting_gone(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },
    { 30, connect },
    { 40, open_app },
    { 80, show_inbox },
    { 100, notes_task_exits },            /* Seen gone, still connected */
    { 120, cancel_open },                 /* Told: its deadline 320 */
    { 400, tile_notes },
    { 420, keep_starts },
    { 500, NULL },
  };

  g_stop_stays = true;
  run(steps, 9);
  assert_int_equal(g_starts_kept, 2);
  assert_int_equal(g_gets, 0);
}

/****************************************************************************
 * Name: test_exiting_full
 *
 * Description:
 *   With every mark of an old task still exiting taken, the app marked
 *   longest ago gives its place: that app starts at once again, as
 *   before; the others still wait.  No app is refused for want of room.
 ****************************************************************************/

static bool g_marked[4];                /* Notes', Todo's, Reminders', Maps' */

static bool marked(FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_EXITING; i++)
    {
      if (strcmp(g_ui.host.exiting[i].id, id) == 0)
        {
          return true;
        }
    }

  return false;
}

static void plant_exiting(void)
{
  static const char *ids[4] =
  {
    TODO, REMINDERS, PHOTOS, MAPS
  };

  FAR struct host_s *h = &g_ui.host;
  int i;

  assert_int_equal(HOST_EXITING, 4);
  for (i = 0; i < HOST_EXITING; i++)
    {
      strlcpy(h->exiting[i].id, ids[i], sizeof(h->exiting[i].id));
      h->exiting[i].order = ++h->exiting_orders;
    }
}

static void keep_marks(void)
{
  g_marked[0] = marked(NOTES);
  g_marked[1] = marked(TODO);
  g_marked[2] = marked(REMINDERS);
  g_marked[3] = marked(MAPS);
}

static void test_exiting_full(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, plant_exiting },
    { 20, tile_notes },
    { 40, home_now },                     /* Its deadline 240 */
    { 300, keep_marks },
    { 320, tile_todo },                   /* No longer marked */
    { 340, keep_starts },
    { 400, NULL },
  };

  write_manifest(TODO, "reminders", 2, g_two);
  g_stop_stays = true;
  run(steps, 7);
  assert_true(g_marked[0]);
  assert_false(g_marked[1]);              /* Marked longest ago */
  assert_true(g_marked[2]);
  assert_true(g_marked[3]);
  assert_int_equal(g_starts_kept, 2);     /* Notes, then Todo at once */
}

/****************************************************************************
 * Name: test_get_stale
 *
 * Description:
 *   Service states' Get answers with the state as it was when asked: a
 *   late answer that an older task stopped never takes off the mark of a
 *   newer task still exiting.
 ****************************************************************************/

static void test_get_stale(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },                   /* Task 1 */
    { 40, home_now },                     /* Its deadline 240 */
    { 300, notes_exits_missed },
    { 320, tile_notes },                  /* Get B: stopped, at 720 */
    { 350, notes_task_exits },            /* Task 2 started */
    { 380, home_now },                    /* Its deadline 580 */
    { 390, tile_notes },                  /* Waits for task 2 */
    { 800, keep_starts },                 /* After Get B's answer */
    { 810, keep_opening },
    { 900, NULL },
  };

  g_stop_stays = true;
  g_get_late = 400;
  run(steps, 10);
  assert_int_equal(g_starts_kept, 2);
  assert_true(g_opening_left);
}

/****************************************************************************
 * Name: test_exiting_remarked
 *
 * Description:
 *   The app waiting for its old task, whose late Open is told to stop in
 *   a slot of its own: when that slot's deadline passes too, the app is
 *   marked anew, and Service states is asked again about it, as the topic
 *   may have missed its STOPPED.
 ****************************************************************************/

static void test_exiting_remarked(void **state)
{
  static const struct step_s steps[] =
  {
    { 20, tile_notes },                   /* Task 1 */
    { 30, connect },
    { 40, home_now },                     /* Its deadline 240: Get */
    { 50, tile_notes },                   /* Waits */
    { 300, open_app },                    /* Late: told, deadline 500 */
    { 350, notes_exits_missed },          /* No message after it */
    { 600, keep_starts },                 /* Marked anew at 500 */
    { 700, NULL },
  };

  g_stop_stays = true;
  run(steps, 8);
  assert_int_equal(g_starts_kept, 2);
}

/****************************************************************************
 * Name: test_gets_one
 *
 * Description:
 *   Service states' Get is asked one at a time: taps of the waiting app,
 *   and gaps in the topic, while a Get is unanswered ask again only once
 *   it is answered or its time is out, and a time out with nothing new
 *   asks nothing.  The services' calls stay free for other apps' starts.
 ****************************************************************************/

static int g_gets_kept;
static int g_gets_kept2;

static void keep_gets(void)
{
  g_gets_kept = g_gets;
}

static void keep_gets2(void)
{
  g_gets_kept2 = g_gets;
}

/* A change of another service lost, then one told: a gap */

static void topic_gap(void)
{
  g_svc_seq++;
  other_change();
}

static void test_gets_one(void **state)
{
  static const struct step_s taps[] =
  {
    { 20, tile_notes },                   /* Task 1 */
    { 40, home_now },                     /* Its deadline 240: Get */
    { 50, tile_notes },                   /* Waits */
    { 260, tile_notes },                  /* Tapped again, 12 times */
    { 270, tile_notes },
    { 280, tile_notes },
    { 290, tile_notes },
    { 300, tile_notes },
    { 310, tile_notes },
    { 320, tile_notes },
    { 330, tile_notes },
    { 340, tile_notes },
    { 350, tile_notes },
    { 360, tile_notes },
    { 370, tile_notes },
    { 380, keep_gets },
    { 400, tile_todo },                   /* Another app */
    { 450, keep_starts },
    { 500, NULL },
  };

  static const struct step_s gaps[] =
  {
    { 20, tile_notes },
    { 40, home_now },
    { 50, tile_notes },                   /* Get at 240, out at 640 */
    { 260, topic_gap },                   /* 10 gaps */
    { 270, topic_gap },
    { 280, topic_gap },
    { 290, topic_gap },
    { 300, topic_gap },
    { 310, topic_gap },
    { 320, topic_gap },
    { 330, topic_gap },
    { 340, topic_gap },
    { 350, topic_gap },
    { 380, keep_gets },
    { 700, keep_gets2 },                  /* Asked again at 640 */
    { 1100, keep_starts },                /* Its time out at 1040 */
    { 1110, tile_todo },
    { 1150, keep_count },
    { 1200, NULL },
  };

  FAR const char *out;

  write_manifest(TODO, "reminders", 2, g_two);
  g_stop_stays = true;
  g_get_silent = true;
  g_times.who = 400;
  catch_start();
  run(taps, 19);
  out = catch_end();
  assert_int_equal(g_gets_kept, 1);
  assert_int_equal(g_starts_kept, 2);     /* Notes, then Todo */
  assert_string_equal(g_started, TODO);
  assert_null(strstr(out, "not asked"));
  assert_null(strstr(out, "not started"));

  g_starts = 0;
  g_gets = 0;
  catch_start();
  run(gaps, 19);
  out = catch_end();
  assert_int_equal(g_gets_kept, 1);
  assert_int_equal(g_gets_kept2, 2);
  assert_int_equal(g_gets, 2);            /* None at the second's time */
  assert_int_equal(g_starts_kept, 1);
  assert_int_equal(g_states[1], HOST_OPENING);  /* Todo started */
  assert_null(strstr(out, "not asked"));
  assert_null(strstr(out, "not started"));
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
    cmocka_unit_test_setup_teardown(test_sheet_gathered, setup, teardown),
    cmocka_unit_test_setup_teardown(test_shell_app_id, setup, teardown),
    cmocka_unit_test_setup_teardown(test_shell_tables, setup, teardown),
    cmocka_unit_test_setup_teardown(test_dies_before_open, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_slot_until_gone, setup, teardown),
    cmocka_unit_test_setup_teardown(test_retap_after_cancel, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_wait_stopping, setup, teardown),
    cmocka_unit_test_setup_teardown(test_wait_no_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(test_twin_not_stopped, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_shell_kept, setup, teardown),
    cmocka_unit_test_setup_teardown(test_resume_leave_stop, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_owed_drawn_now, setup, teardown),
    cmocka_unit_test_setup_teardown(test_page_owed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_rows_owed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_crash_table_full, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_resume_table_full, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_home_closes_options, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_switch_once, setup, teardown),
    cmocka_unit_test_setup_teardown(test_other_service, setup, teardown),
    cmocka_unit_test_setup_teardown(test_shell_id_taken, setup, teardown),
    cmocka_unit_test_setup_teardown(test_wait_cancel, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stop_asked_once, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_no_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unsent_stops, setup, teardown),
    cmocka_unit_test_setup_teardown(test_restarting_gone, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_still_running, setup, teardown),
    cmocka_unit_test_setup_teardown(test_stopping_closes, setup, teardown),
    cmocka_unit_test_setup_teardown(test_second_task_stopping, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_accept_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(test_open_deadline_busy, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_shell_broken_id, setup, teardown),
    cmocka_unit_test_setup_teardown(test_switch_in_field, setup, teardown),
    cmocka_unit_test_setup_teardown(test_switch_under_pin, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_wait_past_deadline, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_missed_stopped, setup, teardown),
    cmocka_unit_test_setup_teardown(test_exiting_gone, setup, teardown),
    cmocka_unit_test_setup_teardown(test_exiting_full, setup, teardown),
    cmocka_unit_test_setup_teardown(test_get_stale, setup, teardown),
    cmocka_unit_test_setup_teardown(test_exiting_remarked, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_gets_one, setup, teardown),
  };

  return cmocka_run_group_tests_name("ui_life", tests, NULL, NULL);
}
