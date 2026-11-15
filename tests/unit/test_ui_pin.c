/****************************************************************************
 * pnut-os/tests/unit/test_ui_pin.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The SIM PIN screen (scr_pin.c): shown over any screen when Telephony's
 * topic says the SIM needs its PIN or PUK, the code typed with the letter
 * keys' digits, sent to a fake Telephony, wiped.  The pictures go to
 * SCREENS_DIR, as test_ui_screens'.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/telephony.pnut.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"

#include "blocks.h"
#include "input.h"
#include "keymap.h"
#include "text.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define STRIDE   (WIDTH / 8)

/* 2026-09-22 12:07:00 UTC: 14:07 in Warsaw */

#define PIN_NOW   1790078820
#define PIN_RULE  "CET-1CEST,M3.5.0,M10.5.0/3"

#ifndef SCREENS_DIR
#  define SCREENS_DIR  "screens"
#endif

/* The bullet the code is shown with, and a code of four */

#define DOT       "\xe2\x80\xa2"
#define DOTS4     DOT " " DOT " " DOT " " DOT

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct step_s
{
  uint32_t at;
  CODE void (*run)(void);
};

/* The start of the screen's state: its code and the PUK kept for the new
 * PIN (scr_pin.c)
 */

struct pin_state_s
{
  char code[9];
  char puk[9];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_frame[STRIDE * HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static struct pnut_module_s g_owners;
static struct pnut_telephony_server_s g_server;
static FAR struct pnut_publisher_s *g_tel;
static FAR struct pnut_publisher_s *g_time;
static time_t g_now = PIN_NOW;

static int g_pins;
static int g_puks;
static char g_pin[9];
static char g_puk[9];
static bool g_refuse;
static FAR struct pin_state_s *g_state;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fake_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                       int16_t h, bool full)
{
  return 0;
}

static const struct display_ops_s g_ops =
{
  fake_update
};

static time_t fixed_now(void)
{
  return g_now;
}

/* The fake Telephony */

static void fake_status(FAR struct pnut_telephony_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_telephony_status_request_t *in,
                        FAR void *arg)
{
  pnut_telephony_fail(server, req, PNUT_STATUS_NOTFOUND, 0, "not here");
}

static void fake_answer(FAR struct pnut_telephony_server_s *server,
                        FAR const struct pnut_request_s *req, bool puk)
{
  pnut_telephony_reply_t out;

  memset(&out, 0, sizeof(out));
  if (g_refuse)
    {
      pnut_telephony_fail(server, req, PNUT_STATUS_DENIED,
                          PNUT_TELEPHONY_ERROR_CODE_REFUSED, "refused");
    }
  else if (puk)
    {
      pnut_telephony_enter_puk_reply(server, req, PNUT_STATUS_OK, &out);
    }
  else
    {
      pnut_telephony_enter_pin_reply(server, req, PNUT_STATUS_OK, &out);
    }
}

static void fake_pin(FAR struct pnut_telephony_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_telephony_pin_request_t *in,
                     FAR void *arg)
{
  g_pins++;
  strcpy(g_pin, in->pin);
  fake_answer(server, req, false);
}

static void fake_puk(FAR struct pnut_telephony_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_telephony_puk_request_t *in,
                     FAR void *arg)
{
  g_puks++;
  strcpy(g_puk, in->puk);
  strcpy(g_pin, in->pin);
  fake_answer(server, req, true);
}

static void fake_command(FAR struct pnut_telephony_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_telephony_command_request_t *in,
                         FAR void *arg)
{
  pnut_telephony_fail(server, req, PNUT_STATUS_DENIED, 0, "no");
}

static const struct pnut_telephony_handlers_s g_handlers =
{
  fake_status,
  fake_pin,
  fake_puk,
  fake_command
};

static int owners_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  pnut_module_ready(module);
  return pnut_telephony_serve(loop, "telephony", 4, &g_handlers, NULL,
                              &g_server);
}

static void owners_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  pnut_telephony_close(&g_server);
}

/* The topics: the SIM's state, tries; Warsaw */

static void publish_sim(int sim, uint32_t tries)
{
  pnut_telephony_info_t tel;

  memset(&tel, 0, sizeof(tel));
  tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.sim   = sim;
  tel.tries = tries;
  if (sim == PNUT_TELEPHONY_INFO_SIM_READY)
    {
      tel.registration = PNUT_TELEPHONY_INFO_REGISTRATION_HOME;
      tel.tech         = PNUT_TELEPHONY_INFO_TECH_LTE;
      tel.bars         = 3;
    }

  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);
}

static void publish_time(void)
{
  pnut_time_change_t time;

  memset(&time, 0, sizeof(time));
  strcpy(time.zone, "Europe/Warsaw");
  strcpy(time.rule, PIN_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &time), 0);
}

static void step_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR const struct step_s *step = arg;

  if (step->run != NULL)
    {
      step->run();
    }
  else
    {
      pnut_loop_stop(loop, 0);
    }
}

static void run(FAR const struct step_s *steps, int nsteps)
{
  struct pnut_loop_config_s lconfig;
  struct display_config_s screen;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-uipin-XXXXXX";
  char cmd[64];
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  lconfig.timers = 48;            /* The steps', the UI's */
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);
  assert_int_equal(pnut_telephony_info_advertise(loop, &g_tel), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &g_time), 0);

  memset(&g_owners, 0, sizeof(g_owners));
  g_owners.name  = "owners";
  g_owners.start = owners_start;
  g_owners.stop  = owners_stop;
  assert_int_equal(pnut_module_add(loop, &g_owners), 0);

  memset(g_frame, 0xff, sizeof(g_frame));
  memset(&screen, 0, sizeof(screen));
  screen.frame  = g_frame;
  screen.stride = STRIDE;
  screen.width  = WIDTH;
  screen.height = HEIGHT;
  screen.ops    = &g_ops;

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &g_ui.display;
  input_init(&g_input, &iconfig);
  input_set_layout(&g_input, "blackberry-q20");

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.screen = &screen;
  uconfig.input  = &g_input;
  uconfig.now    = fixed_now;
  ui_init(&g_ui, &uconfig);
  assert_int_equal(pnut_module_add(loop, &g_ui.module), 0);
  assert_int_equal(pnut_module_add(loop, &g_input.module), 0);

  g_pins   = 0;
  g_puks   = 0;
  g_refuse = false;
  memset(g_pin, 0, sizeof(g_pin));
  memset(g_puk, 0, sizeof(g_puk));
  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_topic_unadvertise(g_tel);
  pnut_topic_unadvertise(g_time);
  pnut_loop_destroy(loop);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  assert_int_equal(system(cmd), 0);
}

static void key(uint32_t code, bool special)
{
  input_key(&g_input, code, special, true);
  input_key(&g_input, code, special, false);
}

static void type(FAR const char *text)
{
  while (*text != '\0')
    {
      key((uint8_t)*text++, false);
    }
}

static FAR const char *top(void)
{
  return ui_top(&g_ui)->cls->name;
}

static bool shown_in(FAR lv_obj_t *obj, FAR const char *text)
{
  uint32_t i;

  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    {
      return false;
    }

  if (lv_obj_check_type(obj, &lv_label_class) &&
      strcmp(lv_label_get_text(obj), text) == 0)
    {
      return true;
    }

  for (i = 0; i < lv_obj_get_child_count(obj); i++)
    {
      if (shown_in(lv_obj_get_child(obj, i), text))
        {
          return true;
        }
    }

  return false;
}

static bool shows(FAR const char *text)
{
  return shown_in(ui_top(&g_ui)->root, text);
}

static void zeroed(FAR const char *buf, size_t size)
{
  size_t i;

  for (i = 0; i < size; i++)
    {
      assert_int_equal(buf[i], 0);
    }
}

/* The frame as a PBM picture (P4: a set bit black, the panel's white) */

static void save(FAR const char *name)
{
  char path[128];
  FILE *f;
  size_t i;

  display_draw_now(&g_ui.display);
  mkdir(SCREENS_DIR, 0755);
  snprintf(path, sizeof(path), "%s/%s.pbm", SCREENS_DIR, name);
  f = fopen(path, "wb");
  assert_non_null(f);
  fprintf(f, "P4\n%d %d\n", WIDTH, HEIGHT);
  for (i = 0; i < sizeof(g_frame); i++)
    {
      fputc((uint8_t)~g_frame[i], f);
    }

  fclose(f);
}

/* test_pin_entered: the screen over Home; the letter keys' digits; too
 * short; refused; taken, and wiped
 */

static void entered_locked(void)
{
  publish_time();
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 3);
}

static void entered_shown(void)
{
  assert_string_equal(top(), "pin");
  assert_true(shows("3 of 3"));
  type("wer");
  key(KEYMAP_ENTER, true);
  assert_true(shows(UI_T_PIN_LENGTH));
  assert_int_equal(g_pins, 0);
  type("s");
  assert_true(shows(DOTS4));
  save("pin");
  g_refuse = true;
  g_state  = ui_top(&g_ui)->state;
  key(KEYMAP_F2, true);
  zeroed(g_state->code, sizeof(g_state->code));
}

static void entered_refused(void)
{
  assert_int_equal(g_pins, 1);
  assert_string_equal(g_pin, "1234");
  assert_true(shows(UI_T_WRONG_PIN));
  assert_false(shows(DOTS4));
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 2);
}

static void entered_again(void)
{
  assert_true(shows("2 of 3"));
  save("pin-wrong");
  g_refuse = false;
  type("1234");
  key(KEYMAP_ENTER, true);
}

static void entered_taken(void)
{
  assert_int_equal(g_pins, 2);
  zeroed(g_state->code, sizeof(g_state->code));

  /* The SIM ready: the screen goes */

  publish_sim(PNUT_TELEPHONY_INFO_SIM_READY, 0);
}

static void entered_gone(void)
{
  assert_string_equal(top(), "home");
}

static void test_pin_entered(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, entered_locked },
    { 500, entered_shown },
    { 900, entered_refused },
    { 1300, entered_again },
    { 1700, entered_taken },
    { 2100, entered_gone },
    { 2200, NULL },
  };

  run(steps, 7);
}

/* test_pin_puk: the PUK, then a new PIN, both sent together */

static void puk_locked(void)
{
  publish_time();
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK, 10);
}

static void puk_typed(void)
{
  assert_string_equal(top(), "pin");
  assert_true(shows(UI_T_SIM_PUK_HEAD));
  type("1234");
  key(KEYMAP_ENTER, true);
  assert_true(shows(UI_T_PUK_LENGTH));
  type("5678");
  save("puk");
  key(KEYMAP_ENTER, true);
  assert_true(shows(UI_T_SIM_NEW_PIN_HEAD));
  assert_int_equal(g_puks, 0);
  g_state = ui_top(&g_ui)->state;
  type("4321");
  key(KEYMAP_ENTER, true);
  zeroed(g_state->code, sizeof(g_state->code));
  zeroed(g_state->puk, sizeof(g_state->puk));
}

static void puk_sent(void)
{
  assert_int_equal(g_puks, 1);
  assert_string_equal(g_puk, "12345678");
  assert_string_equal(g_pin, "4321");
}

static void test_pin_puk(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, puk_locked },
    { 500, puk_typed },
    { 900, puk_sent },
    { 1000, NULL },
  };

  run(steps, 4);
}

/* test_pin_to_puk: the last try of the PIN refused, the SIM asks for its
 * PUK: still told why
 */

static void topuk_locked(void)
{
  publish_time();
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 1);
}

static void topuk_wrong(void)
{
  g_refuse = true;
  type("1234");
  key(KEYMAP_ENTER, true);
}

static void topuk_asks(void)
{
  assert_true(shows(UI_T_WRONG_PIN));
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK, 10);
}

static void topuk_shown(void)
{
  assert_true(shows(UI_T_SIM_PUK_HEAD));
  assert_true(shows(UI_T_WRONG_PIN));
}

static void test_pin_to_puk(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, topuk_locked },
    { 500, topuk_wrong },
    { 900, topuk_asks },
    { 1300, topuk_shown },
    { 1400, NULL },
  };

  run(steps, 5);
}

/* test_pin_skipped: Skip; not again until the SIM asks anew; blocked */

static void skipped_locked(void)
{
  publish_time();
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 3);
}

static void skipped_skip(void)
{
  assert_string_equal(top(), "pin");
  key(KEYMAP_F3, true);
  assert_string_equal(top(), "home");
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 2);
}

static void skipped_stays(void)
{
  assert_string_equal(top(), "home");
  publish_sim(PNUT_TELEPHONY_INFO_SIM_SIM_UNKNOWN, 0);
}

static void skipped_anew(void)
{
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 2);
}

static void skipped_back(void)
{
  assert_string_equal(top(), "pin");
  publish_sim(PNUT_TELEPHONY_INFO_SIM_BLOCKED, 0);
}

static void skipped_blocked(void)
{
  assert_string_equal(top(), "pin");
  assert_true(shows(UI_T_SIM_BLOCKED));
  type("1234");
  assert_false(shows(DOTS4));
  key(KEYMAP_F2, true);
  assert_int_equal(g_pins, 0);
}

static void test_pin_skipped(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, skipped_locked },
    { 500, skipped_skip },
    { 900, skipped_stays },
    { 1300, skipped_anew },
    { 1700, skipped_back },
    { 2100, skipped_blocked },
    { 2200, NULL },
  };

  run(steps, 7);
}

/* test_pin_typing_back: the screen below typed in a field; it types again
 * once the PIN screen goes
 */

static void typer_build(FAR struct screen_s *s)
{
  ui_header(s, NULL, "Typing");
  block_softkeys(s->root, "", "", "Back");
  ui_typing(s, true);
}

static const struct screen_class_s g_typer =
{
  "typer", typer_build, NULL, NULL, NULL, NULL, 0
};

static void typing_open(void)
{
  publish_time();
  ui_push(&g_ui, &g_typer, NULL);
  assert_true(g_input.state.typing);
  publish_sim(PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN, 3);
}

static void typing_over(void)
{
  assert_string_equal(top(), "pin");
  publish_sim(PNUT_TELEPHONY_INFO_SIM_READY, 0);
}

static void typing_back(void)
{
  assert_string_equal(top(), "typer");
  assert_true(g_input.state.typing);
}

static void test_pin_typing_back(void **state)
{
  static const struct step_s steps[] =
  {
    { 300, typing_open },
    { 700, typing_over },
    { 1100, typing_back },
    { 1200, NULL },
  };

  run(steps, 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_pin_entered),
    cmocka_unit_test(test_pin_puk),
    cmocka_unit_test(test_pin_to_puk),
    cmocka_unit_test(test_pin_skipped),
    cmocka_unit_test(test_pin_typing_back),
  };

  return cmocka_run_group_tests_name("ui_pin", tests, NULL, NULL);
}
