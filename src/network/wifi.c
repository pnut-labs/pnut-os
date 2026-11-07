/****************************************************************************
 * pnut-os/src/network/wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Wi-Fi (RFC 0031), a module of the network program: the radio up while
 * wifi.enabled is, a scan streamed to whoever asked, a network joined and
 * an address taken, the link watched and a lost network joined again,
 * the state on the wifi topic.  Every call into the driver blocks, so it
 * runs on a worker, one at a time; the loop keeps the state.
 *
 * As the system before this one found on the same hardware: each join
 * starts from a radio taken down and brought up again, since after a
 * failed join every scan and join failed until it was.
 *
 * A network joined becomes known: up to 16, last used first, kept as
 * Wi-Fi's settings, their passphrases as secrets in Settings, never in
 * Wi-Fi's memory beyond a join.  Whenever Wi-Fi is on and no network is
 * joined, it scans and joins a known one in reach: the one joined last,
 * else the strongest.  A join the owner asked for that fails is reported
 * and not tried on its own again until the owner joins it again: the
 * driver cannot tell a wrong passphrase from a network out of reach.  One
 * joined on its own that fails is tried less and less often, up to every
 * 8 minutes, others in reach meanwhile (RFC 0031).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_UI           "ui"    /* The system UI's program */
#define WIFI_LINK_MS      10000
#define WIFI_AGAIN_MS     30000
#define WIFI_AGAIN_MAX_MS 480000  /* 8 minutes (RFC 0031) */
#define WIFI_IDLE_MS      60000
#define WIFI_KICK_MS      1000    /* A job the worker would not take */
#define WIFI_SAVE_MS      1000    /* A write's failed call, again: the */
#define WIFI_SAVE_MAX_MS  60000   /* wait doubled up to a minute */

/* A slot's settings: its network's name, security and passphrase */

#define WIFI_NET(i)       (PNUT_WIFI_SETTINGS_NET_0 + 3 * (i))
#define WIFI_SEC(i)       (PNUT_WIFI_SETTINGS_SEC_0 + 3 * (i))
#define WIFI_PSK(i)       (PNUT_WIFI_SETTINGS_PSK_0 + 3 * (i))

/* A slot's plan in a write: left; the passphrase of the network just
 * joined; none; or else the slot whose passphrase it takes
 */

#define WIFI_FROM_KEEP    (-1)
#define WIFI_FROM_PASS    (-2)
#define WIFI_FROM_NONE    (-3)

/* A name escaped: each byte as \xNN at most, and a NUL */

#define WIFI_ESCAPED_MAX  (4 * (WIFI_NAME_MAX - 1) + 1)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wifi_next(FAR struct wifi_s *wifi);
static void wifi_save(FAR struct wifi_s *wifi);
static void wifi_stop_seeking(FAR struct wifi_s *wifi);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t wifi_ms(uint32_t value, uint32_t fallback)
{
  return value != 0 ? value : fallback;
}

/* The slots' settings lie three by three, as wifi.proto declares them */

#define WIFI_LAID_OUT \
  (PNUT_WIFI_SETTINGS_SEC_0 == PNUT_WIFI_SETTINGS_NET_0 + 1 && \
   PNUT_WIFI_SETTINGS_PSK_0 == PNUT_WIFI_SETTINGS_NET_0 + 2 && \
   PNUT_WIFI_SETTINGS_NET_1 == PNUT_WIFI_SETTINGS_NET_0 + 3 && \
   PNUT_WIFI_SETTINGS_PSK_15 == PNUT_WIFI_SETTINGS_NET_0 + 47)

typedef char wifi_slots_laid_out[WIFI_LAID_OUT ? 1 : -1];

/* The monotonic clock, in milliseconds */

static uint64_t wifi_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: wifi_escape, wifi_unescape
 *
 * Description:
 *   A network's name as a slot keeps it (RFC 0031): a name need not be
 *   UTF-8, so each byte that is not part of a UTF-8 character, each
 *   control character and each backslash is written as \xNN.  Unescaped,
 *   false for a text that is no name: too long, or with a NUL.
 *
 ****************************************************************************/

static int wifi_utf8_len(FAR const uint8_t *p)
{
  int n;
  int i;

  if (p[0] < 0x80)
    {
      return 1;
    }

  n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
      p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
      p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
  for (i = 1; i < n; i++)
    {
      if ((p[i] & 0xc0) != 0x80)
        {
          return 0;
        }
    }

  /* No overlong form, surrogate or code point past U+10FFFF */

  if ((n == 3 && p[0] == 0xe0 && p[1] < 0xa0) ||
      (n == 3 && p[0] == 0xed && p[1] >= 0xa0) ||
      (n == 4 && p[0] == 0xf0 && p[1] < 0x90) ||
      (n == 4 && p[0] == 0xf4 && p[1] >= 0x90))
    {
      return 0;
    }

  return n;
}

static void wifi_escape(FAR const char *name, FAR char *out)
{
  FAR const uint8_t *p = (FAR const uint8_t *)name;
  int n;

  while (*p != '\0')
    {
      n = wifi_utf8_len(p);
      if (n == 1 && *p >= 0x20 && *p != 0x7f && *p != '\\')
        {
          *out++ = (char)*p++;
        }
      else if (n > 1)
        {
          memcpy(out, p, n);
          out += n;
          p   += n;
        }
      else
        {
          out += sprintf(out, "\\x%02x", *p++);
        }
    }

  *out = '\0';
}

static int wifi_hex(char c)
{
  return c >= '0' && c <= '9' ? c - '0' :
         c >= 'a' && c <= 'f' ? c - 'a' + 10 :
         c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool wifi_unescape(FAR const char *text, FAR char *name)
{
  size_t len = 0;
  int hi;
  int lo;

  while (*text != '\0')
    {
      if (len >= WIFI_NAME_MAX - 1)
        {
          return false;
        }

      hi = text[0] == '\\' && text[1] == 'x' ? wifi_hex(text[2]) : -1;
      lo = hi >= 0 ? wifi_hex(text[3]) : -1;
      if (lo >= 0)
        {
          if (hi == 0 && lo == 0)
            {
              return false;
            }

          name[len++] = (char)(hi << 4 | lo);
          text += 4;
        }
      else
        {
          name[len++] = *text++;
        }
    }

  name[len] = '\0';
  return true;
}

/* A setting's value in the settings' struct, a string's */

static FAR const char *wifi_text(FAR struct wifi_s *wifi, int index)
{
  return (FAR const char *)&wifi->settings.values +
         g_pnut_wifi_settings_desc.decls[index].offset;
}

/****************************************************************************
 * Name: wifi_known_find, wifi_known_front, wifi_known_remove
 *
 * Description:
 *   A known network by its name and security, or -1; one put first, the
 *   network used longest ago forgotten if there is no room; one taken
 *   away.
 *
 ****************************************************************************/

static int wifi_known_find(FAR struct wifi_s *wifi, FAR const char *name,
                           bool secured)
{
  int i;

  for (i = 0; i < WIFI_KNOWN_MAX && wifi->known[i].name[0] != '\0'; i++)
    {
      if (wifi->known[i].secured == secured &&
          strcmp(wifi->known[i].name, name) == 0)
        {
          return i;
        }
    }

  return -1;
}

static FAR struct wifi_known_s *wifi_known_front(FAR struct wifi_s *wifi,
                                                 FAR const char *name,
                                                 bool secured)
{
  struct wifi_known_s k;
  int at = wifi_known_find(wifi, name, secured);

  if (at < 0)
    {
      memset(&k, 0, sizeof(k));
      strlcpy(k.name, name, sizeof(k.name));
      k.secured = secured;
      at = WIFI_KNOWN_MAX - 1;
      if (wifi->known[at].name[0] != '\0')
        {
          pnut_log(&wifi->module, PNUT_LOG_INFO,
                   "\"%s\" forgotten: used longest ago",
                   wifi->known[at].name);
        }

      explicit_bzero(&wifi->known[at], sizeof(wifi->known[at]));
    }
  else
    {
      k = wifi->known[at];
    }

  memmove(&wifi->known[1], &wifi->known[0], at * sizeof(wifi->known[0]));
  wifi->known[0] = k;
  explicit_bzero(&k, sizeof(k));
  return &wifi->known[0];
}

static void wifi_known_remove(FAR struct wifi_s *wifi, int at)
{
  explicit_bzero(&wifi->known[at], sizeof(wifi->known[at]));
  memmove(&wifi->known[at], &wifi->known[at + 1],
          (WIFI_KNOWN_MAX - 1 - at) * sizeof(wifi->known[0]));
  memset(&wifi->known[WIFI_KNOWN_MAX - 1], 0, sizeof(wifi->known[0]));
}

/****************************************************************************
 * Name: wifi_publish
 ****************************************************************************/

static void wifi_publish(FAR struct wifi_s *wifi)
{
  if (wifi->topic != NULL)
    {
      pnut_wifi_info_publish(wifi->topic, &wifi->state);
    }
}

static void wifi_set_state(FAR struct wifi_s *wifi, uint8_t state)
{
  wifi->state.state = state;
  if (state != PNUT_WIFI_INFO_STATE_JOINED)
    {
      wifi->state.signal  = 0;
      wifi->state.address = 0;
    }

  if (state == PNUT_WIFI_INFO_STATE_OFF ||
      state == PNUT_WIFI_INFO_STATE_ON)
    {
      wifi->state.name[0] = '\0';
    }

  wifi_publish(wifi);
}

/****************************************************************************
 * Name: wifi_forget_pass
 *
 * Description:
 *   A join's passphrase, and its network's name, wiped.
 *
 ****************************************************************************/

static void wifi_forget_pass(FAR struct wifi_s *wifi)
{
  explicit_bzero(wifi->passphrase, sizeof(wifi->passphrase));
  wifi->name[0] = '\0';
}

/****************************************************************************
 * Name: wifi_fail
 ****************************************************************************/

static void wifi_fail(FAR struct wifi_s *wifi,
                      FAR const struct pnut_request_s *req, int status,
                      uint32_t code, FAR const char *text)
{
  if (wifi->serving)
    {
      pnut_wifi_fail(&wifi->server, req, status, code, text);
    }
}

/****************************************************************************
 * Name: wifi_join_answer
 *
 * Description:
 *   The join request waiting, if one does, answered: OK, or failed with
 *   a code.
 *
 ****************************************************************************/

static void wifi_join_answer(FAR struct wifi_s *wifi, uint32_t code)
{
  pnut_wifi_reply_t reply;

  if (!wifi->joining_req)
    {
      return;
    }

  wifi->joining_req = false;
  if (code == PNUT_WIFI_ERROR_CODE_NONE)
    {
      memset(&reply, 0, sizeof(reply));
      if (wifi->serving)
        {
          pnut_wifi_join_reply(&wifi->server, &wifi->join_req,
                               PNUT_STATUS_OK, &reply);
        }
    }
  else
    {
      wifi_fail(wifi, &wifi->join_req, PNUT_STATUS_UNAVAILABLE, code,
                code == PNUT_WIFI_ERROR_CODE_CANCELLED ?
                "left before it joined" : "the join failed");
    }
}

/* Whether a join runs on the worker now */

static bool wifi_joining(FAR struct wifi_s *wifi)
{
  return wifi->working && wifi->work.job == WIFI_JOB_JOIN;
}

/****************************************************************************
 * Name: wifi_scan_push
 *
 * Description:
 *   The scan's networks to its stream, as its credit allows; then its end.
 *
 ****************************************************************************/

static void wifi_scan_push(FAR struct wifi_s *wifi)
{
  pnut_wifi_network_t item;
  FAR struct wifi_found_s *f;

  if (!wifi->scan.open || !wifi->scanned)
    {
      return;
    }

  while (wifi->pushed < wifi->nfound)
    {
      f = &wifi->found[wifi->pushed];
      memset(&item, 0, sizeof(item));
      strlcpy(item.name, f->name, sizeof(item.name));
      item.signal   = f->signal;
      item.channel  = f->channel;
      item.security = f->secured ? PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                      PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
      if (pnut_wifi_scan_push(&wifi->server, &wifi->scan, &item) < 0)
        {
          return;                 /* No credit: ready() goes on */
        }

      wifi->pushed++;
    }

  pnut_stream_end(&wifi->scan, PNUT_STATUS_ENDED);
  wifi->scanned = false;
}

/* Credit came: the rest; a stream over may let the radio down */

static void wifi_scan_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  wifi_scan_push(arg);
  wifi_next(arg);
}

static void wifi_scan_closed(FAR struct pnut_stream_s *stream, int status,
                             FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  /* The caller went: the scan runs on, its networks for nobody */

  wifi->scanned = false;
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_run
 *
 * Description:
 *   A job, on a worker: the driver's calls, which block.
 *
 ****************************************************************************/

static void wifi_run(FAR void *arg)
{
  FAR struct wifi_work_s *w = arg;
  FAR const struct wifi_ops_s *ops = w->ops;
  int n;

  w->ret    = OK;
  w->radio  = w->up;
  w->linked = false;
  switch (w->job)
    {
      case WIFI_JOB_UP:
        w->ret   = ops->up(w->arg);
        w->radio = w->ret >= 0;
        break;

      case WIFI_JOB_DOWN:
        ops->down(w->arg);
        w->radio = false;
        break;

      case WIFI_JOB_SCAN:
        if (!w->up && ops->up(w->arg) < 0)
          {
            w->ret = -ENODEV;     /* The radio would not come up */
            break;
          }

        n = ops->scan(w->arg, w->found, WIFI_SCAN_MAX);
        w->ret    = n < 0 ? n : OK;
        w->nfound = n < 0 ? 0 : n;
        w->radio  = true;
        if (!w->keep)
          {
            ops->down(w->arg);
            w->radio = false;
          }

        break;

      case WIFI_JOB_JOIN:

        /* From a radio brought up afresh: one left from a failed join
         * fails every join after it
         */

        ops->down(w->arg);
        w->radio = false;
        if (ops->up(w->arg) < 0)
          {
            w->ret = -ENODEV;
            break;
          }

        w->radio = true;
        w->ret   = ops->join(w->arg, w->name, w->passphrase, &w->signal);
        if (w->ret == OK)
          {
            w->ret = ops->address(w->arg, &w->addr);
            w->linked = w->ret == OK;
          }

        if (w->ret < 0)
          {
            /* Not left as a failed join leaves it */

            ops->down(w->arg);
            w->radio = false;
          }

        break;

      case WIFI_JOB_LINK:
        w->linked = ops->linked(w->arg, &w->signal);
        break;

      default:
        break;
    }

  explicit_bzero(w->passphrase, sizeof(w->passphrase));
}

/****************************************************************************
 * Name: wifi_link_timer, wifi_kick_timer
 ****************************************************************************/

static void wifi_link_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->link     = NULL;
  wifi->link_due = true;
  wifi_next(wifi);
}

static void wifi_kick_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->kick = NULL;
  wifi_next(wifi);
}

static void wifi_timer(FAR struct wifi_s *wifi,
                       FAR struct pnut_timer_s **timer, uint32_t ms,
                       pnut_timer_handler_t cb)
{
  int ret;

  if (*timer != NULL)
    {
      return;
    }

  ret = pnut_timer_start(wifi->module.loop, ms, 0, cb, wifi, timer);
  if (ret < 0)
    {
      pnut_log(&wifi->module, PNUT_LOG_WARNING, "No timer: %d", ret);
      *timer = NULL;
    }
}

static void wifi_cancel(FAR struct wifi_s *wifi,
                        FAR struct pnut_timer_s **timer)
{
  if (*timer != NULL)
    {
      pnut_timer_cancel(wifi->module.loop, *timer);
      *timer = NULL;
    }
}

/****************************************************************************
 * Name: wifi_save, wifi_save_step
 *
 * Description:
 *   Write the known networks to the slots.  The plan first: each slot
 *   left as it is, or given its network and the passphrase it takes, the
 *   network just joined's, or the one in the slot that holds its network
 *   now, read before any slot is written.  Then one setting at a time,
 *   each slot's passphrase, name and security.  One write at a time: a
 *   change meanwhile writes again after it.
 *
 ****************************************************************************/

static void wifi_save_step(FAR struct wifi_s *wifi);

static void wifi_save_end(FAR struct wifi_s *wifi)
{
  wifi->saving = false;
  explicit_bzero(wifi->save_psk, sizeof(wifi->save_psk));
  if (wifi->save_again)
    {
      wifi->save_again = false;
      wifi_save(wifi);
    }

  wifi_next(wifi);
}

static void wifi_save(FAR struct wifi_s *wifi)
{
  FAR struct wifi_known_s *k;
  FAR struct wifi_slot_s *slot;
  bool writes = false;
  bool pass;
  int i;
  int j;

  if (!wifi->known_loaded || !wifi->settings_open)
    {
      return;
    }

  if (wifi->saving)
    {
      wifi->save_again = true;
      return;
    }

  for (i = 0; i < WIFI_KNOWN_MAX; i++)
    {
      k    = &wifi->known[i];
      slot = &wifi->slots[i];
      memset(&wifi->save_to[i], 0, sizeof(wifi->save_to[i]));
      strlcpy(wifi->save_to[i].name, k->name, sizeof(wifi->save_to[i].name));
      wifi->save_to[i].secured = k->secured;

      pass = k->name[0] != '\0' && k->pending;
      if (!pass && !slot->unsure && slot->secured == k->secured &&
          strcmp(slot->name, k->name) == 0)
        {
          wifi->save_from[i] = WIFI_FROM_KEEP;
          continue;
        }

      writes = true;
      wifi->save_from[i] = WIFI_FROM_NONE;
      if (k->name[0] == '\0' || !k->secured)
        {
          k->pending = false;
          explicit_bzero(k->pass, sizeof(k->pass));
          continue;
        }

      if (pass)
        {
          wifi->save_from[i] = WIFI_FROM_PASS;
          strlcpy(wifi->save_psk[i], k->pass, sizeof(wifi->save_psk[i]));
          k->pending = false;
          explicit_bzero(k->pass, sizeof(k->pass));
          continue;
        }

      for (j = 0; j < WIFI_KNOWN_MAX; j++)
        {
          if (!wifi->slots[j].unsure && wifi->slots[j].secured &&
              strcmp(wifi->slots[j].name, k->name) == 0)
            {
              wifi->save_from[i] = j;
              break;
            }
        }

      if (wifi->save_from[i] == WIFI_FROM_NONE)
        {
          pnut_log(&wifi->module, PNUT_LOG_WARNING,
                   "\"%s\": its passphrase is lost: join it again",
                   k->name);
          k->refused = true;
        }
    }

  if (!writes)
    {
      explicit_bzero(wifi->save_psk, sizeof(wifi->save_psk));
      return;
    }

  wifi->saving       = true;
  wifi->save_reading = true;
  wifi->save_at      = 0;
  wifi->save_step    = 0;
  wifi->save_fails   = 0;
  wifi_save_step(wifi);
}

static void wifi_save_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->save_timer = NULL;
  wifi_save_step(wifi);
}

/* A call failed: the same again, a second later, then less and less
 * often; never given up while Wi-Fi runs, since a slot left half written
 * would lose its passphrase.  Unless Settings refused the call itself,
 * which no retry mends: the write ends, the slot left unsure.  ret is
 * Settings' status, or a call's negated errno value.
 */

static void wifi_save_failed(FAR struct wifi_s *wifi, int ret,
                             bool refused)
{
  uint32_t ms = WIFI_SAVE_MS;
  uint8_t i;

  if (refused)
    {
      pnut_log(&wifi->module, PNUT_LOG_ERROR,
               "Settings refused a known network's slot %d: status %d",
               wifi->save_at, ret);
      wifi_save_end(wifi);
      return;
    }

  if (wifi->save_fails < UINT8_MAX)
    {
      wifi->save_fails++;
    }

  for (i = 1; i < wifi->save_fails && ms < WIFI_SAVE_MAX_MS; i++)
    {
      ms <<= 1;
    }

  if (wifi->save_fails == 1)
    {
      pnut_log(&wifi->module, PNUT_LOG_WARNING,
               "Writing the known networks: %d; again", ret);
    }

  wifi_timer(wifi, &wifi->save_timer,
             ms < WIFI_SAVE_MAX_MS ? ms : WIFI_SAVE_MAX_MS,
             wifi_save_timer);
}

/* Settings' answers that no retry mends: the call itself refused */

static bool wifi_save_final(int status)
{
  return status == PNUT_STATUS_DENIED || status == PNUT_STATUS_NOTFOUND ||
         status == PNUT_STATUS_INVALID;
}

static void wifi_save_got(FAR struct pnut_settings_cache_s *cache,
                          int status, uint32_t code, FAR const char *value,
                          FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  if (status != PNUT_STATUS_OK)
    {
      wifi_save_failed(wifi, status, wifi_save_final(status));
      return;
    }

  strlcpy(wifi->save_psk[wifi->save_at], value,
          sizeof(wifi->save_psk[0]));
  wifi->save_at++;
  wifi->save_fails = 0;
  wifi_save_step(wifi);
}

static void wifi_save_done(FAR struct pnut_settings_cache_s *cache,
                           int status, uint32_t code, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;
  int i = wifi->save_at;

  if (status != PNUT_STATUS_OK)
    {
      wifi_save_failed(wifi, status, wifi_save_final(status));
      return;
    }

  wifi->save_fails = 0;
  if (++wifi->save_step == 3)
    {
      wifi->slots[i] = wifi->save_to[i];
      explicit_bzero(wifi->save_psk[i], sizeof(wifi->save_psk[i]));
      wifi->save_at++;
      wifi->save_step = 0;
    }

  wifi_save_step(wifi);
}

static void wifi_save_step(FAR struct wifi_s *wifi)
{
  char text[WIFI_ESCAPED_MAX];
  pnut_setting_value_t v;
  int ret;
  int i;

  if (!wifi->saving)
    {
      return;
    }

  if (wifi->save_reading)
    {
      for (i = wifi->save_at; i < WIFI_KNOWN_MAX; i++)
        {
          if (wifi->save_from[i] >= 0)
            {
              wifi->save_at = i;
              ret = pnut_settings_cache_get_secret(
                      &wifi->settings.cache, WIFI_PSK(wifi->save_from[i]),
                      wifi_save_got, wifi);
              if (ret < 0)
                {
                  wifi_save_failed(wifi, ret, false);
                }

              return;
            }
        }

      wifi->save_reading = false;
      wifi->save_at      = 0;
      wifi->save_step    = 0;
    }

  for (i = wifi->save_at; i < WIFI_KNOWN_MAX; i++)
    {
      if (wifi->save_from[i] != WIFI_FROM_KEEP)
        {
          break;
        }
    }

  if (i == WIFI_KNOWN_MAX)
    {
      wifi_save_end(wifi);
      return;
    }

  if (i != wifi->save_at)
    {
      wifi->save_at   = i;
      wifi->save_step = 0;
    }

  memset(&v, 0, sizeof(v));
  v.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  if (wifi->save_step == 0)
    {
      wifi->slots[i].unsure = true;
      ret = pnut_settings_cache_set_secret(&wifi->settings.cache,
                                           WIFI_PSK(i), wifi->save_psk[i],
                                           wifi_save_done, wifi);
    }
  else if (wifi->save_step == 1)
    {
      wifi_escape(wifi->save_to[i].name, text);
      strlcpy(v.value.text, text, sizeof(v.value.text));
      ret = pnut_settings_cache_set(&wifi->settings.cache, WIFI_NET(i), &v,
                                    wifi_save_done, wifi);
    }
  else
    {
      strlcpy(v.value.text, wifi->save_to[i].secured ? "secured" : "open",
              sizeof(v.value.text));
      ret = pnut_settings_cache_set(&wifi->settings.cache, WIFI_SEC(i), &v,
                                    wifi_save_done, wifi);
    }

  if (ret < 0)
    {
      wifi_save_failed(wifi, ret, false);
    }
}

/****************************************************************************
 * Name: wifi_load
 *
 * Description:
 *   The known networks, from the slots, once the settings are read: a
 *   free slot, or one holding a network an earlier slot holds, skipped,
 *   and the slots written again without them.
 *
 ****************************************************************************/

static void wifi_load(FAR struct wifi_s *wifi)
{
  FAR struct wifi_slot_s *slot;
  int n = 0;
  int i;

  memset(wifi->known, 0, sizeof(wifi->known));
  for (i = 0; i < WIFI_KNOWN_MAX; i++)
    {
      slot = &wifi->slots[i];
      memset(slot, 0, sizeof(*slot));
      slot->secured = strcmp(wifi_text(wifi, WIFI_SEC(i)), "secured") == 0;
      if (!wifi_unescape(wifi_text(wifi, WIFI_NET(i)), slot->name))
        {
          pnut_log(&wifi->module, PNUT_LOG_WARNING,
                   "Known network %d: no network's name: forgotten", i);
          slot->name[0] = '\0';
          slot->unsure  = true;
        }

      if (slot->name[0] == '\0' ||
          wifi_known_find(wifi, slot->name, slot->secured) >= 0)
        {
          continue;
        }

      strlcpy(wifi->known[n].name, slot->name, sizeof(wifi->known[n].name));
      wifi->known[n].secured = slot->secured;
      n++;
    }

  wifi->known_loaded = true;
  pnut_log(&wifi->module, PNUT_LOG_INFO, "%d known networks", n);
  wifi_save(wifi);
}

/****************************************************************************
 * Name: wifi_seek_ok, wifi_pick, wifi_idle_timer
 *
 * Description:
 *   Whether to look for a known network now: Wi-Fi on, the radio up, no
 *   network joined or being joined, none the owner asked for, and not
 *   left by the owner.  After a scan, the one to join: the network joined
 *   last if it is in reach, else the strongest; none refused, nor waiting
 *   after failures.  None: a look again later, when the first waiting may
 *   be tried, or after a while.
 *
 ****************************************************************************/

static bool wifi_seek_ok(FAR struct wifi_s *wifi)
{
  return wifi->enabled && wifi->radio && !wifi->radio_failed &&
         wifi->known_loaded && !wifi->held &&
         wifi->known[0].name[0] != '\0' &&
         wifi->state.state == PNUT_WIFI_INFO_STATE_ON &&
         !wifi->joining_req && !wifi->join_wanted &&
         !wifi->fetch_wanted && !wifi->fetching && !wifi->leave_wanted &&
         !wifi->scan.open && !wifi->scan_wanted;
}

static void wifi_idle_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->idle        = NULL;
  wifi->seek_wanted = true;
  wifi_next(wifi);
}

static void wifi_join_known(FAR struct wifi_s *wifi, int k)
{
  strlcpy(wifi->name, wifi->known[k].name, sizeof(wifi->name));
  wifi->secured  = wifi->known[k].secured;
  wifi->new_pass = false;
  explicit_bzero(wifi->passphrase, sizeof(wifi->passphrase));
  if (wifi->secured)
    {
      wifi->fetch_wanted = true;
    }
  else
    {
      wifi->join_wanted = true;
    }
}

static void wifi_pick(FAR struct wifi_s *wifi)
{
  uint64_t now = wifi_now();
  uint64_t wake = UINT64_MAX;
  int16_t signal = INT16_MIN;
  uint64_t delay;
  int best = -1;
  int i;
  int k;

  if (!wifi_seek_ok(wifi))
    {
      return;
    }

  for (i = 0; i < wifi->nfound; i++)
    {
      k = wifi_known_find(wifi, wifi->found[i].name, wifi->found[i].secured);
      if (k < 0 || wifi->known[k].refused)
        {
          continue;
        }

      if (wifi->known[k].not_before > now)
        {
          wake = wifi->known[k].not_before < wake ?
                 wifi->known[k].not_before : wake;
          continue;
        }

      if (best < 0 || k == 0 ||
          (best != 0 && wifi->found[i].signal > signal))
        {
          best   = k;
          signal = wifi->found[i].signal;
        }
    }

  if (best >= 0)
    {
      pnut_log(&wifi->module, PNUT_LOG_INFO, "Joining \"%s\", known",
               wifi->known[best].name);
      wifi_join_known(wifi, best);
      return;
    }

  delay = wifi_ms(wifi->config.idle_ms, WIFI_IDLE_MS);
  if (wake != UINT64_MAX && wake - now < delay)
    {
      delay = wake - now;
    }

  wifi_timer(wifi, &wifi->idle, (uint32_t)delay, wifi_idle_timer);
}

/* After a failure, the wait before a known network is tried on its own
 * again: doubled each time, up to 8 minutes
 */

static void wifi_backoff(FAR struct wifi_s *wifi, int k)
{
  uint64_t ms = wifi_ms(wifi->config.again_ms, WIFI_AGAIN_MS);
  uint8_t i;

  if (wifi->known[k].fails < UINT8_MAX)
    {
      wifi->known[k].fails++;
    }

  for (i = 1; i < wifi->known[k].fails && ms < WIFI_AGAIN_MAX_MS; i++)
    {
      ms <<= 1;
    }

  wifi->known[k].not_before = wifi_now() +
                              (ms < WIFI_AGAIN_MAX_MS ? ms :
                               WIFI_AGAIN_MAX_MS);
}

/****************************************************************************
 * Name: wifi_fetch, wifi_fetched
 *
 * Description:
 *   A known secured network's passphrase, read from Settings for its
 *   join: from the slot that holds it.
 *
 ****************************************************************************/

/* Its passphrase not had: none in Settings (lost), the network refused
 * until the owner gives it again; or Settings did not answer now, the
 * network tried later, as after a failed join
 */

static void wifi_no_pass(FAR struct wifi_s *wifi, int ret, bool lost)
{
  int k = wifi_known_find(wifi, wifi->name, wifi->secured);

  pnut_log(&wifi->module, PNUT_LOG_WARNING,
           "\"%s\": its passphrase %s: %d", wifi->name,
           lost ? "is lost: join it again" : "cannot be read now", ret);
  if (k >= 0 && lost)
    {
      wifi->known[k].refused = true;
    }
  else if (k >= 0 && !wifi->joining_req)
    {
      wifi_backoff(wifi, k);
    }

  if (wifi->joining_req && lost)
    {
      wifi_join_answer(wifi, PNUT_WIFI_ERROR_CODE_REFUSED);
    }
  else if (wifi->joining_req)
    {
      wifi->joining_req = false;
      wifi_fail(wifi, &wifi->join_req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_NONE,
                "its passphrase cannot be read now; try again");
    }

  wifi->seek_wanted = true;
  wifi_forget_pass(wifi);
}

static void wifi_fetched(FAR struct pnut_settings_cache_s *cache,
                         int status, uint32_t code, FAR const char *value,
                         FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->fetching = false;
  if (wifi->fetch_stale)
    {
      wifi->fetch_stale = false;
    }
  else if (status != PNUT_STATUS_OK || value[0] == '\0')
    {
      wifi_no_pass(wifi, status != PNUT_STATUS_OK ? -status : -ENOENT,
                   status == PNUT_STATUS_OK);
    }
  else
    {
      strlcpy(wifi->passphrase, value, sizeof(wifi->passphrase));
      wifi->join_wanted = true;
    }

  wifi_next(wifi);
}

static void wifi_fetch(FAR struct wifi_s *wifi)
{
  int ret = -ENOENT;
  int i;

  for (i = 0; i < WIFI_KNOWN_MAX; i++)
    {
      if (!wifi->slots[i].unsure && wifi->slots[i].secured &&
          strcmp(wifi->slots[i].name, wifi->name) == 0)
        {
          ret = pnut_settings_cache_get_secret(&wifi->settings.cache,
                                               WIFI_PSK(i), wifi_fetched,
                                               wifi);
          break;
        }
    }

  if (ret == -EBUSY || ret == -ENOTCONN)
    {
      wifi_timer(wifi, &wifi->kick, WIFI_KICK_MS, wifi_kick_timer);
      return;
    }

  wifi->fetch_wanted = false;
  if (ret < 0)
    {
      wifi_no_pass(wifi, ret, ret == -ENOENT);
      return;
    }

  wifi->fetching    = true;
  wifi->fetch_stale = false;
}

/****************************************************************************
 * Name: wifi_joined
 *
 * Description:
 *   A network joined becomes known, first: its passphrase written to
 *   Settings if it is new, or the owner gave another, and wiped here.
 *
 ****************************************************************************/

static void wifi_joined(FAR struct wifi_s *wifi)
{
  FAR struct wifi_known_s *k;
  bool keep;

  keep = wifi->new_pass ||
         wifi_known_find(wifi, wifi->name, wifi->secured) < 0;
  k = wifi_known_front(wifi, wifi->name, wifi->secured);
  k->refused    = false;
  k->fails      = 0;
  k->not_before = 0;
  if (keep && wifi->secured)
    {
      k->pending = true;
      strlcpy(k->pass, wifi->passphrase, sizeof(k->pass));
    }

  explicit_bzero(wifi->passphrase, sizeof(wifi->passphrase));
  wifi->new_pass = false;
  wifi_save(wifi);
}

/****************************************************************************
 * Name: wifi_done
 *
 * Description:
 *   A job's end, on the loop: the state follows, the request waiting is
 *   answered, the scan's networks go out.
 *
 ****************************************************************************/

static void wifi_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct wifi_s *wifi =
    (FAR struct wifi_s *)((FAR char *)arg - offsetof(struct wifi_s, work));
  FAR struct wifi_work_s *w = &wifi->work;
  uint8_t job = w->job;
  uint32_t code;
  bool cancelled;
  int i;

  /* What follows may start nothing: wifi_next() at the end does */

  wifi->working = false;
  wifi->in_done = true;
  wifi->radio   = w->radio;
  w->job        = WIFI_JOB_NONE;
  if (w->radio)
    {
      wifi->radio_failed = false;
    }

  switch (job)
    {
      case WIFI_JOB_UP:
        if (w->ret < 0)
          {
            /* Not tried again on its own: the owner may turn Wi-Fi off
             * and on, or ask for a scan or a join
             */

            pnut_log(&wifi->module, PNUT_LOG_ERROR,
                     "The radio would not come up: %d", w->ret);
            wifi->radio_failed  = true;
            wifi->state.failure = PNUT_WIFI_ERROR_CODE_NO_RADIO;
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
          }
        else
          {
            wifi->seek_wanted = true;
          }

        break;

      case WIFI_JOB_DOWN:
        wifi_set_state(wifi, wifi->enabled ? PNUT_WIFI_INFO_STATE_ON :
                       PNUT_WIFI_INFO_STATE_OFF);
        break;

      case WIFI_JOB_SCAN:
        wifi->nfound = w->nfound;
        for (i = 0; i < w->nfound; i++)
          {
            wifi->found[i] = w->found[i];
          }

        wifi->pushed  = 0;
        wifi->scanned = wifi->scan.open;
        if (w->ret < 0)
          {
            pnut_log(&wifi->module, PNUT_LOG_WARNING, "A scan failed: %d",
                     w->ret);
            if (w->ret == -ENODEV && wifi->enabled)
              {
                wifi->radio_failed  = true;
                wifi->state.failure = PNUT_WIFI_ERROR_CODE_NO_RADIO;
                wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
              }

            if (wifi->scan.open)
              {
                pnut_stream_end(&wifi->scan, PNUT_STATUS_UNAVAILABLE);
              }

            wifi->scanned = false;
          }

        wifi_scan_push(wifi);
        if (wifi->seeking)
          {
            wifi->seeking = false;
            wifi_pick(wifi);
          }

        break;

      case WIFI_JOB_JOIN:

        /* Wi-Fi turned off, or left, while it ran: the request is told,
         * and the radio goes down next
         */

        cancelled = !wifi->enabled || wifi->leave_wanted;
        if (cancelled)
          {
            if (w->ret == OK)
              {
                pnut_log(&wifi->module, PNUT_LOG_INFO,
                         "Joined \"%s\", left at once", w->name);
              }

            wifi_set_state(wifi, wifi->enabled ? PNUT_WIFI_INFO_STATE_ON :
                           PNUT_WIFI_INFO_STATE_OFF);
            wifi_join_answer(wifi, wifi->enabled ?
                             PNUT_WIFI_ERROR_CODE_CANCELLED :
                             PNUT_WIFI_ERROR_CODE_OFF);
            wifi_forget_pass(wifi);
          }
        else if (w->ret == OK)
          {
            wifi->state.signal  = w->signal;
            wifi->state.address = w->addr;
            wifi->state.failure = PNUT_WIFI_ERROR_CODE_NONE;
            strlcpy(wifi->state.name, w->name, sizeof(wifi->state.name));
            pnut_log(&wifi->module, PNUT_LOG_INFO, "Joined \"%s\", %d dBm",
                     w->name, w->signal);
            wifi_joined(wifi);
            wifi->link_due = false;
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_JOINED);
            wifi_timer(wifi, &wifi->link,
                       wifi_ms(wifi->config.link_ms, WIFI_LINK_MS),
                       wifi_link_timer);
            wifi_join_answer(wifi, PNUT_WIFI_ERROR_CODE_NONE);
          }
        else
          {
            code = w->ret == -EADDRNOTAVAIL || w->ret == -EAGAIN ?
                   PNUT_WIFI_ERROR_CODE_NO_ADDRESS :
                   w->ret == -ENOENT ? PNUT_WIFI_ERROR_CODE_NOT_FOUND :
                   w->ret == -ENODEV || w->ret == -EIO ?
                   PNUT_WIFI_ERROR_CODE_NO_RADIO :
                   PNUT_WIFI_ERROR_CODE_REFUSED;
            pnut_log(&wifi->module, PNUT_LOG_WARNING,
                     "Joining \"%s\" failed: %d", w->name, w->ret);
            wifi->state.failure = code;
            wifi->radio_failed  = code == PNUT_WIFI_ERROR_CODE_NO_RADIO;
            wifi_set_state(wifi, wifi->radio_failed ?
                           PNUT_WIFI_INFO_STATE_OFF :
                           PNUT_WIFI_INFO_STATE_ON);

            /* A network asked for: the owner is told, and it is not
             * tried on its own until the owner joins it again.  One
             * joined on its own: later, others in reach meanwhile.
             */

            i = wifi_known_find(wifi, wifi->name, wifi->secured);
            if (wifi->joining_req)
              {
                if (i >= 0)
                  {
                    wifi->known[i].refused = true;
                  }

                wifi_join_answer(wifi, code);
              }
            else if (i >= 0)
              {
                wifi_backoff(wifi, i);
              }

            wifi_forget_pass(wifi);
            wifi->seek_wanted = true;
          }

        break;

      case WIFI_JOB_LINK:
        if (wifi->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            break;
          }

        if (w->linked)
          {
            if (w->signal != wifi->state.signal)
              {
                wifi->state.signal = w->signal;
                wifi_publish(wifi);
              }

            wifi_timer(wifi, &wifi->link,
                       wifi_ms(wifi->config.link_ms, WIFI_LINK_MS),
                       wifi_link_timer);
          }
        else
          {
            pnut_log(&wifi->module, PNUT_LOG_INFO, "\"%s\" lost",
                     wifi->state.name);
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
            wifi->seek_wanted = true;
          }

        break;

      default:
        break;
    }

  /* On, the radio up (by any job): no longer off for want of it */

  if (w->radio && wifi->enabled &&
      wifi->state.state == PNUT_WIFI_INFO_STATE_OFF)
    {
      if (wifi->state.failure == PNUT_WIFI_ERROR_CODE_NO_RADIO)
        {
          wifi->state.failure = PNUT_WIFI_ERROR_CODE_NONE;
        }

      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
    }

  wifi->in_done = false;
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_submit
 ****************************************************************************/

static int wifi_submit(FAR struct wifi_s *wifi, uint8_t job)
{
  FAR struct wifi_work_s *w = &wifi->work;
  int ret;

  w->ops  = wifi->config.ops;
  w->arg  = wifi->config.arg;
  w->job  = job;
  w->up   = wifi->radio;
  w->keep = wifi->enabled;
  if (job == WIFI_JOB_JOIN)
    {
      strlcpy(w->name, wifi->name, sizeof(w->name));
      strlcpy(w->passphrase, wifi->passphrase, sizeof(w->passphrase));
    }

  ret = pnut_job_submit(wifi->module.loop, wifi_run, wifi_done, w);
  if (ret < 0)
    {
      /* Never on the loop: the driver's calls block for seconds */

      explicit_bzero(w->passphrase, sizeof(w->passphrase));
      w->job = WIFI_JOB_NONE;
      return ret;
    }

  wifi->working = true;
  return OK;
}

/****************************************************************************
 * Name: wifi_next
 *
 * Description:
 *   The next job, once none runs: off first, then leaving, a join, a
 *   scan, the radio up, a look at the link.
 *
 ****************************************************************************/

static void wifi_next(FAR struct wifi_s *wifi)
{
  FAR bool *wanted = NULL;
  uint8_t job;
  int ret;

  if (wifi->in_done || wifi->config.ops == NULL)
    {
      return;
    }

  /* A known network's passphrase, read from Settings, not on the worker;
   * not while the slots are written, which may move it
   */

  if (wifi->fetch_wanted && !wifi->fetching && !wifi->saving &&
      wifi->enabled)
    {
      wifi_fetch(wifi);
    }

  if (wifi->working)
    {
      return;
    }

  if (!wifi->enabled && wifi->radio && !wifi->scan.open)
    {
      job    = WIFI_JOB_DOWN;
      wanted = &wifi->leave_wanted;   /* Left, too, once down */
    }
  else if (wifi->leave_wanted)
    {
      job    = WIFI_JOB_DOWN;
      wanted = &wifi->leave_wanted;
    }
  else if (wifi->join_wanted && wifi->enabled)
    {
      job    = WIFI_JOB_JOIN;
      wanted = &wifi->join_wanted;
    }
  else if (wifi->scan_wanted)
    {
      job    = WIFI_JOB_SCAN;
      wanted = &wifi->scan_wanted;
    }
  else if (wifi->enabled && !wifi->radio && !wifi->radio_failed)
    {
      job = WIFI_JOB_UP;
    }
  else if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED &&
           wifi->link_due)
    {
      job    = WIFI_JOB_LINK;
      wanted = &wifi->link_due;
    }
  else if (wifi->seek_wanted && wifi_seek_ok(wifi) && wifi->idle == NULL)
    {
      job    = WIFI_JOB_SCAN;
      wanted = &wifi->seek_wanted;
    }
  else
    {
      /* A look kept for after the owner's scan, which wifi_next() follows
       * as it ends
       */

      if (!wifi->scan.open && !wifi->scan_wanted)
        {
          wifi->seek_wanted = false;
        }

      return;
    }

  ret = wifi_submit(wifi, job);
  if (ret < 0)
    {
      pnut_log(&wifi->module, PNUT_LOG_WARNING,
               "The worker took no job: %d; again in a second", ret);
      wifi_timer(wifi, &wifi->kick, WIFI_KICK_MS, wifi_kick_timer);
      return;
    }

  if (wanted == &wifi->seek_wanted)
    {
      wifi->seeking = true;
    }

  if (wanted != NULL)
    {
      *wanted = false;
    }

  if (job == WIFI_JOB_JOIN)
    {
      strlcpy(wifi->state.name, wifi->name, sizeof(wifi->state.name));
      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_JOINING);
    }
}

/****************************************************************************
 * Name: wifi_do_scan, wifi_do_join, wifi_do_leave
 *
 * Description:
 *   A request from the system UI or the shell, once Service states said
 *   it is.
 *
 ****************************************************************************/

static void wifi_do_scan(FAR struct wifi_s *wifi,
                         FAR const struct pnut_request_s *req)
{
  int ret;

  if (wifi->scan.open)
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "a scan runs");
      return;
    }

  ret = pnut_wifi_scan_accept(&wifi->server, req, &wifi->scan,
                              wifi_scan_ready, wifi_scan_closed, wifi);
  if (ret < 0)
    {
      return;
    }

  wifi->scanned     = false;
  wifi->scan_wanted = true;
  wifi_next(wifi);
}

static void wifi_do_join(FAR struct wifi_s *wifi,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_join_request_t *in)
{
  size_t len = strnlen(in->passphrase, sizeof(in->passphrase));
  int k;

  if (!wifi->enabled)
    {
      wifi_fail(wifi, req, PNUT_STATUS_UNAVAILABLE, PNUT_WIFI_ERROR_CODE_OFF,
                "Wi-Fi is off");
      return;
    }

  if (in->name[0] == '\0' || (len > 0 && (len < 8 || len > 63)))
    {
      wifi_fail(wifi, req, PNUT_STATUS_INVALID, PNUT_WIFI_ERROR_CODE_NONE,
                "a name, and a passphrase of 8 to 63 characters or none");
      return;
    }

  /* One join at a time, one Wi-Fi started on its own too: its end would
   * answer this request.  One it was about to start gives way.
   */

  if (wifi->joining_req || wifi_joining(wifi))
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "a join runs");
      return;
    }

  wifi_stop_seeking(wifi);

  /* No passphrase: a known secured network's own, read from Settings,
   * or else an open network
   */

  strlcpy(wifi->name, in->name, sizeof(wifi->name));
  strlcpy(wifi->passphrase, in->passphrase, sizeof(wifi->passphrase));
  wifi->new_pass = len > 0;
  wifi->secured  = len > 0 ||
                   wifi_known_find(wifi, in->name, true) >= 0;

  k = wifi_known_find(wifi, wifi->name, wifi->secured);
  if (k >= 0)
    {
      wifi->known[k].refused = false;
    }

  wifi->join_req     = *req;
  wifi->joining_req  = true;
  wifi->held         = false;
  wifi->leave_wanted = false;
  if (wifi->secured && len == 0)
    {
      wifi->fetch_wanted = true;
    }
  else
    {
      wifi->join_wanted = true;
    }

  wifi_next(wifi);
}

/* No look for a known network, nor a join of one about to start: a
 * passphrase being read is not used
 */

static void wifi_stop_seeking(FAR struct wifi_s *wifi)
{
  wifi->seek_wanted  = false;
  wifi->fetch_wanted = false;
  wifi->fetch_stale  = wifi->fetching;
  if (!wifi_joining(wifi))
    {
      wifi->join_wanted = false;
    }

  wifi_cancel(wifi, &wifi->idle);
}

/* A join waiting, not running, dropped: its request told; a running
 * one's is told as it ends
 */

static void wifi_drop_join(FAR struct wifi_s *wifi, uint32_t code)
{
  wifi_stop_seeking(wifi);
  wifi->join_wanted = false;
  explicit_bzero(wifi->passphrase, sizeof(wifi->passphrase));
  if (!wifi_joining(wifi))
    {
      wifi_forget_pass(wifi);
      wifi_join_answer(wifi, code);
    }
}

/* The network joined, or being joined, left: the radio down */

static void wifi_leave_now(FAR struct wifi_s *wifi)
{
  wifi_drop_join(wifi, PNUT_WIFI_ERROR_CODE_CANCELLED);
  wifi->leave_wanted = wifi->radio || wifi_joining(wifi);
  if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED)
    {
      wifi_cancel(wifi, &wifi->link);
      wifi->link_due = false;
    }
}

static void wifi_do_leave(FAR struct wifi_s *wifi,
                          FAR const struct pnut_request_s *req)
{
  pnut_wifi_reply_t reply;

  wifi_leave_now(wifi);
  wifi->held = true;              /* Until a join, or off and on */

  memset(&reply, 0, sizeof(reply));
  pnut_wifi_leave_reply(&wifi->server, req, PNUT_STATUS_OK, &reply);
  wifi_next(wifi);
}

/* Known: the known networks, last used first, without passphrases */

static void wifi_do_known(FAR struct wifi_s *wifi,
                          FAR const struct pnut_request_s *req)
{
  FAR pnut_wifi_known_networks_t *out = &wifi->known_out;
  FAR pnut_wifi_known_network_t *n;
  int i;

  if (!wifi->known_loaded)
    {
      wifi_fail(wifi, req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_NONE, "not read from Settings yet");
      return;
    }

  memset(out, 0, sizeof(*out));
  for (i = 0; i < WIFI_KNOWN_MAX && wifi->known[i].name[0] != '\0'; i++)
    {
      n = &out->networks[out->networks_count++];
      strlcpy(n->name, wifi->known[i].name, sizeof(n->name));
      n->security = wifi->known[i].secured ?
                    PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                    PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
      n->refused  = wifi->known[i].refused;
    }

  pnut_wifi_known_reply(&wifi->server, req, PNUT_STATUS_OK, out);
}

/* Forget: a known network and its passphrase; left, if it is the one
 * joined or being joined, and others looked for
 */

static void wifi_do_forget(FAR struct wifi_s *wifi,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_wifi_forget_request_t *in)
{
  bool secured = in->security == PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED;
  pnut_wifi_reply_t reply;
  int k = wifi_known_find(wifi, in->name, secured);

  if (!wifi->known_loaded || k < 0)
    {
      wifi_fail(wifi, req, PNUT_STATUS_NOTFOUND,
                PNUT_WIFI_ERROR_CODE_NOT_KNOWN, "no such known network");
      return;
    }

  pnut_log(&wifi->module, PNUT_LOG_INFO, "\"%s\" forgotten",
           wifi->known[k].name);
  wifi_known_remove(wifi, k);
  wifi_save(wifi);

  /* Joined, being joined, or about to be: its passphrase being read,
   * or its join waiting for the worker.  Left, or dropped, so that it is
   * not joined, and known, again; others are looked for.
   */

  if (wifi->secured == secured && strcmp(wifi->name, in->name) == 0)
    {
      if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED ||
          wifi->state.state == PNUT_WIFI_INFO_STATE_JOINING ||
          wifi_joining(wifi))
        {
          wifi_leave_now(wifi);
        }
      else
        {
          wifi_drop_join(wifi, PNUT_WIFI_ERROR_CODE_CANCELLED);
          wifi->seek_wanted = true;
        }
    }

  memset(&reply, 0, sizeof(reply));
  pnut_wifi_forget_reply(&wifi->server, req, PNUT_STATUS_OK, &reply);
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_who, wifi_ask
 *
 * Description:
 *   Who may call (RFC 0031): the system UI, and a command typed in the
 *   developer's shell; Service states says who sent a request.
 *
 ****************************************************************************/

static void wifi_dispatch(FAR struct wifi_s *wifi,
                          FAR struct wifi_ask_s *ask)
{
  if (ask->method == PNUT_WIFI_SCAN)
    {
      wifi_do_scan(wifi, &ask->req);
    }
  else if (ask->method == PNUT_WIFI_JOIN)
    {
      wifi_do_join(wifi, &ask->req, &ask->join);
    }
  else if (ask->method == PNUT_WIFI_KNOWN)
    {
      wifi_do_known(wifi, &ask->req);
    }
  else if (ask->method == PNUT_WIFI_FORGET)
    {
      wifi_do_forget(wifi, &ask->req, &ask->forget);
    }
  else
    {
      wifi_do_leave(wifi, &ask->req);
    }

  explicit_bzero(&ask->join, sizeof(ask->join));
}

static void wifi_who(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_services_who_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifi_ask_s *ask = arg;
  FAR struct wifi_s *wifi = ask->wifi;

  ask->used = false;
  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_TIMEOUT)
    {
      wifi_fail(wifi, &ask->req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_DENIED, "who calls cannot be told now");
      explicit_bzero(&ask->join, sizeof(ask->join));
      return;
    }

  if (status != PNUT_STATUS_OK || out == NULL ||
      (strcmp(out->name, WIFI_UI) != 0 && !out->shell))
    {
      wifi_fail(wifi, &ask->req, PNUT_STATUS_DENIED,
                PNUT_WIFI_ERROR_CODE_DENIED,
                "for the system UI and the developer's shell");
      explicit_bzero(&ask->join, sizeof(ask->join));
      return;
    }

  wifi_dispatch(wifi, ask);
}

static void wifi_ask(FAR struct wifi_s *wifi, uint8_t method,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_wifi_join_request_t *join,
                     FAR const pnut_wifi_forget_request_t *forget)
{
  pnut_services_who_request_t who;
  FAR struct wifi_ask_s *ask = NULL;
  int ret;
  int i;

  for (i = 0; i < 2 && ask == NULL; i++)
    {
      if (!wifi->asks[i].used)
        {
          ask = &wifi->asks[i];
        }
    }

  if (ask == NULL)
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "too many requests wait");
      return;
    }

  memset(ask, 0, sizeof(*ask));
  ask->wifi   = wifi;
  ask->method = method;
  ask->req    = *req;
  if (join != NULL)
    {
      ask->join = *join;
    }

  if (forget != NULL)
    {
      ask->forget = *forget;
    }

  if (wifi->config.services == NULL)
    {
      wifi_dispatch(wifi, ask);
      return;
    }

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ask->used = true;
  ret = wifi->services_open ?
        pnut_services_who(&wifi->services, &who, 0, wifi_who, ask) :
        -ENOTCONN;
  if (ret < 0)
    {
      ask->used = false;
      explicit_bzero(&ask->join, sizeof(ask->join));
      wifi_fail(wifi, req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_DENIED, "who calls cannot be told now");
    }
}

static void wifi_on_scan(FAR struct pnut_wifi_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_scan_request_t *in,
                         FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_SCAN, req, NULL, NULL);
}

static void wifi_on_join(FAR struct pnut_wifi_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_join_request_t *in,
                         FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_JOIN, req, in, NULL);
}

static void wifi_on_leave(FAR struct pnut_wifi_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_wifi_leave_request_t *in,
                          FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_LEAVE, req, NULL, NULL);
}

static void wifi_on_known(FAR struct pnut_wifi_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_wifi_known_request_t *in,
                          FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_KNOWN, req, NULL, NULL);
}

static void wifi_on_forget(FAR struct pnut_wifi_server_s *server,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_wifi_forget_request_t *in,
                           FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_FORGET, req, NULL, in);
}

static const struct pnut_wifi_handlers_s g_wifi_handlers =
{
  .scan   = wifi_on_scan,
  .join   = wifi_on_join,
  .leave  = wifi_on_leave,
  .known  = wifi_on_known,
  .forget = wifi_on_forget,
};

/****************************************************************************
 * Name: wifi_settings
 *
 * Description:
 *   The settings were read, or one changed: the radio follows
 *   wifi.enabled.
 *
 ****************************************************************************/

static void wifi_settings(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;
  bool enabled = wifi->settings.values.enabled;

  /* The slots are Wi-Fi's: read once, as it starts, and changed only by
   * its own writes, whose changes come back here
   */

  if (index == PNUT_SETTINGS_CACHE_ALL && !wifi->known_loaded)
    {
      wifi_load(wifi);
      wifi->seek_wanted = true;
    }

  if ((index != PNUT_SETTINGS_CACHE_ALL &&
       index != PNUT_WIFI_SETTINGS_ENABLED) || enabled == wifi->enabled)
    {
      wifi_next(wifi);
      return;
    }

  wifi->enabled = enabled;
  pnut_log(&wifi->module, PNUT_LOG_INFO, "Wi-Fi %s",
           enabled ? "on" : "off");
  if (!enabled)
    {
      /* Off on the topic at once, for its readers: the radio goes down
       * after a job that holds it, a scan, has ended
       */

      wifi_drop_join(wifi, PNUT_WIFI_ERROR_CODE_OFF);
      wifi_cancel(wifi, &wifi->link);
      wifi->link_due = false;

      /* A network joined, or being joined, is left even if Wi-Fi is on
       * again before the radio goes down: its passphrase is gone
       */

      if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED ||
          wifi_joining(wifi))
        {
          wifi->leave_wanted = true;
        }

      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
    }
  else
    {
      wifi->radio_failed = false;   /* Off and on: tried again */
      wifi->held         = false;   /* And known networks looked for */

      /* On again before the radio went down */

      if (wifi->radio && wifi->state.state == PNUT_WIFI_INFO_STATE_OFF)
        {
          wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
        }
    }

  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_start, wifi_stop
 ****************************************************************************/

static int wifi_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_s *wifi = (FAR struct wifi_s *)module;
  int ret;

  ret = pnut_wifi_info_advertise(loop, &wifi->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No wifi topic: %d", ret);
      wifi->topic = NULL;
    }

  wifi_publish(wifi);

  ret = pnut_wifi_serve(loop, WIFI_SERVICE, 0, &g_wifi_handlers, wifi,
                        &wifi->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve %s: %d", WIFI_SERVICE,
               ret);
      return ret;
    }

  wifi->serving = true;
  if (wifi->config.services != NULL)
    {
      ret = pnut_services_connect(loop, wifi->config.services, NULL, NULL,
                                  &wifi->services);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No Service states: %d; callers refused", ret);
        }
      else
        {
          wifi->services_open = true;
        }
    }

  ret = pnut_wifi_settings_open(module, wifi_settings, wifi,
                                &wifi->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d; Wi-Fi off",
               ret);
    }
  else
    {
      wifi->settings_open = true;
    }

  pnut_module_ready(module);
  return OK;
}

static void wifi_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_s *wifi = (FAR struct wifi_s *)module;

  wifi_forget_pass(wifi);
  explicit_bzero(wifi->asks, sizeof(wifi->asks));
  explicit_bzero(wifi->known, sizeof(wifi->known));
  explicit_bzero(wifi->save_psk, sizeof(wifi->save_psk));
  wifi->known_loaded = false;
  wifi->saving       = false;
  wifi->fetching = false;
  wifi_cancel(wifi, &wifi->link);
  wifi_cancel(wifi, &wifi->kick);
  wifi_cancel(wifi, &wifi->idle);
  wifi_cancel(wifi, &wifi->save_timer);

  if (wifi->settings_open)
    {
      pnut_wifi_settings_close(&wifi->settings);
      wifi->settings_open = false;
    }

  if (wifi->services_open)
    {
      pnut_services_disconnect(&wifi->services);
      wifi->services_open = false;
    }

  if (wifi->serving)
    {
      pnut_wifi_close(&wifi->server);
      wifi->serving = false;
    }

  if (wifi->topic != NULL)
    {
      pnut_topic_unadvertise(wifi->topic);
      wifi->topic = NULL;
    }

  /* The radio down, on the loop now: the program ends.  With a job
   * running, the program does it once the worker is done (wifi_end()).
   */

  if (wifi->radio && !wifi->working && wifi->config.ops != NULL)
    {
      wifi->config.ops->down(wifi->config.arg);
      wifi->radio = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void wifi_init(FAR struct wifi_s *wifi,
               FAR const struct wifi_config_s *config)
{
  memset(wifi, 0, sizeof(*wifi));
  wifi->config       = *config;
  wifi->module.name  = "wifi";
  wifi->module.start = wifi_start;
  wifi->module.stop  = wifi_stop;
  wifi->state.state  = PNUT_WIFI_INFO_STATE_OFF;
}

void wifi_end(FAR struct wifi_s *wifi)
{
  /* The loop is gone, its worker joined: a job left running had its
   * radio up, or half up
   */

  if ((wifi->radio || wifi->working) && wifi->config.ops != NULL)
    {
      wifi->config.ops->down(wifi->config.arg);
      wifi->radio   = false;
      wifi->working = false;
    }

  explicit_bzero(&wifi->work, sizeof(wifi->work));
}
