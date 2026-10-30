/****************************************************************************
 * pnut-os/src/lib/include/pnut/settings_cache.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_SETTINGS_CACHE_H
#define __PNUT_OS_LIB_PNUT_SETTINGS_CACHE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/client.h>
#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/settings.pb.h"
#include "pnut/settings.pnut.h"
#include "pnut/settings.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Settings' socket, in the run directory */

#define PNUT_SETTINGS_CACHE_SERVICE  "settings"

/* The most settings an owner's cache holds */

#define PNUT_SETTINGS_CACHE_MAX  64

/* The index a change handler is given when it cannot tell which setting
 * changed: the values were read anew
 */

#define PNUT_SETTINGS_CACHE_ALL  (-1)

/* A module's log level, a setting of the owner named as the module
 * (RFC 0026): a choice of "error", "warning", "info" and "debug"
 */

#define PNUT_SETTINGS_CACHE_LEVEL  "log.level"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* How a setting's value is kept in its owner's struct */

enum pnut_setting_kind_e
{
  PNUT_SETTING_KIND_BOOL = 0,     /* bool */
  PNUT_SETTING_KIND_INT32,        /* int32_t */
  PNUT_SETTING_KIND_UINT32,       /* uint32_t */
  PNUT_SETTING_KIND_INT64,        /* int64_t */
  PNUT_SETTING_KIND_TEXT,         /* char[size], NUL-terminated */
};

/* A setting's declaration, as the generator writes it from a settings
 * message's field (options.proto): what RegisterSchema is told, and where
 * the value goes in the message's struct
 */

struct pnut_setting_decl_s
{
  FAR const char *key;
  FAR const char *label;
  FAR const char *group;
  FAR const char *unit;
  FAR const char * const *choices;
  FAR const char *text;           /* A string's or choice's default */
  int64_t integer;                /* A boolean's or integer's default */
  int64_t min;
  int64_t max;
  uint32_t max_length;
  uint16_t offset;                /* In the owner's struct */
  uint16_t size;
  uint8_t nchoices;
  uint8_t type;                   /* pnut_setting_type_t */
  uint8_t kind;                   /* enum pnut_setting_kind_e */
  bool has_default;
  bool is_public;
};

/* An owner's settings: the generator writes one for each settings message */

struct pnut_settings_desc_s
{
  FAR const char *owner;
  FAR const struct pnut_setting_decl_s *decls;
  uint8_t count;
};

struct pnut_settings_cache_s;
struct pnut_settings_link_s;

/* Called on the loop when a setting's value in the struct changed, or with
 * PNUT_SETTINGS_CACHE_ALL once the values are first read
 */

typedef CODE void
  (*pnut_settings_changed_t)(FAR struct pnut_settings_cache_s *cache,
                             int index, FAR void *arg);

/* Called on the loop with a change's answer: Settings' status, and its
 * error code (pnut.SettingsError) if it gave one
 */

typedef CODE void
  (*pnut_settings_done_t)(FAR struct pnut_settings_cache_s *cache,
                          int status, uint32_t code, FAR void *arg);

/* A change asked of Settings and not answered yet */

struct pnut_settings_set_s
{
  FAR struct pnut_settings_cache_s *cache;
  pnut_settings_done_t done;
  FAR void *arg;
};

/* A program's cache of one owner's settings (RFC 0025): it registers the
 * owner's schema with Settings, reads the values into the owner's struct,
 * and reads them again whenever the settings topic tells of a change, or
 * of changes missed, or Settings comes back.  Reads are plain reads of the
 * struct, on the loop; changes go to Settings, and come back through the
 * topic.  A program's caches share one connection to Settings and one
 * reader of the topic (RFC 0023).
 *
 * An owner named as its module holds the module's log level too, the
 * setting PNUT_SETTINGS_CACHE_LEVEL (RFC 0026): the cache registers it
 * with the owner's schema, and sets the module's level from its value.
 * It is not in the struct, and the change handler is not told of it.
 */

struct pnut_settings_cache_s
{
  /* The library's */

  FAR struct pnut_module_s *module;
  FAR const struct pnut_settings_desc_s *desc;
  FAR void *values;
  pnut_settings_changed_t changed;
  FAR void *arg;

  FAR struct pnut_settings_link_s *link;

  /* The link's next cache */

  FAR struct pnut_settings_cache_s *flink;
  FAR struct pnut_timer_s *timer; /* The next try, once */
  uint32_t backoff;

  uint8_t state;
  uint8_t next;                   /* The next declaration to register */
  bool ready;                     /* The values have been read */
  bool again;                     /* A change came while reading */
  bool level;                     /* Holds the module's log level */

  /* Settings had no room, and the log was told: kept across a lost
   * connection, so that the log hears when there is room
   */

  bool full;
  uint64_t touched;               /* The values that changed in a read */
  char after[sizeof(((FAR pnut_settings_page_request_t *)0)->after)];

  struct pnut_settings_set_s sets[PNUT_CLIENT_INFLIGHT];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: pnut_settings_cache_open
 *
 * Description:
 *   Start a cache of an owner's settings, on a running module's loop (from
 *   its start, for example), logging as that module.  values is the
 *   owner's struct: it holds the defaults until Settings' values are read,
 *   and is written only on the loop.  An owner named as the module holds
 *   its log level too; a cache the library opened for the level alone
 *   (pnut_loop_config_s's levels) is closed then.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value otherwise.
 *
 ****************************************************************************/

int pnut_settings_cache_open(FAR struct pnut_module_s *module,
                             FAR const struct pnut_settings_desc_s *desc,
                             FAR void *values,
                             pnut_settings_changed_t changed,
                             FAR void *arg,
                             FAR struct pnut_settings_cache_s *cache);

/****************************************************************************
 * Name: pnut_settings_cache_close
 ****************************************************************************/

void pnut_settings_cache_close(FAR struct pnut_settings_cache_s *cache);

/****************************************************************************
 * Name: pnut_settings_cache_ready
 *
 * Description:
 *   Whether the struct holds Settings' values, not only the defaults.
 *
 ****************************************************************************/

bool
pnut_settings_cache_ready(FAR const struct pnut_settings_cache_s *cache);

/****************************************************************************
 * Name: pnut_settings_cache_set
 *
 * Description:
 *   Ask Settings to change the setting at index; the struct changes once
 *   Settings announces it.  The generator writes typed calls on this.
 *
 * Returned Value:
 *   Zero (OK), and done is called later, if not NULL; -EINVAL for an index
 *   out of range; -ENOTCONN while Settings cannot be reached; -EBUSY when
 *   as many calls as allowed are in flight.
 *
 ****************************************************************************/

int pnut_settings_cache_set(FAR struct pnut_settings_cache_s *cache,
                            int index,
                            FAR const pnut_setting_value_t *value,
                            pnut_settings_done_t done, FAR void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __PNUT_OS_LIB_PNUT_SETTINGS_CACHE_H */
