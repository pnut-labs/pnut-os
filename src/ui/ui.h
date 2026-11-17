/****************************************************************************
 * pnut-os/src/ui/ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_UI_H
#define __PNUT_OS_UI_UI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/board.pnut.h"
#include "pnut/telephony.pnut.h"
#include "pnut/wifi.pnut.h"

#include "display.h"
#include "host.h"
#include "input.h"
#include "status.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Screens on the stack, at most; a screen's options, at most */

#define UI_DEPTH    8
#define UI_OPTIONS  8

/* Room for an app's text as drawn, every byte perhaps \xNN (app.c): the
 * short texts (titles, lines, labels, values, up to 64 bytes), four at
 * once; the long one (a line of up to 192 bytes)
 */

/* The apps' timings (RFC 0037), in milliseconds, the defaults of
 * struct ui_times_s
 */

#define UI_TIME_WHO       1000    /* Service states names an app */
#define UI_TIME_OPENING   700     /* Nothing drawn, then "Opening" */
#define UI_TIME_OPEN      10000   /* Open after a Start */
#define UI_TIME_BUSY      3000    /* An answer owed, then Busy */
#define UI_TIME_HUNG      10000   /* ... then "isn't responding" */
#define UI_TIME_STALL     3000    /* Events that cannot be sent */
#define UI_TIME_STOP      3000    /* Stop's deadline (RFC 0008) */
#define UI_TIME_GATHER    1000    /* Updates nobody's key caused */
#define UI_TIME_SPACING   2000    /* ... drawn at most this often */

#define UI_TEXTS       4
#define UI_TEXT_MAX    (64 * 4 + 1)
#define UI_TEXT_LONG   (192 * 4 + 1)

/* Told to a screen's status() beside status.h's: Wi-Fi's service came
 * (back), its state to read again; and all of it, as a screen comes back
 * on top
 */

#define UI_WIFI_SERVICE  0x10
#define UI_STATUS_ALL    (STATUS_TEL | STATUS_WIFI | STATUS_TIME | \
                          STATUS_BATTERY | UI_WIFI_SERVICE)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The apps' timings, in milliseconds (the UI_TIME_ defaults, which the
 * tests shorten)
 */

struct ui_times_s
{
  uint32_t who;
  uint32_t opening;
  uint32_t open;
  uint32_t busy;
  uint32_t hung;
  uint32_t stall;
  uint32_t stop;
  uint32_t gather;
  uint32_t spacing;
};

struct ui_config_s
{
  /* Board's service, which names the screen and the input devices
   * (RFC 0024); or NULL, and screen gives the frame (tests)
   */

  FAR const char *board;
  FAR const struct display_config_s *screen;

  FAR struct input_s *input;      /* Input, its own module */

  /* The device's: open the screen's framebuffer (fb_open), an input
   * device (input_dev_open).  NULL where there are none (tests).
   */

  CODE int (*open_screen)(FAR const char *path,
                          FAR struct display_config_s *config,
                          FAR size_t *len);
  CODE int (*open_input)(FAR struct input_s *in, FAR const char *path,
                         bool touch);

  /* The wall clock; NULL for time() (tests give a fixed one) */

  CODE time_t (*now)(void);

  /* Apps (RFC 0037): their manifests' folder, and Service states'
   * service, which names them; either NULL serves no apps
   */

  FAR const char *apps;
  FAR const char *services;

  /* The apps' timings; all 0: the defaults */

  struct ui_times_s times;
};

/* The system UI (RFC 0019): its screens, on LVGL, on the program's loop.
 * It asks Board for the screen and the input devices, opens the screen
 * and hands the input devices to Input.
 */

struct screen_s;

/* A screen's options, open over it */

struct ui_options_s
{
  FAR struct screen_s *screen;
  CODE void (*chosen)(FAR struct screen_s *s, int index);
  FAR lv_obj_t *box;
  FAR lv_obj_t *shadow;
  FAR lv_obj_t *catcher;          /* Takes a tap outside them */
  FAR lv_obj_t *rows[UI_OPTIONS];
  char letters[UI_OPTIONS];       /* An option's command letter, or 0 */
  uint8_t count;
  uint8_t focus;
};

struct ui_s
{
  struct pnut_module_s module;    /* First: "ui" */
  struct ui_config_s config;
  struct display_s display;
  struct ui_status_s status;
  bool status_open;
  FAR struct screen_s *stack[UI_DEPTH];
  uint8_t depth;
  struct ui_options_s options;    /* Open when box is not NULL */
  uint32_t serials;               /* The last screen's serial */
  struct pnut_wifi_client_s wifi; /* Shared by Wi-Fi's screens */
  bool wifi_open;
  struct pnut_telephony_client_s tel;   /* The SIM PIN screen's */
  bool tel_open;
  bool pin_skipped;               /* Skipped until the SIM asks anew */
  struct host_s host;             /* Apps' connections */
  bool host_open;

  /* An app's text as drawn: a template draws from these, one screen at a
   * time, and its labels keep their own copies
   */

  char texts[UI_TEXTS][UI_TEXT_MAX];
  char text_long[UI_TEXT_LONG];
  struct pnut_board_client_s board;
  FAR struct pnut_timer_s *retry; /* Asking Board again, once */
  bool board_open;
  bool shown;                     /* The screen is open */
  int fd;                         /* The framebuffer's, or -1 */
  FAR void *map;                  /* Its mapping, or NULL */
  size_t maplen;
};

/* A kind of screen: what it builds, how it takes keys and taps on its
 * blocks (true: handled; else Back and the right softkey pop it), what it
 * does when the device's state changes, what it wipes as it goes
 */

struct screen_class_s
{
  FAR const char *name;
  CODE void (*build)(FAR struct screen_s *s);
  CODE bool (*key)(FAR struct screen_s *s, uint8_t action, uint32_t ch);
  CODE void (*event)(FAR struct screen_s *s, uint16_t id);
  CODE void (*status)(FAR struct screen_s *s, uint8_t what);
  CODE void (*leave)(FAR struct screen_s *s);
  size_t size;                    /* Its state, zeroed */
};

/* A screen on the stack */

struct screen_s
{
  FAR const struct screen_class_s *cls;
  FAR struct ui_s *ui;
  FAR lv_obj_t *root;             /* LVGL's screen */
  FAR lv_obj_t *header;           /* Its header, if it has one */
  FAR const void *arg;            /* What it was opened with */
  uint16_t focus;                 /* The item focused (for tests) */
  uint32_t serial;                /* Its own, never reused: an answer that
                                   * comes after it went finds no screen */
  bool typing;                    /* A field of it has the keys */
  FAR void *state;                /* The class's, size bytes */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const struct screen_class_s g_scr_home;
extern const struct screen_class_s g_scr_menu;
extern const struct screen_class_s g_scr_unavailable;
extern const struct screen_class_s g_scr_settings;
extern const struct screen_class_s g_scr_wifi;
extern const struct screen_class_s g_scr_join;
extern const struct screen_class_s g_scr_cell;
extern const struct screen_class_s g_scr_pin;
extern const struct screen_class_s g_scr_app;     /* An app's (app.c) */
extern const struct screen_class_s g_scr_opening; /* scr_opening.c */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ui_init(FAR struct ui_s *ui, FAR const struct ui_config_s *config);

/* The UI_TIME_ defaults */

void ui_times_default(FAR struct ui_times_s *t);

/* Open a screen on top of the stack, with what it shows; take it off (not
 * Home)
 */

int ui_push(FAR struct ui_s *ui, FAR const struct screen_class_s *cls,
            FAR const void *arg);
void ui_pop(FAR struct ui_s *ui);
FAR struct screen_s *ui_top(FAR struct ui_s *ui);

/* A screen of the stack replaced, in its place, by a new one of a class
 * (shown if it is on top); a screen taken off wherever it is (not Home).
 * -ENOENT for a screen not on the stack.
 */

int ui_replace(FAR struct ui_s *ui, FAR struct screen_s *old,
               FAR const struct screen_class_s *cls, FAR const void *arg);
void ui_remove(FAR struct ui_s *ui, FAR struct screen_s *s);

/* A new screen of a class put at an index of the stack, under those
 * above, and not shown (an app's screen beneath the SIM PIN); at the top,
 * or past it, it is pushed
 */

int ui_insert(FAR struct ui_s *ui, int index,
              FAR const struct screen_class_s *cls, FAR const void *arg);

/* A screen's options, over it (Proto-Options): W and S move, E, Enter and
 * the middle softkey choose, Back, the options' keys and a tap outside
 * close them.  chosen is told the index.
 */

int ui_options(FAR struct screen_s *s, FAR const char * const *items,
               uint8_t count,
               CODE void (*chosen)(FAR struct screen_s *s, int index));

/* The same, each option with its command letter (lowercase, or 0 for
 * none), drawn at its right; the letter's key chooses it (RFC 0037)
 */

int ui_options_lettered(FAR struct screen_s *s,
                        FAR const char * const *items,
                        FAR const char *letters, uint8_t count,
                        CODE void (*chosen)(FAR struct screen_s *s,
                                            int index));
bool ui_options_open(FAR struct ui_s *ui);

/* A screen's options closed, nothing chosen (a sheet comes over them) */

void ui_options_shut(FAR struct ui_s *ui, FAR struct screen_s *s);

/* Lay the top screen out after it changed outside its key and tap
 * handlers (an answer, a timer), so that a tap finds its blocks
 */

void ui_place(FAR struct ui_s *ui);

/* The screen of that serial, still on the stack; or NULL */

FAR struct screen_s *ui_find(FAR struct ui_s *ui, uint32_t serial);

/* Wi-Fi's client, connected the first time it is asked for; NULL if it
 * cannot be.  A call's answer finds its screen with ui_find().
 */

FAR struct pnut_wifi_client_s *ui_wifi(FAR struct ui_s *ui);
FAR struct pnut_telephony_client_s *ui_telephony(FAR struct ui_s *ui);

/* Whether a text field of the screen has the keys (letters type): kept
 * with the screen, and given back to it when it is on top again
 */

void ui_typing(FAR struct screen_s *s, bool typing);

/* A screen's header, kept as the device's state is */

FAR lv_obj_t *ui_header(FAR struct screen_s *s, FAR const char *icon,
                        FAR const char *title);

/* The time now, local */

void ui_local(FAR struct ui_s *ui, FAR struct tm *tm);

#endif /* __PNUT_OS_UI_UI_H */
