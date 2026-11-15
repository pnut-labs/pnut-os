/****************************************************************************
 * pnut-os/src/ui/scr_wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Wi-Fi (Set-WiFi): the switch (wifi.enabled), the network joined, the
 * networks in reach from a scan.  A known network, or an open one, is
 * joined at once; a new secured one opens the join screen.  The options:
 * Scan again, Leave, Forget, where they apply.  Forget asks first.
 *
 * Wi-Fi's answers find this screen by its serial: one that comes after it
 * went changes nothing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <pnut/client.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "blocks.h"
#include "describe.h"
#include "glue.h"
#include "icons.h"
#include "keymap.h"
#include "list.h"
#include "setting.h"
#include "text.h"
#include "theme.h"
#include "ui.h"
#include "wifilist.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_ID_SWITCH     1
#define WIFI_ID_CONNECTED  2
#define WIFI_ID_NET        16     /* + the row's index */

#define WIFI_CREDIT        4      /* Networks given at a time */
#define WIFI_JOIN_MS       60000  /* Join takes up to about 35 s */

/* The options, as chosen */

#define WIFI_OPT_SCAN      0
#define WIFI_OPT_LEAVE     1
#define WIFI_OPT_FORGET    2

#define WIFI_DETAIL_MAX    64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wifi_scr_s
{
  struct list_s list;
  struct setting_watch_s watch;
  struct setting_s enabled;       /* wifi.enabled */
  bool watch_open;

  FAR lv_obj_t *sw;
  FAR lv_obj_t *message;          /* Why something failed */
  FAR lv_obj_t *conn_head;
  FAR lv_obj_t *conn;
  FAR lv_obj_t *near_head;
  FAR lv_obj_t *rows[WIFILIST_MAX];
  uint8_t nrows;                  /* Rows made so far */

  /* What each row shows, as drawn: the list moves on between redraws
   * while the scan goes on, a row acts on what it shows
   */

  pnut_wifi_network_t drawn[WIFILIST_MAX];
  bool drawn_known[WIFILIST_MAX];

  /* The scan */

  struct wifilist_s nets;
  uint16_t stream;
  uint8_t got;                    /* Networks since credit was given */
  bool scanning;
  bool scan_failed;
  bool scanned;                   /* A scan ended */
  time_t scanned_at;
  FAR struct pnut_timer_s *redraw;

  /* The known networks */

  pnut_wifi_known_networks_t known;
  bool has_known;

  /* Wi-Fi's state as last seen: a change reads the known ones again */

  uint8_t seen_state;
  char seen_name[sizeof(((pnut_wifi_info_t *)0)->name)];

  /* The network focused, for the focus to follow it */

  char focus_name[sizeof(((pnut_wifi_network_t *)0)->name)];

  /* The options shown, and what Forget would forget */

  uint8_t opts[3];
  char forget_name[sizeof(((pnut_wifi_network_t *)0)->name)];
  pnut_wifi_security_t forget_security;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wifi_show(FAR struct screen_s *s);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct ui_s *wifi_ui(FAR struct pnut_wifi_client_s *client)
{
  return (FAR struct ui_s *)((FAR char *)client -
                             offsetof(struct ui_s, wifi));
}

static FAR void *wifi_token(FAR struct screen_s *s)
{
  return (FAR void *)(uintptr_t)s->serial;
}

/* The screen an answer is for, if it is still there */

static FAR struct screen_s *wifi_screen(FAR struct pnut_wifi_client_s *c,
                                        FAR void *arg)
{
  return ui_find(wifi_ui(c), (uint32_t)(uintptr_t)arg);
}

static bool wifi_on(FAR struct wifi_scr_s *st)
{
  return st->enabled.known && st->enabled.value.value.boolean;
}

static void wifi_message(FAR struct screen_s *s, FAR const char *text)
{
  FAR struct wifi_scr_s *st = s->state;

  block_set_text(st->message, text != NULL ? text : "");
  if (text != NULL && text[0] != '\0')
    {
      lv_obj_remove_flag(st->message, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(st->message, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Whether a network is known, and as what */

static FAR const pnut_wifi_known_network_t *
wifi_known(FAR struct wifi_scr_s *st, FAR const char *name)
{
  int i;

  for (i = 0; st->has_known && i < st->known.networks_count; i++)
    {
      if (strcmp(st->known.networks[i].name, name) == 0)
        {
          return &st->known.networks[i];
        }
    }

  return NULL;
}

/* "Secured · strong · saved": the security if known (NULL if not), the
 * strength, whether it is known
 */

static void wifi_detail(FAR char *buf, size_t size,
                        FAR const pnut_wifi_security_t *security,
                        FAR const char *strength, bool saved)
{
  buf[0] = '\0';
  if (security != NULL)
    {
      strlcpy(buf, *security == PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN ?
                   UI_T_OPEN_NETWORK : UI_T_SECURED, size);
      strlcat(buf, UI_DOT, size);
    }

  strlcat(buf, strength, size);
  if (saved)
    {
      strlcat(buf, UI_DOT, size);
      strlcat(buf, UI_T_SAVED, size);
    }
}

static bool wifi_connected(FAR struct screen_s *s)
{
  FAR const struct ui_status_s *dev = &s->ui->status;

  return dev->has_wifi &&
         (dev->wifi.state == PNUT_WIFI_INFO_STATE_JOINED ||
          dev->wifi.state == PNUT_WIFI_INFO_STATE_JOINING);
}

/* The name of the network a row shows, for the focus to follow it */

static FAR const char *wifi_row_name(FAR struct screen_s *s,
                                     FAR lv_obj_t *row)
{
  FAR struct wifi_scr_s *st = s->state;
  uint16_t id;

  if (row == NULL)
    {
      return NULL;
    }

  if (row == st->conn)
    {
      return s->ui->status.wifi.name;
    }

  id = block_id(row);
  if (id >= WIFI_ID_NET && id < WIFI_ID_NET + st->nrows)
    {
      return st->drawn[id - WIFI_ID_NET].name;
    }

  return NULL;
}

static FAR lv_obj_t *wifi_net_row(FAR struct screen_s *s, int j)
{
  FAR struct wifi_scr_s *st = s->state;

  if (j == st->nrows)
    {
      st->rows[j] = block_row(st->list.box, WIFI_ID_NET + j, NULL, "", "");
      block_row_chevron(st->rows[j]);
      st->nrows++;
    }

  return st->rows[j];
}

/* The focus moved: its id for tests, its network's name to follow */

static void wifi_track(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR const char *name = wifi_row_name(s, st->list.focus);

  s->focus = st->list.focus != NULL ? block_id(st->list.focus) : 0;
  strlcpy(st->focus_name, name != NULL ? name : "", sizeof(st->focus_name));
}

/* Show everything as it is now: the switch, the network joined, the
 * networks in reach (the one joined left out); the focus stays on its
 * network
 */

static void wifi_show(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;
  FAR const pnut_wifi_known_network_t *k;
  FAR const pnut_wifi_network_t *n;
  char name[DESCRIBE_NAME_MAX];
  char detail[WIFI_DETAIL_MAX];
  FAR const char *focused = st->focus_name;
  FAR lv_obj_t *row;
  struct tm tm;
  bool on = wifi_on(st);
  bool conn = on && wifi_connected(s);
  int i;
  int j = 0;

  if (st->enabled.known)
    {
      block_switch_set(st->sw, on);
    }

  /* The network joined */

  if (conn)
    {
      describe_name(dev->wifi.name, name, sizeof(name));
      k = wifi_known(st, dev->wifi.name);
      if (dev->wifi.state == PNUT_WIFI_INFO_STATE_JOINING)
        {
          strlcpy(detail, UI_T_JOINING_DOTS, sizeof(detail));
        }
      else
        {
          wifi_detail(detail, sizeof(detail),
                      k != NULL ? &k->security : NULL,
                      describe_strength(dev->wifi.signal), false);
        }

      block_row_set(st->conn, name, detail);
      lv_obj_remove_flag(st->conn_head, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(st->conn, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(st->conn_head, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(st->conn, LV_OBJ_FLAG_HIDDEN);
    }

  /* The networks in reach */

  if (st->has_known)
    {
      wifilist_known(&st->nets, &st->known);
    }

  for (i = 0; on && i < st->nets.count; i++)
    {
      n = &st->nets.nets[i];
      if (conn && strcmp(n->name, dev->wifi.name) == 0)
        {
          continue;
        }

      describe_name(n->name, name, sizeof(name));
      wifi_detail(detail, sizeof(detail), &n->security,
                  describe_strength(n->signal), st->nets.known[i]);
      row = wifi_net_row(s, j);
      block_row_set(row, name, detail);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
      st->drawn[j]       = *n;
      st->drawn_known[j] = st->nets.known[i];
      j++;
    }

  for (i = j; i < st->nrows; i++)
    {
      lv_obj_add_flag(st->rows[i], LV_OBJ_FLAG_HIDDEN);
    }

  /* Its heading */

  if (!on)
    {
      lv_obj_add_flag(st->near_head, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(st->near_head, LV_OBJ_FLAG_HIDDEN);
      if (st->scanning)
        {
          block_heading_set(st->near_head, UI_T_SCANNING);
        }
      else if (st->scan_failed)
        {
          block_heading_set(st->near_head, UI_T_SCAN_FAILED);
        }
      else if (st->scanned && j == 0)
        {
          block_heading_set(st->near_head, UI_T_NONE_NEARBY);
        }
      else if (st->scanned)
        {
          status_local(&s->ui->status, st->scanned_at, &tm);
          snprintf(detail, sizeof(detail), UI_T_SCANNED, tm.tm_hour,
                   tm.tm_min);
          block_heading_set(st->near_head, detail);
        }
      else
        {
          block_heading_set(st->near_head, UI_T_NEARBY);
        }
    }

  /* The focus: on its network, wherever that is now */

  if (focused[0] != '\0')
    {
      if (conn && strcmp(focused, dev->wifi.name) == 0)
        {
          list_focus(&st->list, st->conn);
        }
      else
        {
          for (i = 0; i < j; i++)
            {
              if (strcmp(st->drawn[i].name, focused) == 0)
                {
                  list_focus(&st->list, st->rows[i]);
                  break;
                }
            }
        }
    }

  list_check(&st->list);
  wifi_track(s);
  wifilist_shown(&st->nets, glue_now());
}

/* After an answer or a timer: shown, and laid out for taps */

static void wifi_changed(FAR struct screen_s *s)
{
  wifi_show(s);
  ui_place(s->ui);
}

/* The known networks */

static void wifi_known_done(FAR struct pnut_wifi_client_s *client,
                            int status,
                            FAR const pnut_wifi_known_networks_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);
  FAR struct wifi_scr_s *st;

  if (s == NULL || status != PNUT_STATUS_OK)
    {
      return;
    }

  st = s->state;
  st->known     = *out;
  st->has_known = true;
  wifi_changed(s);
}

static void wifi_read_known(FAR struct screen_s *s)
{
  FAR struct pnut_wifi_client_s *client = ui_wifi(s->ui);
  pnut_wifi_known_request_t req;

  /* Not connected yet: read once it is (UI_WIFI_SERVICE) */

  if (client != NULL)
    {
      memset(&req, 0, sizeof(req));
      pnut_wifi_known(client, &req, 0, wifi_known_done, wifi_token(s));
    }
}

/* The scan */

static void wifi_redraw(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct screen_s *s = arg;
  FAR struct wifi_scr_s *st = s->state;

  st->redraw = NULL;
  wifi_changed(s);
}

static void wifi_scan_over(FAR struct screen_s *s, bool failed)
{
  FAR struct wifi_scr_s *st = s->state;

  st->scanning    = false;
  st->scan_failed = failed;
  st->scanned     = !failed;
  st->scanned_at  = status_now(&s->ui->status);
  wifilist_end(&st->nets);
  if (st->redraw != NULL)
    {
      pnut_timer_cancel(s->ui->module.loop, st->redraw);
      st->redraw = NULL;
    }

  wifi_changed(s);
}

static void wifi_scan_opened(FAR struct pnut_wifi_client_s *client,
                             int status, FAR const pnut_error_t *err,
                             FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);

  if (s != NULL && status != PNUT_STATUS_OK)
    {
      wifi_scan_over(s, true);
    }
}

static void wifi_scan_item(FAR struct pnut_wifi_client_s *client,
                           int status, FAR const pnut_wifi_network_t *item,
                           FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);
  FAR struct wifi_scr_s *st;
  uint64_t now = glue_now();
  uint64_t wait;

  if (s == NULL)
    {
      return;
    }

  st = s->state;
  if (status != PNUT_STATUS_OK)
    {
      wifi_scan_over(s, status != PNUT_STATUS_ENDED);
      return;
    }

  wifilist_add(&st->nets, item);
  if (++st->got == WIFI_CREDIT)
    {
      st->got = 0;
      pnut_wifi_scan_credit(client, st->stream, WIFI_CREDIT);
      if (!st->scanning)
        {
          return;                 /* Ended inside, the connection lost */
        }
    }

  /* The first results at once, then at most every 5 s */

  if (wifilist_due(&st->nets, now))
    {
      wifi_changed(s);
    }
  else if (st->redraw == NULL && st->nets.dirty)
    {
      wait = WIFILIST_EVERY_MS - (now - st->nets.shown);
      pnut_timer_start(s->ui->module.loop, (uint32_t)wait, 0, wifi_redraw, s,
                       &st->redraw);
    }
}

static void wifi_scan(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR struct pnut_wifi_client_s *client = ui_wifi(s->ui);
  pnut_wifi_scan_request_t req;

  if (st->scanning || !wifi_on(st))
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  wifilist_start(&st->nets, glue_now());
  st->got     = 0;
  st->scanned = false;

  /* Wi-Fi's service not there yet: the scan once it is (UI_WIFI_SERVICE) */

  if (client != NULL && !pnut_client_connected(client->client))
    {
      st->scan_failed = false;
      wifi_show(s);
      return;
    }

  st->scan_failed = client == NULL ||
                    pnut_wifi_scan(client, &req, WIFI_CREDIT, 0,
                                   wifi_scan_opened, wifi_scan_item,
                                   wifi_token(s), &st->stream) < 0;
  st->scanning    = !st->scan_failed;
  wifi_show(s);
}

/* Stop the scan, its networks forgotten: Wi-Fi turned off, or leaving */

static void wifi_scan_stop(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;

  if (st->scanning)
    {
      pnut_wifi_scan_cancel(&s->ui->wifi, st->stream);
      st->scanning = false;
    }

  if (st->redraw != NULL)
    {
      pnut_timer_cancel(s->ui->module.loop, st->redraw);
      st->redraw = NULL;
    }

  wifilist_start(&st->nets, glue_now());
  wifilist_end(&st->nets);
  st->scanned     = false;
  st->scan_failed = false;
}

/* Join, Leave, Forget: a failure said under the switch */

static void wifi_answer(FAR struct screen_s *s, int status,
                        FAR const pnut_error_t *err)
{
  if (status != PNUT_STATUS_OK)
    {
      wifi_message(s, describe_wifi_failure(status,
                                            err != NULL ? err->code : 0));
    }

  wifi_read_known(s);
  wifi_changed(s);
}

static void wifi_join_done(FAR struct pnut_wifi_client_s *client,
                           int status, FAR const pnut_wifi_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);

  if (s != NULL)
    {
      wifi_answer(s, status, err);
    }
}

static void wifi_join(FAR struct screen_s *s, FAR const char *name)
{
  FAR struct pnut_wifi_client_s *client = ui_wifi(s->ui);
  pnut_wifi_join_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.name, name, sizeof(req.name));
  wifi_message(s, NULL);
  if (client == NULL ||
      pnut_wifi_join(client, &req, WIFI_JOIN_MS, wifi_join_done,
                     wifi_token(s)) < 0)
    {
      wifi_message(s, UI_T_WIFI_NO_ANSWER);
    }
}

static void wifi_leave_done(FAR struct pnut_wifi_client_s *client,
                            int status, FAR const pnut_wifi_reply_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);

  if (s != NULL)
    {
      wifi_answer(s, status, err);
    }
}

static void wifi_forget_done(FAR struct pnut_wifi_client_s *client,
                             int status, FAR const pnut_wifi_reply_t *out,
                             FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct screen_s *s = wifi_screen(client, arg);

  if (s != NULL)
    {
      wifi_answer(s, status, err);
    }
}

static void wifi_forget_chosen(FAR struct screen_s *s, int index)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR struct pnut_wifi_client_s *client = ui_wifi(s->ui);
  pnut_wifi_forget_request_t req;

  if (index != 0 || client == NULL)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  strlcpy(req.name, st->forget_name, sizeof(req.name));
  req.security = st->forget_security;
  wifi_message(s, NULL);
  if (pnut_wifi_forget(client, &req, 0, wifi_forget_done,
                       wifi_token(s)) < 0)
    {
      wifi_message(s, UI_T_WIFI_NO_ANSWER);
    }
}

static void wifi_chosen(FAR struct screen_s *s, int index)
{
  static FAR const char * const confirm[2] =
  {
    UI_T_FORGET_YES, UI_T_KEEP
  };

  FAR struct wifi_scr_s *st = s->state;
  FAR struct pnut_wifi_client_s *client = ui_wifi(s->ui);
  pnut_wifi_leave_request_t req;

  switch (st->opts[index])
    {
      case WIFI_OPT_SCAN:
        wifi_scan_stop(s);
        wifi_scan(s);
        break;

      case WIFI_OPT_LEAVE:
        memset(&req, 0, sizeof(req));
        wifi_message(s, NULL);
        if (client == NULL ||
            pnut_wifi_leave(client, &req, 0, wifi_leave_done,
                            wifi_token(s)) < 0)
          {
            wifi_message(s, UI_T_WIFI_NO_ANSWER);
          }
        break;

      case WIFI_OPT_FORGET:
        ui_options(s, confirm, 2, wifi_forget_chosen);
        break;

      default:
        break;
    }
}

/* The options: Scan again (all is false: only those of the network
 * joined), Leave when joined, Forget on a known network's row
 */

static void wifi_options(FAR struct screen_s *s, bool all)
{
  static FAR const char * const names[3] =
  {
    UI_T_SCAN_AGAIN, UI_T_LEAVE, UI_T_FORGET
  };

  FAR struct wifi_scr_s *st = s->state;
  FAR const pnut_wifi_known_network_t *k;
  FAR const char *name = wifi_row_name(s, st->list.focus);
  FAR const char *items[3];
  uint8_t n = 0;
  int i;

  if (!wifi_on(st))
    {
      return;
    }

  k = name != NULL ? wifi_known(st, name) : NULL;
  if (all)
    {
      st->opts[n++] = WIFI_OPT_SCAN;
    }

  if (wifi_connected(s))
    {
      st->opts[n++] = WIFI_OPT_LEAVE;
    }

  if (k != NULL)
    {
      strlcpy(st->forget_name, k->name, sizeof(st->forget_name));
      st->forget_security = k->security;
      st->opts[n++] = WIFI_OPT_FORGET;
    }

  for (i = 0; i < n; i++)
    {
      items[i] = names[st->opts[i]];
    }

  if (n > 0)
    {
      ui_options(s, items, n, wifi_chosen);
    }
}

/* The switch refused: said (the watch closes with the screen, its calls
 * with it)
 */

static void wifi_set_done(FAR void *arg, int status, uint32_t code)
{
  FAR struct screen_s *s = arg;

  if (status != PNUT_STATUS_OK)
    {
      wifi_message(s, UI_T_WIFI_FAILED);
      ui_place(s->ui);
    }
}

/* The primary action on the row focused */

static void wifi_open(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR lv_obj_t *row = st->list.focus;
  FAR const pnut_wifi_network_t *n;
  uint16_t id;

  if (row == NULL)
    {
      return;
    }

  if (row == st->sw)
    {
      if (st->enabled.known)
        {
          wifi_message(s, NULL);
          if (setting_set_bool(&st->watch, 0, !wifi_on(st), wifi_set_done,
                               s) < 0)
            {
              wifi_message(s, UI_T_WIFI_FAILED);
            }
        }

      return;
    }

  if (row == st->conn)
    {
      wifi_options(s, false);       /* Leave, Forget */
      return;
    }

  id = block_id(row);
  if (id < WIFI_ID_NET || id >= WIFI_ID_NET + st->nrows)
    {
      return;
    }

  n = &st->drawn[id - WIFI_ID_NET];
  if (st->drawn_known[id - WIFI_ID_NET] ||
      n->security == PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN)
    {
      wifi_join(s, n->name);
    }
  else
    {
      ui_push(s->ui, &g_scr_join, n);
    }
}

/* wifi.enabled read, or changed: the scan follows it */

static void wifi_setting(FAR void *arg, int index)
{
  FAR struct screen_s *s = arg;
  FAR struct wifi_scr_s *st = s->state;

  if (wifi_on(st))
    {
      if (!st->scanning && !st->scanned)
        {
          wifi_scan(s);
        }
    }
  else
    {
      wifi_scan_stop(s);
    }

  wifi_changed(s);
}

static void wifi_build(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;

  ui_header(s, UI_ICON_WIFI, UI_T_WIFI);
  list_init(&st->list, s->root);
  st->sw = block_switch_row(st->list.box, WIFI_ID_SWITCH, UI_T_WIFI, false);
  block_row_set_value(st->sw, UI_T_UNKNOWN);

  st->message = lv_label_create(st->list.box);
  theme_label(st->message, &g_ui_sans_12);
  lv_label_set_long_mode(st->message, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(st->message, lv_pct(100));
  lv_obj_set_style_pad_hor(st->message, 8, 0);
  lv_obj_set_style_pad_ver(st->message, 4, 0);
  lv_obj_add_flag(st->message, LV_OBJ_FLAG_HIDDEN);

  st->conn_head = block_heading(st->list.box, UI_T_CONNECTED);
  st->conn = block_row(st->list.box, WIFI_ID_CONNECTED, NULL, "", "");
  block_row_chevron(st->conn);
  st->near_head = block_heading(st->list.box, UI_T_NEARBY);
  block_softkeys(s->root, UI_T_OPTIONS, UI_T_OPEN, UI_T_BACK);

  st->seen_state = dev->wifi.state;
  strlcpy(st->seen_name, dev->wifi.name, sizeof(st->seen_name));
  st->enabled.owner = "wifi";
  st->enabled.key   = "enabled";
  st->watch_open = setting_open(&st->watch, s->ui->module.loop,
                                &st->enabled, 1, wifi_setting, s) >= 0;

  wifi_read_known(s);
  wifi_show(s);
  list_move(&st->list, 1);
  wifi_track(s);
}

static bool wifi_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct wifi_scr_s *st = s->state;

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        list_move(&st->list, action == UI_ACT_DOWN ? 1 : -1);
        wifi_track(s);
        return true;

      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        wifi_open(s);
        return true;

      case UI_ACT_OPTIONS:
      case UI_ACT_SOFT_LEFT:
        wifi_options(s, true);
        return true;

      default:
        return false;
    }
}

static void wifi_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR lv_obj_t *row = list_find(&st->list, id);

  if (row != NULL)
    {
      list_focus(&st->list, row);
      wifi_track(s);
      wifi_open(s);
    }
}

static void wifi_status(FAR struct screen_s *s, uint8_t what)
{
  FAR struct wifi_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;

  /* Wi-Fi's service back, or its state changed (a join elsewhere): the
   * known networks again
   */

  if ((what & UI_WIFI_SERVICE) != 0 ||
      ((what & STATUS_WIFI) != 0 &&
       (dev->wifi.state != st->seen_state ||
        strcmp(dev->wifi.name, st->seen_name) != 0)))
    {
      st->seen_state = dev->wifi.state;
      strlcpy(st->seen_name, dev->wifi.name, sizeof(st->seen_name));
      wifi_read_known(s);
    }

  /* The service back: the scan it could not make */

  if ((what & UI_WIFI_SERVICE) != 0 && wifi_on(st) && !st->scanning &&
      (st->scan_failed || !st->scanned))
    {
      wifi_scan(s);
    }

  if (what & (STATUS_WIFI | STATUS_TIME))
    {
      wifi_show(s);
    }
}

static void wifi_leave(FAR struct screen_s *s)
{
  FAR struct wifi_scr_s *st = s->state;

  wifi_scan_stop(s);
  if (st->watch_open)
    {
      setting_close(&st->watch);
      st->watch_open = false;
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_wifi =
{
  "wifi",
  wifi_build,
  wifi_key,
  wifi_event,
  wifi_status,
  wifi_leave,
  sizeof(struct wifi_scr_s)
};
