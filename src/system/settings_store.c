/****************************************************************************
 * pnut-os/src/system/settings_store.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/msg.h>

#include "settings_store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The longest secret, in bytes (RFC 0025) */

#define SETTINGS_SECRET_MAX   1024

/* A page's settings at most (settings.proto) */

#define SETTINGS_PAGE_MAX \
  (sizeof(((FAR pnut_settings_register_request_t *)0)->settings) / \
   sizeof(pnut_setting_t))

#define SETTINGS_LIST_MAX \
  (sizeof(((FAR pnut_settings_values_t *)0)->entries) / \
   sizeof(pnut_settings_entry_t))

#define SETTINGS_DESCRIBE_MAX \
  (sizeof(((FAR pnut_settings_schema_t *)0)->settings) / \
   sizeof(pnut_setting_t))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct settings_choice_s
{
  FAR struct settings_choice_s *next;
  char name[sizeof(((FAR pnut_setting_t *)0)->choices[0])];
};

struct settings_text_s
{
  char text[SETTINGS_TEXT_MAX + 1];
};

/* A value, as its entry's type says */

union settings_val_u
{
  bool boolean;
  int64_t integer;
  uint8_t choice;                 /* Its place among the choices */
  FAR struct settings_text_s *text;
};

/* A setting: its schema and its value */

struct settings_entry_s
{
  FAR struct settings_entry_s *next;     /* The owner's next, by key */
  char key[SETTINGS_NAME_MAX + 1];
  char label[sizeof(((FAR pnut_setting_t *)0)->label)];
  char group[sizeof(((FAR pnut_setting_t *)0)->group)];
  char unit[sizeof(((FAR pnut_setting_t *)0)->unit)];
  uint8_t type;                          /* pnut_setting_type_t */
  uint8_t nchoices;
  bool is_public;
  bool declared;                         /* In the schema registering */
  uint32_t max_length;
  int64_t min;
  int64_t max;
  FAR struct settings_choice_s *choices;
  union settings_val_u value;
  union settings_val_u def;
};

struct settings_owner_s
{
  char name[SETTINGS_NAME_MAX + 1];      /* Empty: a free place */
  FAR struct settings_entry_s *entries;  /* By key */
  bool registering;                      /* Between its first and last */
  bool loaded;                           /* Its file has been read */
  bool dirty;                            /* Changed since encoded */
  bool held;                             /* Its file is not written */
  uint8_t failures;                      /* Writes failed in a row */
  uint64_t retry;                        /* Not tried before this time */
};

/* What the file decoder needs */

struct settings_decode_s
{
  FAR struct settings_store_s *store;
  FAR struct settings_owner_s *owner;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: settings_name_ok
 *
 * Description:
 *   A key: lowercase letters, digits, dots and underscores.  An owner, a
 *   service's name or an app's id, may have hyphens too; it names a file,
 *   so it never starts with a dot.
 *
 ****************************************************************************/

static bool settings_name_ok(FAR const char *name, bool owner)
{
  size_t i;

  if (name[0] == '\0' || name[0] == '.')
    {
      return false;
    }

  for (i = 0; name[i] != '\0'; i++)
    {
      if (i >= SETTINGS_NAME_MAX)
        {
          return false;
        }

      if ((name[i] < 'a' || name[i] > 'z') &&
          (name[i] < '0' || name[i] > '9') &&
          name[i] != '.' && name[i] != '_' &&
          (!owner || name[i] != '-'))
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: settings_owner_find
 ****************************************************************************/

static FAR struct settings_owner_s *
settings_owner_find(FAR struct settings_store_s *store, FAR const char *name)
{
  uint8_t i;

  for (i = 0; i < store->nowners; i++)
    {
      if (store->owners[i].name[0] != '\0' &&
          strcmp(store->owners[i].name, name) == 0)
        {
          return &store->owners[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: settings_entry_find
 ****************************************************************************/

static FAR struct settings_entry_s *
settings_entry_find(FAR struct settings_owner_s *owner, FAR const char *key)
{
  FAR struct settings_entry_s *entry = NULL;
  int cmp;

  for (entry = owner->entries; entry != NULL; entry = entry->next)
    {
      cmp = strcmp(entry->key, key);
      if (cmp == 0)
        {
          return entry;
        }

      if (cmp > 0)
        {
          break;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: settings_entry_insert
 *
 * Description:
 *   Put an entry in its owner's list, in the order of the keys.
 *
 ****************************************************************************/

static void settings_entry_insert(FAR struct settings_owner_s *owner,
                                  FAR struct settings_entry_s *entry)
{
  FAR struct settings_entry_s **link = &owner->entries;

  while (*link != NULL && strcmp((*link)->key, entry->key) < 0)
    {
      link = &(*link)->next;
    }

  entry->next = *link;
  *link       = entry;
}

/****************************************************************************
 * Name: settings_entry_release
 *
 * Description:
 *   Give back what an entry's type took from the pools.
 *
 ****************************************************************************/

static void settings_entry_release(FAR struct settings_store_s *store,
                                   FAR struct settings_entry_s *entry)
{
  FAR struct settings_choice_s *choice;

  if (entry->type == PNUT_SETTING_TYPE_STRING)
    {
      pnut_pool_free(&store->texts, entry->value.text);
      pnut_pool_free(&store->texts, entry->def.text);
      entry->value.text = NULL;
      entry->def.text   = NULL;
    }

  while (entry->choices != NULL)
    {
      choice         = entry->choices;
      entry->choices = choice->next;
      pnut_pool_free(&store->choices, choice);
    }

  entry->nchoices = 0;
}

/****************************************************************************
 * Name: settings_choice_find
 *
 * Description:
 *   A name's place among an entry's choices, or -1.
 *
 ****************************************************************************/

static int settings_choice_find(FAR const struct settings_entry_s *entry,
                                FAR const char *name)
{
  FAR const struct settings_choice_s *choice;
  int i = 0;

  for (choice = entry->choices; choice != NULL; choice = choice->next, i++)
    {
      if (strcmp(choice->name, name) == 0)
        {
          return i;
        }
    }

  return -1;
}

/****************************************************************************
 * Name: settings_choice_name
 ****************************************************************************/

static FAR const char *
settings_choice_name(FAR const struct settings_entry_s *entry, int index)
{
  FAR const struct settings_choice_s *choice = entry->choices;

  while (choice != NULL && index-- > 0)
    {
      choice = choice->next;
    }

  return choice != NULL ? choice->name : "";
}

/****************************************************************************
 * Name: settings_value_get
 *
 * Description:
 *   An entry's value, or its default, as it travels.
 *
 ****************************************************************************/

static void settings_value_get(FAR const struct settings_entry_s *entry,
                               FAR const union settings_val_u *val,
                               FAR pnut_setting_value_t *out)
{
  memset(out, 0, sizeof(*out));

  switch (entry->type)
    {
      case PNUT_SETTING_TYPE_BOOLEAN:
        out->which_value   = PNUT_SETTING_VALUE_BOOLEAN_TAG;
        out->value.boolean = val->boolean;
        break;

      case PNUT_SETTING_TYPE_INTEGER:
        out->which_value   = PNUT_SETTING_VALUE_INTEGER_TAG;
        out->value.integer = val->integer;
        break;

      case PNUT_SETTING_TYPE_STRING:
        out->which_value = PNUT_SETTING_VALUE_TEXT_TAG;
        strlcpy(out->value.text, val->text->text, sizeof(out->value.text));
        break;

      case PNUT_SETTING_TYPE_CHOICE:
        out->which_value = PNUT_SETTING_VALUE_TEXT_TAG;
        strlcpy(out->value.text, settings_choice_name(entry, val->choice),
                sizeof(out->value.text));
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: settings_value_check
 *
 * Description:
 *   Whether a value fits an entry's schema.
 *
 ****************************************************************************/

static int settings_value_check(FAR const struct settings_entry_s *entry,
                                FAR const pnut_setting_value_t *value,
                                FAR uint32_t *code)
{
  pb_size_t which;

  switch (entry->type)
    {
      case PNUT_SETTING_TYPE_BOOLEAN:
        which = PNUT_SETTING_VALUE_BOOLEAN_TAG;
        break;

      case PNUT_SETTING_TYPE_INTEGER:
        which = PNUT_SETTING_VALUE_INTEGER_TAG;
        break;

      case PNUT_SETTING_TYPE_STRING:
      case PNUT_SETTING_TYPE_CHOICE:
        which = PNUT_SETTING_VALUE_TEXT_TAG;
        break;

      default:
        *code = PNUT_SETTINGS_ERROR_CODE_SECRET;
        return PNUT_STATUS_UNAVAILABLE;
    }

  if (value->which_value != which)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_WRONG_TYPE;
      return PNUT_STATUS_INVALID;
    }

  if (entry->type == PNUT_SETTING_TYPE_INTEGER &&
      (value->value.integer < entry->min ||
       value->value.integer > entry->max))
    {
      *code = PNUT_SETTINGS_ERROR_CODE_OUT_OF_RANGE;
      return PNUT_STATUS_INVALID;
    }

  if (entry->type == PNUT_SETTING_TYPE_STRING &&
      strlen(value->value.text) > entry->max_length)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_TOO_LONG;
      return PNUT_STATUS_INVALID;
    }

  if (entry->type == PNUT_SETTING_TYPE_CHOICE &&
      settings_choice_find(entry, value->value.text) < 0)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_NOT_A_CHOICE;
      return PNUT_STATUS_INVALID;
    }

  return PNUT_STATUS_OK;
}

/****************************************************************************
 * Name: settings_value_put
 *
 * Description:
 *   Set an entry's value, or its default, from a value that fits.
 *
 * Returned Value:
 *   Whether it changed.
 *
 ****************************************************************************/

static bool settings_value_put(FAR struct settings_entry_s *entry,
                               FAR union settings_val_u *val,
                               FAR const pnut_setting_value_t *value)
{
  bool changed;
  int index;

  switch (entry->type)
    {
      case PNUT_SETTING_TYPE_BOOLEAN:
        changed      = val->boolean != value->value.boolean;
        val->boolean = value->value.boolean;
        return changed;

      case PNUT_SETTING_TYPE_INTEGER:
        changed      = val->integer != value->value.integer;
        val->integer = value->value.integer;
        return changed;

      case PNUT_SETTING_TYPE_STRING:
        changed = strcmp(val->text->text, value->value.text) != 0;
        strlcpy(val->text->text, value->value.text,
                sizeof(val->text->text));
        return changed;

      case PNUT_SETTING_TYPE_CHOICE:
        index       = settings_choice_find(entry, value->value.text);
        changed     = val->choice != index;
        val->choice = (uint8_t)index;
        return changed;

      default:
        return false;
    }
}

/****************************************************************************
 * Name: settings_reset_one
 *
 * Description:
 *   An entry's value back to its default.
 *
 ****************************************************************************/

static bool settings_reset_one(FAR struct settings_entry_s *entry)
{
  pnut_setting_value_t def;

  if (entry->type == PNUT_SETTING_TYPE_SECRET)
    {
      return false;
    }

  settings_value_get(entry, &entry->def, &def);
  return settings_value_put(entry, &entry->value, &def);
}

/****************************************************************************
 * Name: settings_schema_check
 *
 * Description:
 *   Whether a setting's declaration holds together.  A default left out
 *   is the type's zero (false, 0, empty, the first choice), and must fit
 *   too.
 *
 ****************************************************************************/

static int settings_schema_check(FAR const pnut_setting_t *setting,
                                 FAR uint32_t *code)
{
  FAR const pnut_setting_value_t *def = &setting->default_value;
  bool has = setting->has_default_value;
  pb_size_t i;
  pb_size_t j;

  if (!settings_name_ok(setting->key, false))
    {
      *code = PNUT_SETTINGS_ERROR_CODE_BAD_NAME;
      return PNUT_STATUS_INVALID;
    }

  *code = PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA;

  switch (setting->type)
    {
      case PNUT_SETTING_TYPE_BOOLEAN:
        if (has && def->which_value != PNUT_SETTING_VALUE_BOOLEAN_TAG)
          {
            return PNUT_STATUS_INVALID;
          }

        break;

      case PNUT_SETTING_TYPE_INTEGER:
        if (setting->min > setting->max ||
            (has && def->which_value != PNUT_SETTING_VALUE_INTEGER_TAG))
          {
            return PNUT_STATUS_INVALID;
          }

        if ((has ? def->value.integer : 0) < setting->min ||
            (has ? def->value.integer : 0) > setting->max)
          {
            return PNUT_STATUS_INVALID;
          }

        break;

      case PNUT_SETTING_TYPE_STRING:
        if (setting->max_length == 0 ||
            setting->max_length > SETTINGS_TEXT_MAX ||
            (has && (def->which_value != PNUT_SETTING_VALUE_TEXT_TAG ||
                     strlen(def->value.text) > setting->max_length)))
          {
            return PNUT_STATUS_INVALID;
          }

        break;

      case PNUT_SETTING_TYPE_CHOICE:
        if (setting->choices_count == 0 ||
            (has && def->which_value != PNUT_SETTING_VALUE_TEXT_TAG))
          {
            return PNUT_STATUS_INVALID;
          }

        for (i = 0; i < setting->choices_count; i++)
          {
            if (setting->choices[i][0] == '\0')
              {
                return PNUT_STATUS_INVALID;
              }

            for (j = 0; j < i; j++)
              {
                if (strcmp(setting->choices[i], setting->choices[j]) == 0)
                  {
                    return PNUT_STATUS_INVALID;
                  }
              }
          }

        if (has)
          {
            for (i = 0; i < setting->choices_count; i++)
              {
                if (strcmp(setting->choices[i], def->value.text) == 0)
                  {
                    break;
                  }
              }

            if (i == setting->choices_count)
              {
                return PNUT_STATUS_INVALID;
              }
          }

        break;

      case PNUT_SETTING_TYPE_SECRET:
        if (setting->max_length == 0 ||
            setting->max_length > SETTINGS_SECRET_MAX || has)
          {
            return PNUT_STATUS_INVALID;
          }

        break;

      default:
        return PNUT_STATUS_INVALID;
    }

  *code = PNUT_SETTINGS_ERROR_CODE_NONE;
  return PNUT_STATUS_OK;
}

/****************************************************************************
 * Name: settings_schema_put
 *
 * Description:
 *   Fill an entry from its declaration, taking its texts and choices from
 *   the pools, which the caller has checked have room; its value is its
 *   default.
 *
 ****************************************************************************/

static void settings_schema_put(FAR struct settings_store_s *store,
                                FAR struct settings_entry_s *entry,
                                FAR const pnut_setting_t *setting)
{
  FAR struct settings_choice_s **link;
  FAR struct settings_choice_s *choice;
  pb_size_t i;

  strlcpy(entry->key, setting->key, sizeof(entry->key));
  strlcpy(entry->label, setting->label, sizeof(entry->label));
  strlcpy(entry->group, setting->group, sizeof(entry->group));
  strlcpy(entry->unit, setting->unit, sizeof(entry->unit));
  entry->type       = setting->type;
  entry->is_public  = setting->is_public;
  entry->max_length = setting->max_length;
  entry->min        = setting->min;
  entry->max        = setting->max;
  entry->declared   = true;

  memset(&entry->value, 0, sizeof(entry->value));
  memset(&entry->def, 0, sizeof(entry->def));

  if (entry->type == PNUT_SETTING_TYPE_STRING)
    {
      entry->value.text = pnut_pool_alloc(&store->texts);
      entry->def.text   = pnut_pool_alloc(&store->texts);
    }

  if (entry->type == PNUT_SETTING_TYPE_CHOICE)
    {
      link = &entry->choices;
      for (i = 0; i < setting->choices_count; i++)
        {
          choice = pnut_pool_alloc(&store->choices);
          strlcpy(choice->name, setting->choices[i], sizeof(choice->name));
          *link = choice;
          link  = &choice->next;
        }

      entry->nchoices = setting->choices_count;
    }

  /* Without a default, the zeroed one stands: false, 0, an empty string
   * (the pools give zeroed objects), the first choice
   */

  if (setting->has_default_value)
    {
      settings_value_put(entry, &entry->def, &setting->default_value);
    }

  settings_reset_one(entry);
}

/****************************************************************************
 * Name: settings_schema_get
 *
 * Description:
 *   An entry's declaration, as it travels.
 *
 ****************************************************************************/

static void settings_schema_get(FAR const struct settings_entry_s *entry,
                                FAR pnut_setting_t *out)
{
  FAR const struct settings_choice_s *choice;

  memset(out, 0, sizeof(*out));
  strlcpy(out->key, entry->key, sizeof(out->key));
  strlcpy(out->label, entry->label, sizeof(out->label));
  strlcpy(out->group, entry->group, sizeof(out->group));
  strlcpy(out->unit, entry->unit, sizeof(out->unit));
  out->type       = entry->type;
  out->is_public  = entry->is_public;
  out->max_length = entry->max_length;
  out->min        = entry->min;
  out->max        = entry->max;

  for (choice = entry->choices; choice != NULL; choice = choice->next)
    {
      strlcpy(out->choices[out->choices_count++], choice->name,
              sizeof(out->choices[0]));
    }

  if (entry->type != PNUT_SETTING_TYPE_SECRET)
    {
      out->has_default_value = true;
      settings_value_get(entry, &entry->def, &out->default_value);
    }
}

/****************************************************************************
 * Name: settings_lookup
 *
 * Description:
 *   A request's owner, and its setting when key is not NULL.
 *
 ****************************************************************************/

static int settings_lookup(FAR struct settings_store_s *store,
                           FAR const char *owner, FAR const char *key,
                           FAR struct settings_owner_s **ownerp,
                           FAR struct settings_entry_s **entryp,
                           FAR uint32_t *code)
{
  /* Not before its first schema is whole, and its file read: a change
   * then would be written over the values in its file
   */

  *ownerp = settings_owner_find(store, owner);
  if (*ownerp == NULL)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_UNKNOWN_OWNER;
      return PNUT_STATUS_NOTFOUND;
    }

  if (!(*ownerp)->loaded)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_REGISTERING;
      return PNUT_STATUS_UNAVAILABLE;
    }

  if (key != NULL)
    {
      *entryp = settings_entry_find(*ownerp, key);
      if (*entryp == NULL)
        {
          *code = PNUT_SETTINGS_ERROR_CODE_UNKNOWN_KEY;
          return PNUT_STATUS_NOTFOUND;
        }
    }

  return PNUT_STATUS_OK;
}

/****************************************************************************
 * Name: settings_encode_entries
 *
 * Description:
 *   pnut.SettingsFile's entries, from an owner's values.  A secret has
 *   none yet.
 *
 ****************************************************************************/

static bool settings_encode_entries(FAR pb_ostream_t *stream,
                                    FAR const pb_field_t *field,
                                    FAR void * const *arg)
{
  FAR struct settings_owner_s *owner = *arg;
  FAR struct settings_entry_s *entry = NULL;
  pnut_settings_entry_t out;

  for (entry = owner->entries; entry != NULL; entry = entry->next)
    {
      if (entry->type == PNUT_SETTING_TYPE_SECRET)
        {
          continue;
        }

      memset(&out, 0, sizeof(out));
      strlcpy(out.key, entry->key, sizeof(out.key));
      out.has_value = true;
      settings_value_get(entry, &entry->value, &out.value);

      if (!pb_encode_tag_for_field(stream, field) ||
          !pb_encode_submessage(stream, PNUT_SETTINGS_ENTRY_FIELDS, &out))
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: settings_decode_entry
 *
 * Description:
 *   One of pnut.SettingsFile's entries, into the owner's values.
 *
 ****************************************************************************/

static bool settings_decode_entry(FAR pb_istream_t *stream,
                                  FAR const pb_field_t *field,
                                  FAR void **arg)
{
  FAR struct settings_decode_s *ctx = *arg;
  FAR struct settings_entry_s *entry = NULL;
  pnut_settings_entry_t in;
  uint32_t code;

  memset(&in, 0, sizeof(in));
  if (!pb_decode(stream, PNUT_SETTINGS_ENTRY_FIELDS, &in))
    {
      return false;
    }

  entry = settings_entry_find(ctx->owner, in.key);
  if (entry == NULL || !in.has_value ||
      settings_value_check(entry, &in.value, &code) != PNUT_STATUS_OK)
    {
      /* No longer in the schema, or no longer fits it: the file is
       * written again without it
       */

      ctx->owner->dirty = true;
      return true;
    }

  settings_value_put(entry, &entry->value, &in.value);
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int settings_store_init(FAR struct settings_store_s *store,
                        FAR const struct settings_limits_s *limits)
{
  int ret;

  memset(store, 0, sizeof(*store));

  store->owners = calloc(limits->owners, sizeof(struct settings_owner_s));
  if (store->owners == NULL)
    {
      return -ENOMEM;
    }

  store->nowners = limits->owners;

  ret = pnut_pool_init(&store->entries, sizeof(struct settings_entry_s),
                       limits->settings);
  if (ret >= 0)
    {
      ret = pnut_pool_init(&store->texts, sizeof(struct settings_text_s),
                           limits->texts);
    }

  if (ret >= 0)
    {
      ret = pnut_pool_init(&store->choices,
                           sizeof(struct settings_choice_s),
                           limits->choices);
    }

  if (ret < 0)
    {
      settings_store_deinit(store);
    }

  return ret;
}

void settings_store_deinit(FAR struct settings_store_s *store)
{
  pnut_pool_deinit(&store->choices);
  pnut_pool_deinit(&store->texts);
  pnut_pool_deinit(&store->entries);
  free(store->owners);
  store->owners  = NULL;
  store->nowners = 0;
}

int settings_store_register(FAR struct settings_store_s *store,
                            FAR const pnut_settings_register_request_t *in,
                            FAR bool *loadp, FAR const char **keyp,
                            FAR uint32_t *code)
{
  FAR struct settings_entry_s *found[SETTINGS_PAGE_MAX];
  FAR struct settings_entry_s **link;
  FAR struct settings_entry_s *entry = NULL;
  FAR struct settings_owner_s *owner;
  pnut_setting_value_t olds[SETTINGS_PAGE_MAX];
  bool kept[SETTINGS_PAGE_MAX];
  int nentries = 0;
  int ntexts = 0;
  int nchoices = 0;
  pb_size_t i;
  pb_size_t j;
  uint8_t k;
  int ret;

  *loadp = false;
  *keyp  = NULL;
  *code  = PNUT_SETTINGS_ERROR_CODE_NONE;

  if (!settings_name_ok(in->owner, true))
    {
      *code = PNUT_SETTINGS_ERROR_CODE_BAD_NAME;
      return PNUT_STATUS_INVALID;
    }

  /* A schema whose first page came and whose last never did holds its
   * owner's place until that owner registers again: owners are not
   * checked yet, so nobody else names it (RFC 0006 comes with that)
   */

  owner = settings_owner_find(store, in->owner);
  if (!in->first && (owner == NULL || !owner->registering))
    {
      *code = PNUT_SETTINGS_ERROR_CODE_NOT_REGISTERING;
      return PNUT_STATUS_INVALID;
    }

  /* The page whole, before anything changes: each declaration, and room
   * for what it takes beyond what the declaration it replaces gives back
   */

  for (i = 0; i < in->settings_count; i++)
    {
      FAR const pnut_setting_t *setting = &in->settings[i];

      *keyp = setting->key;
      ret   = settings_schema_check(setting, code);
      if (ret != PNUT_STATUS_OK)
        {
          return ret;
        }

      for (j = 0; j < i; j++)
        {
          if (strcmp(in->settings[j].key, setting->key) == 0)
            {
              *code = PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA;
              return PNUT_STATUS_INVALID;
            }
        }

      found[i] = owner != NULL ? settings_entry_find(owner, setting->key) :
                                 NULL;
      if (found[i] != NULL && found[i]->declared && !in->first)
        {
          *code = PNUT_SETTINGS_ERROR_CODE_BAD_SCHEMA;
          return PNUT_STATUS_INVALID;
        }

      if (found[i] == NULL)
        {
          nentries++;
        }
      else
        {
          ntexts   -= found[i]->type == PNUT_SETTING_TYPE_STRING ? 2 : 0;
          nchoices -= found[i]->nchoices;
        }

      ntexts   += setting->type == PNUT_SETTING_TYPE_STRING ? 2 : 0;
      nchoices += setting->type == PNUT_SETTING_TYPE_CHOICE ?
                  setting->choices_count : 0;
    }

  *keyp = NULL;

  if (owner == NULL)
    {
      for (k = 0; k < store->nowners && owner == NULL; k++)
        {
          if (store->owners[k].name[0] == '\0')
            {
              owner = &store->owners[k];
            }
        }
    }

  /* What the last page will drop is not counted: an owner at the pools'
   * limit renames its settings by registering an empty schema first
   */

  if (owner == NULL || nentries > store->entries.avail ||
      ntexts > (int)store->texts.avail ||
      nchoices > (int)store->choices.avail)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_FULL;
      return PNUT_STATUS_BUSY;
    }

  /* The first page starts the schema afresh */

  if (owner->name[0] == '\0')
    {
      strlcpy(owner->name, in->owner, sizeof(owner->name));
    }

  if (in->first)
    {
      for (entry = owner->entries; entry != NULL; entry = entry->next)
        {
          entry->declared = false;
        }

      owner->registering = true;
    }

  /* What the declarations replaced give back first, their values kept
   * aside, so that the new ones find the room counted above
   */

  for (i = 0; i < in->settings_count; i++)
    {
      kept[i] = false;
      if (found[i] != NULL &&
          found[i]->type == in->settings[i].type &&
          found[i]->type != PNUT_SETTING_TYPE_SECRET)
        {
          settings_value_get(found[i], &found[i]->value, &olds[i]);
          kept[i] = true;
        }

      if (found[i] != NULL)
        {
          settings_entry_release(store, found[i]);
        }
    }

  for (i = 0; i < in->settings_count; i++)
    {
      uint32_t unused;

      entry = found[i];
      if (entry == NULL)
        {
          entry = pnut_pool_alloc(&store->entries);
          strlcpy(entry->key, in->settings[i].key, sizeof(entry->key));
          settings_entry_insert(owner, entry);
        }

      settings_schema_put(store, entry, &in->settings[i]);

      /* A value the new declaration still allows is kept */

      if (kept[i] &&
          settings_value_check(entry, &olds[i], &unused) == PNUT_STATUS_OK)
        {
          settings_value_put(entry, &entry->value, &olds[i]);
        }
      else if (found[i] != NULL && owner->loaded)
        {
          owner->dirty = true;
        }
    }

  /* The last page drops what was not declared again */

  if (in->last)
    {
      link = &owner->entries;
      while (*link != NULL)
        {
          entry = *link;
          if (entry->declared)
            {
              link = &entry->next;
              continue;
            }

          *link = entry->next;
          settings_entry_release(store, entry);
          pnut_pool_free(&store->entries, entry);
          if (owner->loaded)
            {
              owner->dirty = true;
            }
        }

      owner->registering = false;
      *loadp             = !owner->loaded;
      owner->loaded      = true;
    }

  return PNUT_STATUS_OK;
}

int settings_store_get(FAR struct settings_store_s *store,
                       FAR const char *owner, FAR const char *key, bool all,
                       FAR pnut_setting_value_t *out, FAR uint32_t *code)
{
  FAR struct settings_owner_s *o;
  FAR struct settings_entry_s *entry = NULL;
  int ret;

  ret = settings_lookup(store, owner, key, &o, &entry, code);
  if (ret != PNUT_STATUS_OK)
    {
      return ret;
    }

  if (!all && !entry->is_public)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_DENIED;
      return PNUT_STATUS_DENIED;
    }

  if (entry->type == PNUT_SETTING_TYPE_SECRET)
    {
      *code = PNUT_SETTINGS_ERROR_CODE_SECRET;
      return PNUT_STATUS_UNAVAILABLE;
    }

  settings_value_get(entry, &entry->value, out);
  return PNUT_STATUS_OK;
}

int settings_store_set(FAR struct settings_store_s *store,
                       FAR const char *owner, FAR const char *key,
                       FAR const pnut_setting_value_t *value,
                       FAR bool *changedp, FAR uint32_t *code)
{
  FAR struct settings_owner_s *o;
  FAR struct settings_entry_s *entry = NULL;
  int ret;

  *changedp = false;

  ret = settings_lookup(store, owner, key, &o, &entry, code);
  if (ret == PNUT_STATUS_OK)
    {
      ret = settings_value_check(entry, value, code);
    }

  if (ret != PNUT_STATUS_OK)
    {
      return ret;
    }

  *changedp = settings_value_put(entry, &entry->value, value);
  o->dirty |= *changedp;
  return PNUT_STATUS_OK;
}

int settings_store_reset(FAR struct settings_store_s *store,
                         FAR const char *owner, FAR const char *key,
                         FAR bool *changedp, FAR uint32_t *code)
{
  FAR struct settings_owner_s *o;
  FAR struct settings_entry_s *entry = NULL;
  int ret;

  *changedp = false;

  ret = settings_lookup(store, owner, key[0] != '\0' ? key : NULL, &o,
                        &entry, code);
  if (ret != PNUT_STATUS_OK)
    {
      return ret;
    }

  if (key[0] != '\0')
    {
      *changedp = settings_reset_one(entry);
    }
  else
    {
      for (entry = o->entries; entry != NULL; entry = entry->next)
        {
          *changedp |= settings_reset_one(entry);
        }
    }

  o->dirty |= *changedp;
  return PNUT_STATUS_OK;
}

int settings_store_list(FAR struct settings_store_s *store,
                        FAR const char *owner, FAR const char *after,
                        bool all, FAR pnut_settings_values_t *out,
                        FAR uint32_t *code)
{
  FAR struct settings_owner_s *o;
  FAR struct settings_entry_s *entry = NULL;
  FAR pnut_settings_entry_t *item;
  int ret;

  memset(out, 0, sizeof(*out));

  ret = settings_lookup(store, owner, NULL, &o, NULL, code);
  if (ret != PNUT_STATUS_OK)
    {
      return ret;
    }

  for (entry = o->entries; entry != NULL; entry = entry->next)
    {
      if ((after[0] != '\0' && strcmp(entry->key, after) <= 0) ||
          (!all && !entry->is_public))
        {
          continue;
        }

      if (out->entries_count == SETTINGS_LIST_MAX)
        {
          out->more = true;
          break;
        }

      item = &out->entries[out->entries_count++];
      strlcpy(item->key, entry->key, sizeof(item->key));
      if (entry->type != PNUT_SETTING_TYPE_SECRET)
        {
          item->has_value = true;
          settings_value_get(entry, &entry->value, &item->value);
        }
    }

  return PNUT_STATUS_OK;
}

int settings_store_describe(FAR struct settings_store_s *store,
                            FAR const char *owner, FAR const char *after,
                            bool all, FAR pnut_settings_schema_t *out,
                            FAR uint32_t *code)
{
  FAR struct settings_owner_s *o;
  FAR struct settings_entry_s *entry = NULL;
  int ret;

  memset(out, 0, sizeof(*out));

  ret = settings_lookup(store, owner, NULL, &o, NULL, code);
  if (ret != PNUT_STATUS_OK)
    {
      return ret;
    }

  for (entry = o->entries; entry != NULL; entry = entry->next)
    {
      if ((after[0] != '\0' && strcmp(entry->key, after) <= 0) ||
          (!all && !entry->is_public))
        {
          continue;
        }

      if (out->settings_count == SETTINGS_DESCRIBE_MAX)
        {
          out->more = true;
          break;
        }

      settings_schema_get(entry, &out->settings[out->settings_count++]);
    }

  return PNUT_STATUS_OK;
}

FAR const char *settings_store_dirty(FAR struct settings_store_s *store,
                                     uint64_t now)
{
  FAR struct settings_owner_s *owner;
  uint8_t i;

  for (i = 0; i < store->nowners; i++)
    {
      owner = &store->owners[(store->cursor + i) % store->nowners];
      if (owner->name[0] != '\0' && owner->dirty && owner->loaded &&
          !owner->held && owner->retry <= now)
        {
          return owner->name;
        }
    }

  return NULL;
}

int64_t settings_store_due(FAR struct settings_store_s *store, uint64_t now)
{
  FAR struct settings_owner_s *owner;
  int64_t due = -1;
  int64_t wait;
  uint8_t i;

  for (i = 0; i < store->nowners; i++)
    {
      owner = &store->owners[i];
      if (owner->name[0] == '\0' || !owner->dirty || !owner->loaded ||
          owner->held)
        {
          continue;
        }

      wait = owner->retry > now ? (int64_t)(owner->retry - now) : 0;
      if (due < 0 || wait < due)
        {
          due = wait;
        }
    }

  return due;
}

unsigned int settings_store_failed(FAR struct settings_store_s *store,
                                   FAR const char *owner, uint64_t now,
                                   uint32_t delay)
{
  FAR struct settings_owner_s *o = settings_owner_find(store, owner);
  uint64_t wait = delay;
  uint8_t i;

  if (o == NULL)
    {
      return 0;
    }

  if (o->failures < UINT8_MAX)
    {
      o->failures++;
    }

  for (i = 1; i < o->failures && wait < SETTINGS_BACKOFF_MAX; i++)
    {
      wait *= 2;
    }

  o->dirty = true;
  o->retry = now + (wait < SETTINGS_BACKOFF_MAX ? wait :
                                                  SETTINGS_BACKOFF_MAX);
  return o->failures;
}

void settings_store_written(FAR struct settings_store_s *store,
                            FAR const char *owner)
{
  FAR struct settings_owner_s *o = settings_owner_find(store, owner);

  if (o != NULL)
    {
      o->failures = 0;
      o->retry    = 0;
    }
}

void settings_store_hold(FAR struct settings_store_s *store,
                         FAR const char *owner)
{
  FAR struct settings_owner_s *o = settings_owner_find(store, owner);

  if (o != NULL)
    {
      o->held = true;
    }
}

void settings_store_touch(FAR struct settings_store_s *store,
                          FAR const char *owner)
{
  FAR struct settings_owner_s *o = settings_owner_find(store, owner);

  if (o != NULL)
    {
      o->dirty = true;
    }
}

bool settings_store_encode(FAR struct settings_store_s *store,
                           FAR const char *owner,
                           FAR pb_ostream_t *stream)
{
  FAR struct settings_owner_s *o = settings_owner_find(store, owner);
  pnut_settings_file_t file;

  if (o == NULL)
    {
      return false;
    }

  store->cursor = (o - store->owners + 1) % store->nowners;

  memset(&file, 0, sizeof(file));
  file.entries.funcs.encode = settings_encode_entries;
  file.entries.arg          = o;

  if (!pb_encode(stream, PNUT_SETTINGS_FILE_FIELDS, &file))
    {
      return false;
    }

  o->dirty = false;
  return true;
}

bool settings_store_decode(FAR struct settings_store_s *store,
                           FAR const char *owner, FAR pb_istream_t *stream)
{
  struct settings_decode_s ctx;
  pnut_settings_file_t file;

  ctx.store = store;
  ctx.owner = settings_owner_find(store, owner);
  if (ctx.owner == NULL)
    {
      return false;
    }

  memset(&file, 0, sizeof(file));
  file.entries.funcs.decode = settings_decode_entry;
  file.entries.arg          = &ctx;

  return pb_decode(stream, PNUT_SETTINGS_FILE_FIELDS, &file);
}
