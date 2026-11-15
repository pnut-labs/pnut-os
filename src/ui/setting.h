/****************************************************************************
 * pnut-os/src/ui/setting.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_SETTING_H
#define __PNUT_OS_UI_SETTING_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/timer.h>

#include "pnut/settings.pnut.h"
#include "pnut/settings.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Settings watched at once, at most */

#define SETTING_MAX  16

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A setting of another program's (an owner's), by owner and key, and its
 * value once read (known)
 */

struct setting_s
{
  FAR const char *owner;
  FAR const char *key;
  pnut_setting_value_t value;
  bool known;
};

typedef CODE void (*setting_changed_t)(FAR void *arg, int index);

/* A change's answer: Settings' status, and its error code
 * (pnut.SettingsError) if it gave one
 */

typedef CODE void (*setting_done_t)(FAR void *arg, int status,
                                    uint32_t code);

struct setting_watch_s;

/* A change asked, not answered yet */

struct setting_call_s
{
  FAR struct setting_watch_s *w;
  setting_done_t done;
  FAR void *arg;
  bool used;
};

struct setting_watch_s
{
  FAR struct setting_s *list;
  uint8_t count;
  setting_changed_t changed;
  FAR void *arg;
  struct pnut_settings_client_s client;
  struct pnut_settings_change_reader_s reader;
  bool client_open;
  bool reader_open;
  bool reading;                   /* A Get is out */
  uint32_t pending;               /* The settings to read (bits) */
  uint32_t again;                 /* To read again after a while */
  FAR struct pnut_loop_s *loop;
  FAR struct pnut_timer_s *retry;
  uint8_t current;                /* The one being read */
  struct setting_call_s calls[PNUT_CLIENT_INFLIGHT];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Watch other programs' settings (RFC 0025): the system UI may read and
 * change every setting, through Settings' own calls (an owner's cache
 * would register the owner's schema, which only the owner may).  Each is
 * read as Settings is reached, and again when the settings topic tells of
 * a change to its owner; changed is told when a value is first known and
 * when it changes.  list is the caller's, kept.
 */

int setting_open(FAR struct setting_watch_s *w, FAR struct pnut_loop_s *loop,
                 FAR struct setting_s *list, uint8_t count,
                 setting_changed_t changed, FAR void *arg);
void setting_close(FAR struct setting_watch_s *w);

/* Ask Settings to change one: the value comes back through the topic.
 * -ENOTCONN while Settings cannot be reached; -EBUSY with too many calls
 * out.
 */

int setting_set_bool(FAR struct setting_watch_s *w, int index, bool value,
                     setting_done_t done, FAR void *arg);
int setting_set_text(FAR struct setting_watch_s *w, int index,
                     FAR const char *text, setting_done_t done,
                     FAR void *arg);

#endif /* __PNUT_OS_UI_SETTING_H */
