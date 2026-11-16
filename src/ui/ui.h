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

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ui_init(FAR struct ui_s *ui, FAR const struct ui_config_s *config);

/* Open a screen on top of the stack, with what it shows; take it off (not
 * Home)
 */

int ui_push(FAR struct ui_s *ui, FAR const struct screen_class_s *cls,
            FAR const void *arg);
void ui_pop(FAR struct ui_s *ui);
FAR struct screen_s *ui_top(FAR struct ui_s *ui);

/* A screen's options, over it (Proto-Options): W and S move, E, Enter and
 * the middle softkey choose, Back, the options' keys and a tap outside
 * close them.  chosen is told the index.
 */

int ui_options(FAR struct screen_s *s, FAR const char * const *items,
               uint8_t count,
               CODE void (*chosen)(FAR struct screen_s *s, int index));
bool ui_options_open(FAR struct ui_s *ui);

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
