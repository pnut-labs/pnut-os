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

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void cache_register(FAR struct pnut_settings_cache_s *cache);
static void cache_list(FAR struct pnut_settings_cache_s *cache);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cache_of
 *
 * Description:
 *   The cache a client is part of.
 *
 ****************************************************************************/

static FAR struct pnut_settings_cache_s *
cache_of(FAR struct pnut_settings_client_s *client)
{
  return (FAR struct pnut_settings_cache_s *)
         ((FAR uint8_t *)client -
          offsetof(struct pnut_settings_cache_s, client));
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

  if (cache->timer != NULL ||
      !pnut_client_connected(cache->client.client))
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
      pnut_log(cache->module, PNUT_LOG_DEBUG,
               "Settings of %s: %s: status %d; trying again",
               cache->desc->owner, what, status);
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
  FAR const struct pnut_settings_desc_s *desc = cache->desc;
  uint64_t touched;
  bool first;
  pb_size_t i;
  int j;

  if (cache->state != CACHE_LISTING)
    {
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      cache_failed(cache, "List", status, err);
      return;
    }

  for (i = 0; i < out->entries_count; i++)
    {
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

  ret = pnut_settings_list(&cache->client, &req, 0, cache_listed, cache);
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

  if (cache->state != CACHE_REGISTERING)
    {
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      cache_failed(cache, "RegisterSchema", status, err);
      return;
    }

  cache->next += CACHE_PAGE;
  if (cache->next < cache->desc->count)
    {
      cache_register(cache);
      return;
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

  for (i = cache->next; i < desc->count && i < cache->next + CACHE_PAGE;
       i++)
    {
      cache_declare(&desc->decls[i], &req->settings[req->settings_count++]);
    }

  req->last = i >= desc->count;

  ret = pnut_settings_register_schema(&cache->client, req, 0,
                                      cache_registered, cache);
  free(req);

  if (ret < 0)
    {
      cache_later(cache);
    }
}

/****************************************************************************
 * Name: cache_state
 *
 * Description:
 *   Settings came up, or went away: register and read everything again
 *   when it is back, since it may have restarted.  The last page of the
 *   schema makes Settings announce the owner's settings, which reads them
 *   once more as they are first read: harmless.
 *
 ****************************************************************************/

static void cache_state(FAR struct pnut_settings_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = arg;

  if (cache->timer != NULL)
    {
      pnut_timer_cancel(cache->module->loop, cache->timer);
      cache->timer = NULL;
    }

  cache->state     = CACHE_IDLE;
  cache->again     = false;
  cache->touched   = 0;
  cache->versioned = false;

  if (connected)
    {
      cache->backoff = CACHE_BACKOFF_MIN;
      cache->next    = 0;
      cache_register(cache);
    }
}

/****************************************************************************
 * Name: cache_change
 *
 * Description:
 *   A change on the settings topic: the owner's, or a gap in the versions,
 *   which may have hidden one, reads the values again.
 *
 ****************************************************************************/

static void cache_change(FAR struct pnut_settings_change_reader_s *reader,
                         FAR const pnut_settings_change_t *msg,
                         FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = arg;
  bool missed;

  missed = cache->versioned && msg->version != cache->version + 1;
  cache->version   = msg->version;
  cache->versioned = true;

  if (!missed && strcmp(msg->owner, cache->desc->owner) != 0)
    {
      return;
    }

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
 * Name: cache_answered
 ****************************************************************************/

static void cache_answered(FAR struct pnut_settings_client_s *client,
                           int status, FAR const pnut_settings_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct pnut_settings_cache_s *cache = cache_of(client);
  int slot = (int)(uintptr_t)arg;
  pnut_settings_done_t done = cache->sets[slot].done;
  FAR void *darg = cache->sets[slot].arg;

  cache->sets[slot].done = NULL;
  cache->sets[slot].arg  = NULL;

  if (done != NULL)
    {
      done(cache, status, err != NULL ? err->code : 0, darg);
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

  for (i = 0; i < desc->count; i++)
    {
      cache_default(cache, &desc->decls[i]);
    }

  /* The topic first, so that no change after the first read is missed */

  ret = pnut_settings_change_subscribe(module->loop, cache_change, cache,
                                       &cache->reader);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING,
               "Settings of %s: no settings topic: %d; changes will not "
               "be seen", desc->owner, ret);
    }
  else
    {
      cache->subscribed = true;
    }

  ret = pnut_settings_connect(module->loop, PNUT_SETTINGS_CACHE_SERVICE,
                              cache_state, cache, &cache->client);
  if (ret < 0)
    {
      if (cache->subscribed)
        {
          pnut_settings_change_unsubscribe(&cache->reader);
          cache->subscribed = false;
        }

      return ret;
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

  if (cache->subscribed)
    {
      pnut_settings_change_unsubscribe(&cache->reader);
      cache->subscribed = false;
    }

  pnut_settings_disconnect(&cache->client);
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
  pnut_settings_set_request_t req;
  int slot;
  int ret;

  if (index < 0 || index >= cache->desc->count)
    {
      return -EINVAL;
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

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, cache->desc->owner, sizeof(req.owner));
  strlcpy(req.key, cache->desc->decls[index].key, sizeof(req.key));
  req.has_value = true;
  req.value     = *value;

  ret = pnut_settings_set(&cache->client, &req, 0, cache_answered,
                          (FAR void *)(uintptr_t)slot);
  if (ret < 0)
    {
      return ret;
    }

  /* A slot with neither is free: one without a handler holds a marker */

  cache->sets[slot].done = done;
  cache->sets[slot].arg  = done != NULL ? arg : (FAR void *)cache;
  return OK;
}
