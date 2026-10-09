/****************************************************************************
 * pnut-os/src/system/settings_store.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_SETTINGS_STORE_H
#define __PNUT_OS_SYSTEM_SETTINGS_STORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/pool.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "pnut/settings.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The longest owner and key, without their NUL */

#define SETTINGS_NAME_MAX     64

/* The longest string, and the most choices (settings.proto) */

#define SETTINGS_TEXT_MAX     256
#define SETTINGS_CHOICES_MAX  16

/* The longest wait before an owner's file is tried again, after its
 * writes failed, in milliseconds
 */

#define SETTINGS_BACKOFF_MAX  60000

/* An owner's file holds at most so many bytes: a choice's value is its
 * name, and only a string's value takes a text (its default another)
 */

#define SETTINGS_ENTRY_SMALL  108
#define SETTINGS_ENTRY_TEXT   331
#define SETTINGS_FILE_MAX(settings, texts) \
  ((size_t)(settings) * SETTINGS_ENTRY_SMALL + \
   (size_t)(texts) / 2 * SETTINGS_ENTRY_TEXT)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The pools' sizes */

struct settings_limits_s
{
  uint8_t owners;                 /* Owners registered */
  uint16_t settings;              /* Settings, all owners' together */
  uint16_t texts;                 /* Strings: a value and a default each */
  uint16_t choices;               /* Choices' names */
};

struct settings_owner_s;

/* Every owner's schema and values, in fixed pools (RFC 0025).  The store
 * does no I/O: the module reads and writes the owners' files through
 * settings_store_encode() and settings_store_decode().
 */

struct settings_store_s
{
  FAR struct settings_owner_s *owners;
  uint8_t nowners;
  uint8_t cursor;                 /* The next owner to look at */
  struct pnut_pool_s entries;
  struct pnut_pool_s texts;
  struct pnut_pool_s choices;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: settings_store_init, settings_store_deinit
 *
 * Description:
 *   Take the pools from the heap, or give them back.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENOMEM.
 *
 ****************************************************************************/

int settings_store_init(FAR struct settings_store_s *store,
                        FAR const struct settings_limits_s *limits);
void settings_store_deinit(FAR struct settings_store_s *store);

/* The requests' work.  Each returns a PNUT_STATUS_ value, and on an error
 * sets *code to a pnut_settings_error_code_t, the error's detail.
 */

/****************************************************************************
 * Name: settings_store_register
 *
 * Description:
 *   A page of an owner's schema.  The first page marks the owner's
 *   settings undeclared; each declares some again, keeping a value the new
 *   declaration still allows, or adds new ones at their defaults; the last
 *   drops those still undeclared.  A page is taken whole or not at all.
 *
 * Input Parameters:
 *   loadp - Set on the last page when the owner's file is yet to be read.
 *   keyp  - Set on an error to the key of the setting at fault, in the
 *           request, or NULL.
 *
 ****************************************************************************/

int settings_store_register(FAR struct settings_store_s *store,
                            FAR const pnut_settings_register_request_t *in,
                            FAR bool *loadp, FAR const char **keyp,
                            FAR uint32_t *code);

/****************************************************************************
 * Name: settings_store_get
 ****************************************************************************/

int settings_store_get(FAR struct settings_store_s *store,
                       FAR const char *owner, FAR const char *key,
                       FAR pnut_setting_value_t *out, FAR uint32_t *code);

/****************************************************************************
 * Name: settings_store_set
 *
 * Input Parameters:
 *   changedp - Set when the value is not the one it was.
 *
 ****************************************************************************/

int settings_store_set(FAR struct settings_store_s *store,
                       FAR const char *owner, FAR const char *key,
                       FAR const pnut_setting_value_t *value,
                       FAR bool *changedp, FAR uint32_t *code);

/****************************************************************************
 * Name: settings_store_reset
 *
 * Description:
 *   One setting back to its default, or every one of the owner's when key
 *   is empty.
 *
 ****************************************************************************/

int settings_store_reset(FAR struct settings_store_s *store,
                         FAR const char *owner, FAR const char *key,
                         FAR bool *changedp, FAR uint32_t *code);

/****************************************************************************
 * Name: settings_store_list, settings_store_describe
 *
 * Description:
 *   A page of an owner's values, or of its schema, in the order of the
 *   keys, after the key given (none: from the first).
 *
 ****************************************************************************/

int settings_store_list(FAR struct settings_store_s *store,
                        FAR const char *owner, FAR const char *after,
                        FAR pnut_settings_values_t *out,
                        FAR uint32_t *code);
int settings_store_describe(FAR struct settings_store_s *store,
                            FAR const char *owner, FAR const char *after,
                            FAR pnut_settings_schema_t *out,
                            FAR uint32_t *code);

/* Writing the owners' files.  The time is the caller's, in milliseconds
 * from any start, so that the store needs no clock.
 */

/****************************************************************************
 * Name: settings_store_dirty
 *
 * Description:
 *   An owner whose values have changed since they were last encoded, and
 *   whose file may be tried now, or NULL; its name is the file's.  An
 *   owner whose file is yet to be read, or is held, is never one.  The
 *   owners take turns: encoding one moves the turn past it.
 *
 ****************************************************************************/

FAR const char *settings_store_dirty(FAR struct settings_store_s *store,
                                     uint64_t now);

/****************************************************************************
 * Name: settings_store_due
 *
 * Description:
 *   Milliseconds until a changed owner's file may be tried: zero if one
 *   may now, -1 if none has changed.
 *
 ****************************************************************************/

int64_t settings_store_due(FAR struct settings_store_s *store, uint64_t now);

/****************************************************************************
 * Name: settings_store_failed
 *
 * Description:
 *   An owner's file was not written: its values count as changed again,
 *   and it is tried again later and later, from delay up to
 *   SETTINGS_BACKOFF_MAX, so that a file system that refuses its writes
 *   is not tried each second, nor keeps the other owners waiting.
 *
 * Returned Value:
 *   How many writes of its file have failed in a row.
 *
 ****************************************************************************/

unsigned int settings_store_failed(FAR struct settings_store_s *store,
                                   FAR const char *owner, uint64_t now,
                                   uint32_t delay);

/****************************************************************************
 * Name: settings_store_written
 *
 * Description:
 *   An owner's file was written.
 *
 ****************************************************************************/

void settings_store_written(FAR struct settings_store_s *store,
                            FAR const char *owner);

/****************************************************************************
 * Name: settings_store_hold
 *
 * Description:
 *   Never write an owner's file while the program runs: it is there, but
 *   could not be read, and writing it would lose what it holds.  The
 *   owner's values still change in memory.
 *
 ****************************************************************************/

void settings_store_hold(FAR struct settings_store_s *store,
                         FAR const char *owner);

/****************************************************************************
 * Name: settings_store_touch
 *
 * Description:
 *   Mark an owner's values as changed, so that its file is written again.
 *
 ****************************************************************************/

void settings_store_touch(FAR struct settings_store_s *store,
                          FAR const char *owner);

/****************************************************************************
 * Name: settings_store_encode
 *
 * Description:
 *   Write an owner's values as its file (pnut.SettingsFile), count them as
 *   no longer changed, and move the turn past the owner.
 *
 * Returned Value:
 *   true on success; false if the stream failed.
 *
 ****************************************************************************/

bool settings_store_encode(FAR struct settings_store_s *store,
                           FAR const char *owner,
                           FAR pb_ostream_t *stream);

/****************************************************************************
 * Name: settings_store_decode
 *
 * Description:
 *   Read an owner's values from its file, once its schema is registered.
 *   A value its schema no longer allows is left at the default, and the
 *   owner is marked as changed, so that its file is written again.
 *
 * Returned Value:
 *   true on success; false if the file does not decode, and then the
 *   values read so far are kept.
 *
 ****************************************************************************/

bool settings_store_decode(FAR struct settings_store_s *store,
                           FAR const char *owner, FAR pb_istream_t *stream);

#endif /* __PNUT_OS_SYSTEM_SETTINGS_STORE_H */
