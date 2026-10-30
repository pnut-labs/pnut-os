/****************************************************************************
 * pnut-os/src/lib/settings_cache.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/log.h>
#include <pnut/msg.h>
#include <pnut/settings_cache.h>

#include "pnut_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The waits before trying again, growing from the first to the longest,
 * in milliseconds
 */

#define CACHE_BACKOFF_MIN  100
#define CACHE_BACKOFF_MAX  5000

/* Declarations to a RegisterSchema page: its settings' max_count */

#define CACHE_PAGE \
  ((int)(sizeof(((FAR pnut_settings_register_request_t *)0)->settings) / \
         sizeof(((FAR pnut_settings_register_request_t *)0)->settings[0])))

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum cache_state_e
{
  CACHE_IDLE = 0,                 /* Not connected, or given up */
  CACHE_REGISTERING,              /* Sending the schema */
  CACHE_LISTING,                  /* Reading the values */
  CACHE_WATCHING,                 /* Up to date; reading again on changes */
};

/* A program's connection to Settings and its reader of the settings
 * topic, which its caches share (RFC 0023): made with the first cache and
 * kept until the loop is destroyed, open while a cache is
 */

struct pnut_settings_link_s
{
  FAR struct pnut_loop_s *loop;
  FAR struct pnut_settings_cache_s *caches;

  /* The next cache of a walk over them: a call may lose the connection
   * at once, and its answers' handlers, the program's, may close caches
   * meanwhile; link_detach() moves it on.  A walk nested in another ends
   * both.
   */

  FAR struct pnut_settings_cache_s *next;
  struct pnut_settings_client_s client;
  struct pnut_settings_change_reader_s reader;
  bool subscribed;
  bool versioned;
  uint32_t version;               /* The last change's */
};

/* A cache the library opens for a module's log level alone */

struct cache_level_s
{
  struct pnut_settings_cache_s cache;
  struct pnut_settings_desc_s desc;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void cache_register(FAR struct pnut_settings_cache_s *cache);
static void cache_list(FAR struct pnut_settings_cache_s *cache);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A module's log level (RFC 0026), in enum pnut_level_e's order */

static FAR const char * const g_cache_levels[] =
{
  "error", "warning", "info", "debug"
};

static const struct pnut_setting_decl_s g_cache_level =
{
  .key         = PNUT_SETTINGS_CACHE_LEVEL,
  .label       = "log.level",
  .group       = "log",
  .choices     = g_cache_levels,
  .nchoices    = sizeof(g_cache_levels) / sizeof(g_cache_levels[0]),
  .text        = "info",
  .type        = PNUT_SETTING_TYPE_CHOICE,
  .kind        = PNUT_SETTING_KIND_TEXT,
  .has_default = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cache_count, cache_decl
 *
 * Description:
 *   The settings a cache registers: its owner's, then the module's log
 *   level if it holds it.
 *
 ****************************************************************************/

static int cache_count(FAR const struct pnut_settings_cache_s *cache)
{
  return cache->desc->count + (cache->level ? 1 : 0);
}

static FAR const struct pnut_setting_decl_s *
cache_decl(FAR const struct pnut_settings_cache_s *cache, int i)
{
  return i < cache->desc->count ? &cache->desc->decls[i] : &g_cache_level;
}

/****************************************************************************
 * Name: cache_setlevel
 *
 * Description:
 *   Set the module's log level from its setting's value.
 *
 ****************************************************************************/

static void cache_setlevel(FAR struct pnut_settings_cache_s *cache,
                           FAR const pnut_setting_value_t *value)
{
  int i;

  if (value->which_value != PNUT_SETTING_VALUE_TEXT_TAG)
    {
      return;
    }

  for (i = 0; i < (int)g_cache_level.nchoices; i++)
    {
      if (strcmp(value->value.text, g_cache_levels[i]) == 0)
        {
          pnut_module_setlevel(cache->module, PNUT_LOG_ERROR + i);
          return;
        }
    }
}

/****************************************************************************
 * Name: cache_default
 *
 * Description:
 *   Put a setting's default in the owner's struct.
 *
 ****************************************************************************/

static void cache_default(FAR struct pnut_settings_cache_s *cache,
                          FAR const struct pnut_setting_decl_s *decl)
{
  FAR uint8_t *field = (FAR uint8_t *)cache->values + decl->offset;
  int64_t integer = decl->has_default ? decl->integer : 0;

  switch (decl->kind)
    {
      case PNUT_SETTING_KIND_BOOL:
        *(FAR bool *)field = integer != 0;
        break;

      case PNUT_SETTING_KIND_INT32:
        *(FAR int32_t *)field = (int32_t)integer;
        break;

      case PNUT_SETTING_KIND_UINT32:
        *(FAR uint32_t *)field = (uint32_t)integer;
        break;

      case PNUT_SETTING_KIND_INT64:
        *(FAR int64_t *)field = integer;
        break;

      case PNUT_SETTING_KIND_TEXT:
        if (decl->has_default && decl->text != NULL)
          {
            strlcpy((FAR char *)field, decl->text, decl->size);
          }
        else if (decl->type == PNUT_SETTING_TYPE_CHOICE &&
                 decl->nchoices > 0)
          {
            strlcpy((FAR char *)field, decl->choices[0], decl->size);
          }
        else
          {
            *(FAR char *)field = '\0';
          }

        break;
    }
}

/****************************************************************************
 * Name: cache_store
 *
 * Description:
 *   Put a value from Settings in the owner's struct.
 *
 * Returned Value:
 *   Whether the struct changed.
 *
 ****************************************************************************/

static bool cache_store(FAR struct pnut_settings_cache_s *cache,
                        FAR const struct pnut_setting_decl_s *decl,
                        FAR const pnut_setting_value_t *value)
{
  FAR uint8_t *field = (FAR uint8_t *)cache->values + decl->offset;
  uint8_t old[sizeof(int64_t)];
  bool changed;

  switch (decl->kind)
    {
      case PNUT_SETTING_KIND_BOOL:
        if (value->which_value != PNUT_SETTING_VALUE_BOOLEAN_TAG)
          {
            return false;
          }

        changed = *(FAR bool *)field != value->value.boolean;
        *(FAR bool *)field = value->value.boolean;
        return changed;

      case PNUT_SETTING_KIND_INT32:
      case PNUT_SETTING_KIND_UINT32:
      case PNUT_SETTING_KIND_INT64:
        if (value->which_value != PNUT_SETTING_VALUE_INTEGER_TAG)
          {
            return false;
          }

        memcpy(old, field, decl->size);
        if (decl->kind == PNUT_SETTING_KIND_INT32)
          {
            *(FAR int32_t *)field = (int32_t)value->value.integer;
          }
        else if (decl->kind == PNUT_SETTING_KIND_UINT32)
          {
            *(FAR uint32_t *)field = (uint32_t)value->value.integer;
          }
        else
          {
            *(FAR int64_t *)field = value->value.integer;
          }

        return memcmp(old, field, decl->size) != 0;

      case PNUT_SETTING_KIND_TEXT:
        if (value->which_value != PNUT_SETTING_VALUE_TEXT_TAG)
          {
            return false;
          }

        changed = strncmp((FAR char *)field, value->value.text,
                          decl->size) != 0;
        strlcpy((FAR char *)field, value->value.text, decl->size);
        return changed;

      default:
        return false;
    }
}

/****************************************************************************
 * Name: cache_retry
 ****************************************************************************/

static void cache_retry(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = arg;

  cache->timer = NULL;

  if (cache->state == CACHE_REGISTERING)
    {
      cache->next = 0;
      cache_register(cache);
    }
  else if (cache->state == CACHE_LISTING)
    {
      cache->after[0] = '\0';
      cache_list(cache);
    }
}

/****************************************************************************
 * Name: cache_later
 *
 * Description:
 *   Try the current step again later, from its start, unless the
 *   connection is down: it starts afresh when it comes back.
 *
 ****************************************************************************/

static void cache_later(FAR struct pnut_settings_cache_s *cache)
{
  int ret;

  if (cache->timer != NULL || cache->link == NULL ||
      cache->link->client.client == NULL ||
      !pnut_client_connected(cache->link->client.client))
    {
      return;
    }

  ret = pnut_timer_start(cache->module->loop, cache->backoff, 0,
                         cache_retry, cache, &cache->timer);
  if (ret < 0)
    {
      pnut_log(cache->module, PNUT_LOG_ERROR,
               "Settings of %s: no timer to try again: %d",
               cache->desc->owner, ret);
      cache->timer = NULL;
      cache->state = CACHE_IDLE;
      return;
    }

  cache->backoff *= 2;
  if (cache->backoff > CACHE_BACKOFF_MAX)
    {
      cache->backoff = CACHE_BACKOFF_MAX;
    }
}

/****************************************************************************
 * Name: cache_failed
 *
 * Description:
 *   A call failed: try again later when it may work then, or give up.
 *
 ****************************************************************************/

static void cache_failed(FAR struct pnut_settings_cache_s *cache,
                         FAR const char *what, int status,
                         FAR const pnut_error_t *err)
{
  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_TIMEOUT ||
      status == PNUT_STATUS_BUSY)
    {
      /* No room left in Settings lasts until it has room, its limits
       * grown or another owner's schema shrunk, unlike Settings starting
       * or busy for a moment: the owner's settings stay at their defaults
       * meanwhile, which is worth a warning, once
       */

      if (err != NULL && err->code == PNUT_SETTINGS_ERROR_CODE_FULL)
        {
          if (!cache->full)
            {
              pnut_log(cache->module, PNUT_LOG_WARNING,
                       "Settings of %s: no room left in Settings; at the "
                       "defaults, trying again", cache->desc->owner);
              cache->full = true;
            }
        }
      else
        {
          pnut_log(cache->module, PNUT_LOG_DEBUG,
                   "Settings of %s: %s: status %d; trying again",
                   cache->desc->owner, what, status);
        }

      cache_later(cache);
      return;
    }

  pnut_log(cache->module, PNUT_LOG_ERROR,
           "Settings of %s: %s: status %d, code %lu %s",
           cache->desc->owner, what, status,
           err != NULL ? (unsigned long)err->code : 0ul,
           err != NULL ? err->text : "");
  cache->state = CACHE_IDLE;
}

/****************************************************************************
 * Name: cache_listed
 ****************************************************************************/

static void cache_listed(FAR struct pnut_settings_client_s *client,
                         int status, FAR const pnut_settings_values_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = arg;
  FAR const struct pnut_settings_desc_s *desc;
  uint64_t touched;
  bool first;
  pb_size_t i;
  int j;

  /* A cache closed since the call has no answer */

  if (cache == NULL || cache->state != CACHE_LISTING)
    {
      return;
    }

  desc = cache->desc;

  if (status != PNUT_STATUS_OK)
    {
      cache_failed(cache, "List", status, err);
      return;
    }

  for (i = 0; i < out->entries_count; i++)
    {
      if (cache->level && out->entries[i].has_value &&
          strcmp(out->entries[i].key, PNUT_SETTINGS_CACHE_LEVEL) == 0)
        {
          cache_setlevel(cache, &out->entries[i].value);
          continue;
        }

      for (j = 0; j < desc->count; j++)
        {
          if (strcmp(desc->decls[j].key, out->entries[i].key) == 0)
            {
              if (out->entries[i].has_value &&
                  cache_store(cache, &desc->decls[j],
                              &out->entries[i].value))
                {
                  cache->touched |= (uint64_t)1 << j;
                }

              break;
            }
        }
    }

  if (out->more && out->entries_count > 0)
    {
      strlcpy(cache->after, out->entries[out->entries_count - 1].key,
              sizeof(cache->after));
      cache_list(cache);
      return;
    }

  /* Read whole: once more if a change came meanwhile, then tell */

  if (cache->again)
    {
      cache->again    = false;
      cache->after[0] = '\0';
      cache_list(cache);
      return;
    }

  cache->state   = CACHE_WATCHING;
  cache->backoff = CACHE_BACKOFF_MIN;
  first          = !cache->ready;
  cache->ready   = true;
  touched        = cache->touched;
  cache->touched = 0;

  if (cache->changed == NULL)
    {
      return;
    }

  if (first)
    {
      cache->changed(cache, PNUT_SETTINGS_CACHE_ALL, cache->arg);
      return;
    }

  for (j = 0; j < desc->count; j++)
    {
      /* A handler may close the cache */

      if (cache->state != CACHE_WATCHING)
        {
          break;
        }

      if ((touched & ((uint64_t)1 << j)) != 0)
        {
          cache->changed(cache, j, cache->arg);
        }
    }
}

/****************************************************************************
 * Name: cache_list
 *
 * Description:
 *   Read a page of the owner's values, after cache->after.
 *
 ****************************************************************************/

static void cache_list(FAR struct pnut_settings_cache_s *cache)
{
  pnut_settings_page_request_t req;
  int ret;

  if (cache->after[0] == '\0')
    {
      cache->again = false;
    }

  cache->state = CACHE_LISTING;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, cache->desc->owner, sizeof(req.owner));
  strlcpy(req.after, cache->after, sizeof(req.after));

  ret = pnut_settings_list(&cache->link->client, &req, 0, cache_listed,
                           cache);
  if (ret < 0)
    {
      cache_later(cache);
    }
}

/****************************************************************************
 * Name: cache_registered
 ****************************************************************************/

static void cache_registered(FAR struct pnut_settings_client_s *client,
                             int status,
                             FAR const pnut_settings_reply_t *out,
                             FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = arg;

  if (cache == NULL || cache->state != CACHE_REGISTERING)
    {
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      cache_failed(cache, "RegisterSchema", status, err);
      return;
    }

  cache->next += CACHE_PAGE;
  if (cache->next < cache_count(cache))
    {
      cache_register(cache);
      return;
    }

  if (cache->full)
    {
      pnut_log(cache->module, PNUT_LOG_INFO,
               "Settings of %s: registered, with room now",
               cache->desc->owner);
      cache->full = false;
    }

  cache->after[0] = '\0';
  cache_list(cache);
}

/****************************************************************************
 * Name: cache_declare
 *
 * Description:
 *   Write a declaration as RegisterSchema takes it.
 *
 ****************************************************************************/

static void cache_declare(FAR const struct pnut_setting_decl_s *decl,
                          FAR pnut_setting_t *setting)
{
  uint8_t i;

  strlcpy(setting->key, decl->key, sizeof(setting->key));
  setting->type       = decl->type;
  setting->min        = decl->min;
  setting->max        = decl->max;
  setting->max_length = decl->max_length;
  setting->is_public  = decl->is_public;

  if (decl->label != NULL)
    {
      strlcpy(setting->label, decl->label, sizeof(setting->label));
    }

  if (decl->group != NULL)
    {
      strlcpy(setting->group, decl->group, sizeof(setting->group));
    }

  if (decl->unit != NULL)
    {
      strlcpy(setting->unit, decl->unit, sizeof(setting->unit));
    }

  for (i = 0; i < decl->nchoices; i++)
    {
      strlcpy(setting->choices[i], decl->choices[i],
              sizeof(setting->choices[i]));
    }

  setting->choices_count     = decl->nchoices;
  setting->has_default_value = decl->has_default;

  if (decl->has_default)
    {
      switch (decl->type)
        {
          case PNUT_SETTING_TYPE_BOOLEAN:
            setting->default_value.which_value =
              PNUT_SETTING_VALUE_BOOLEAN_TAG;
            setting->default_value.value.boolean = decl->integer != 0;
            break;

          case PNUT_SETTING_TYPE_INTEGER:
            setting->default_value.which_value =
              PNUT_SETTING_VALUE_INTEGER_TAG;
            setting->default_value.value.integer = decl->integer;
            break;

          default:
            setting->default_value.which_value =
              PNUT_SETTING_VALUE_TEXT_TAG;
            strlcpy(setting->default_value.value.text, decl->text,
                    sizeof(setting->default_value.value.text));
            break;
        }
    }
}

/****************************************************************************
 * Name: cache_register
 *
 * Description:
 *   Send the page of the schema from cache->next.  The request is large
 *   (three declarations with room for their choices), so it lives on the
 *   heap only while it is encoded.
 *
 ****************************************************************************/

static void cache_register(FAR struct pnut_settings_cache_s *cache)
{
  FAR const struct pnut_settings_desc_s *desc = cache->desc;
  FAR pnut_settings_register_request_t *req;
  int count = cache_count(cache);
  int ret;
  int i;

  cache->state = CACHE_REGISTERING;

  req = calloc(1, sizeof(*req));
  if (req == NULL)
    {
      cache_later(cache);
      return;
    }

  strlcpy(req->owner, desc->owner, sizeof(req->owner));
  req->first = cache->next == 0;

  for (i = cache->next; i < count && i < cache->next + CACHE_PAGE; i++)
    {
      cache_declare(cache_decl(cache, i),
                    &req->settings[req->settings_count++]);
    }

  req->last = i >= count;

  ret = pnut_settings_register_schema(&cache->link->client, req, 0,
                                      cache_registered, cache);
  free(req);

  if (ret < 0)
    {
      cache_later(cache);
    }
}

/****************************************************************************
 * Name: cache_reset
 *
 * Description:
 *   Settings came up, or went away: register and read everything again
 *   when it is back, since it may have restarted.  The last page of the
 *   schema makes Settings announce the owner's settings, which reads them
 *   once more as they are first read: harmless.
 *
 ****************************************************************************/

static void cache_reset(FAR struct pnut_settings_cache_s *cache,
                        bool connected)
{
  if (cache->timer != NULL)
    {
      pnut_timer_cancel(cache->module->loop, cache->timer);
      cache->timer = NULL;
    }

  cache->state   = CACHE_IDLE;
  cache->again   = false;
  cache->touched = 0;

  if (connected)
    {
      cache->backoff = CACHE_BACKOFF_MIN;
      cache->next    = 0;
      cache_register(cache);
    }
}

/****************************************************************************
 * Name: link_state
 *
 * Description:
 *   The connection came up, or went away: every cache starts again.
 *
 ****************************************************************************/

static void link_state(FAR struct pnut_settings_client_s *client,
                       bool connected, FAR void *arg)
{
  FAR struct pnut_settings_link_s *link = arg;
  FAR struct pnut_settings_cache_s *cache;

  link->versioned = false;

  for (cache = link->caches; cache != NULL; cache = link->next)
    {
      link->next = cache->flink;
      cache_reset(cache, connected);
    }

  link->next = NULL;
}

/****************************************************************************
 * Name: cache_reread
 *
 * Description:
 *   Read the owner's values again, now or after the read under way.
 *
 ****************************************************************************/

static void cache_reread(FAR struct pnut_settings_cache_s *cache)
{
  if (cache->state == CACHE_WATCHING)
    {
      cache->after[0] = '\0';
      cache_list(cache);
    }
  else if (cache->state == CACHE_LISTING)
    {
      cache->again = true;
    }
}

/****************************************************************************
 * Name: link_change
 *
 * Description:
 *   A change on the settings topic: the owner's, or a gap in the versions,
 *   which may have hidden one, reads an owner's values again.
 *
 ****************************************************************************/

static void link_change(FAR struct pnut_settings_change_reader_s *reader,
                        FAR const pnut_settings_change_t *msg,
                        FAR void *arg)
{
  FAR struct pnut_settings_link_s *link = arg;
  FAR struct pnut_settings_cache_s *cache;
  bool missed;

  missed = link->versioned && msg->version != link->version + 1;
  link->version   = msg->version;
  link->versioned = true;

  for (cache = link->caches; cache != NULL; cache = link->next)
    {
      link->next = cache->flink;
      if (missed || strcmp(msg->owner, cache->desc->owner) == 0)
        {
          cache_reread(cache);
        }
    }

  link->next = NULL;
}

/****************************************************************************
 * Name: cache_answered
 ****************************************************************************/

static void cache_answered(FAR struct pnut_settings_client_s *client,
                           int status, FAR const pnut_settings_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct pnut_settings_set_s *set = arg;
  pnut_settings_done_t done;
  FAR void *darg;

  /* A cache closed since the call has no answer */

  if (set == NULL)
    {
      return;
    }

  done      = set->done;
  darg      = set->arg;
  set->done = NULL;
  set->arg  = NULL;

  if (done != NULL)
    {
      done(set->cache, status, err != NULL ? err->code : 0, darg);
    }
}

/****************************************************************************
 * Name: link_attach
 *
 * Description:
 *   Add a cache to its program's link, made at the first, and open the
 *   link if it is not: the cache registers once it is connected.
 *
 ****************************************************************************/

static int link_attach(FAR struct pnut_settings_cache_s *cache)
{
  FAR struct pnut_loop_s *loop = cache->module->loop;
  FAR struct pnut_settings_link_s *link = loop->settings;
  int ret;

  if (link == NULL)
    {
      link = calloc(1, sizeof(*link));
      if (link == NULL)
        {
          return -ENOMEM;
        }

      link->loop     = loop;
      loop->settings = link;
    }

  cache->link  = link;
  cache->flink = link->caches;
  link->caches = cache;

  if (link->client.client != NULL)
    {
      if (pnut_client_connected(link->client.client))
        {
          cache_register(cache);
        }

      return OK;
    }

  /* The topic first, so that no change after the first read is missed */

  link->versioned = false;

  ret = pnut_settings_change_subscribe(loop, link_change, link,
                                       &link->reader);
  if (ret < 0)
    {
      pnut_log(cache->module, PNUT_LOG_WARNING,
               "Settings of %s: no settings topic: %d; changes will not "
               "be seen", cache->desc->owner, ret);
    }
  else
    {
      link->subscribed = true;
    }

  ret = pnut_settings_connect(loop, PNUT_SETTINGS_CACHE_SERVICE,
                              link_state, link, &link->client);
  if (ret < 0)
    {
      link->client.client = NULL;
      if (link->subscribed)
        {
          pnut_settings_change_unsubscribe(&link->reader);
          link->subscribed = false;
        }

      link->caches = cache->flink;
      cache->link  = NULL;
      cache->flink = NULL;
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: link_detach
 *
 * Description:
 *   Take a cache off its link: the answers to its calls still in flight
 *   are dropped, and the link closes with its last cache.
 *
 ****************************************************************************/

static void link_detach(FAR struct pnut_settings_cache_s *cache)
{
  FAR struct pnut_settings_link_s *link = cache->link;
  FAR struct pnut_settings_cache_s **pp;
  FAR struct pnut_settings_call_s *call;
  FAR uint8_t *arg;
  int i;

  for (pp = &link->caches; *pp != NULL; pp = &(*pp)->flink)
    {
      if (*pp == cache)
        {
          *pp = cache->flink;
          break;
        }
    }

  if (link->next == cache)
    {
      link->next = cache->flink;
    }

  for (i = 0; i < PNUT_CLIENT_INFLIGHT; i++)
    {
      call = &link->client.calls[i];
      arg  = call->arg;
      if (call->used &&
          (arg == (FAR uint8_t *)cache ||
           (arg >= (FAR uint8_t *)cache->sets &&
            arg < (FAR uint8_t *)(cache->sets + PNUT_CLIENT_INFLIGHT))))
        {
          call->arg = NULL;
        }
    }

  cache->link  = NULL;
  cache->flink = NULL;

  if (link->caches != NULL)
    {
      return;
    }

  if (link->subscribed)
    {
      pnut_settings_change_unsubscribe(&link->reader);
      link->subscribed = false;
    }

  if (link->client.client != NULL)
    {
      pnut_settings_disconnect(&link->client);
    }
}

/****************************************************************************
 * Name: level_close
 *
 * Description:
 *   Close and free the cache the library opened for a module's level.
 *
 ****************************************************************************/

static void level_close(FAR struct pnut_module_s *module)
{
  FAR struct pnut_settings_cache_s *cache = module->levels;

  if (cache != NULL)
    {
      module->levels = NULL;
      pnut_settings_cache_close(cache);

      /* The cache is the first of struct cache_level_s */

      free(cache);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_settings_cache_open
 ****************************************************************************/

int pnut_settings_cache_open(FAR struct pnut_module_s *module,
                             FAR const struct pnut_settings_desc_s *desc,
                             FAR void *values,
                             pnut_settings_changed_t changed,
                             FAR void *arg,
                             FAR struct pnut_settings_cache_s *cache)
{
  int ret;
  int i;

  if (desc->count > PNUT_SETTINGS_CACHE_MAX)
    {
      return -E2BIG;
    }

  memset(cache, 0, sizeof(*cache));
  cache->module  = module;
  cache->desc    = desc;
  cache->values  = values;
  cache->changed = changed;
  cache->arg     = arg;
  cache->backoff = CACHE_BACKOFF_MIN;
  cache->level   = module->loop->config.levels &&
                   strcmp(desc->owner, module->name) == 0;

  for (i = 0; i < desc->count; i++)
    {
      cache_default(cache, &desc->decls[i]);
    }

  ret = link_attach(cache);
  if (ret < 0)
    {
      return ret;
    }

  /* The owner holds the level now: the library's cache of it alone goes,
   * so that the two never register the owner's schema in turn.  The
   * owner's registers after the other's, on the same connection.
   */

  if (cache->level && module->levels != NULL)
    {
      level_close(module);
    }

  return OK;
}

/****************************************************************************
 * Name: pnut_settings_cache_close
 ****************************************************************************/

void pnut_settings_cache_close(FAR struct pnut_settings_cache_s *cache)
{
  if (cache->timer != NULL)
    {
      pnut_timer_cancel(cache->module->loop, cache->timer);
      cache->timer = NULL;
    }

  if (cache->link != NULL)
    {
      link_detach(cache);
    }

  cache->state = CACHE_IDLE;
  cache->ready = false;
}

/****************************************************************************
 * Name: pnut_settings_cache_ready
 ****************************************************************************/

bool
pnut_settings_cache_ready(FAR const struct pnut_settings_cache_s *cache)
{
  return cache->ready;
}

/****************************************************************************
 * Name: pnut_settings_cache_set
 ****************************************************************************/

int pnut_settings_cache_set(FAR struct pnut_settings_cache_s *cache,
                            int index,
                            FAR const pnut_setting_value_t *value,
                            pnut_settings_done_t done, FAR void *arg)
{
  FAR struct pnut_settings_set_s *set;
  pnut_settings_set_request_t req;
  int slot;
  int ret;

  if (index < 0 || index >= cache->desc->count)
    {
      return -EINVAL;
    }

  if (cache->link == NULL || cache->link->client.client == NULL)
    {
      return -ENOTCONN;
    }

  for (slot = 0; slot < PNUT_CLIENT_INFLIGHT; slot++)
    {
      if (cache->sets[slot].done == NULL && cache->sets[slot].arg == NULL)
        {
          break;
        }
    }

  if (slot == PNUT_CLIENT_INFLIGHT)
    {
      return -EBUSY;
    }

  set = &cache->sets[slot];

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, cache->desc->owner, sizeof(req.owner));
  strlcpy(req.key, cache->desc->decls[index].key, sizeof(req.key));
  req.has_value = true;
  req.value     = *value;

  ret = pnut_settings_set(&cache->link->client, &req, 0, cache_answered,
                          set);
  if (ret < 0)
    {
      return ret;
    }

  /* A slot with neither is free: one without a handler holds a marker */

  set->cache = cache;
  set->done  = done;
  set->arg   = done != NULL ? arg : (FAR void *)cache;
  return OK;
}

/****************************************************************************
 * Name: pnut_settings_levels_open
 ****************************************************************************/

void pnut_settings_levels_open(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_settings_cache_s *cache;
  FAR struct pnut_module_s *module;
  FAR struct cache_level_s *own;
  int ret;

  if (!loop->config.levels)
    {
      return;
    }

  for (module = loop->first; module != NULL; module = module->next)
    {
      if (module->levels != NULL)
        {
          continue;
        }

      /* A cache of an owner named as the module holds its level; another
       * module's cache of that owner is a mistake, which two caches
       * registering the owner in turn would hide
       */

      cache = loop->settings != NULL ? loop->settings->caches : NULL;
      while (cache != NULL &&
             strcmp(cache->desc->owner, module->name) != 0)
        {
          cache = cache->flink;
        }

      if (cache != NULL)
        {
          if (cache->module != module)
            {
              pnut_log(module, PNUT_LOG_WARNING,
                       "Its settings' owner is %s's: no log level's "
                       "setting", cache->module->name);
            }

          continue;
        }

      own = calloc(1, sizeof(*own));
      if (own == NULL)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No memory for its log level's setting");
          continue;
        }

      own->desc.owner = module->name;
      ret = pnut_settings_cache_open(module, &own->desc, NULL, NULL, NULL,
                                     &own->cache);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No log level's setting: %d", ret);
          free(own);
          continue;
        }

      module->levels = &own->cache;
    }
}

/****************************************************************************
 * Name: pnut_settings_link_free
 ****************************************************************************/

void pnut_settings_link_free(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_settings_link_s *link = loop->settings;
  FAR struct pnut_settings_cache_s *cache;

  if (link == NULL)
    {
      return;
    }

  /* A cache left open: the connection goes with the loop, and closing
   * the cache later does nothing
   */

  if (link->caches != NULL)
    {
      pnut_loop_log(loop, PNUT_LOG_WARNING,
                    "Settings of %s: still open as the loop goes",
                    link->caches->desc->owner);
    }

  while ((cache = link->caches) != NULL)
    {
      link->caches = cache->flink;
      if (cache->timer != NULL)
        {
          pnut_timer_cancel(loop, cache->timer);
          cache->timer = NULL;
        }

      cache->link  = NULL;
      cache->flink = NULL;
      cache->state = CACHE_IDLE;
    }

  if (link->subscribed)
    {
      pnut_settings_change_unsubscribe(&link->reader);
    }

  if (link->client.client != NULL)
    {
      pnut_settings_disconnect(&link->client);
    }

  loop->settings = NULL;
  free(link);
}

/****************************************************************************
 * Name: pnut_settings_levels_close
 ****************************************************************************/

void pnut_settings_levels_close(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_module_s *module;

  for (module = loop->first; module != NULL; module = module->next)
    {
      level_close(module);
    }
}
