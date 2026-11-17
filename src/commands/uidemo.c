/****************************************************************************
 * pnut-os/src/commands/uidemo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* uidemo: an app from the developer's shell (RFC 0037), to see the
 * system UI's templates on the device.  It opens as org.pnut.demo, shows
 * the template named, with made-up content, and prints every event; Back
 * on its screen leaves.
 *
 *   uidemo list       a List of 40 rows, pulled a page at a time
 *   uidemo detail     a Detail
 *   uidemo empty      an Empty
 *   uidemo confirm    a List, and a Confirm sheet over it
 *   uidemo busy       a Busy screen whose bar fills, then a List
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/ui.pnut.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define UIDEMO_SERVICE  "ui"
#define UIDEMO_ID       "org.pnut.demo"
#define UIDEMO_CREDIT   8
#define UIDEMO_ROWS     40
#define UIDEMO_STEP_MS  1000

/* Rows a message carries (ui.proto) */

#define UIDEMO_PAGE \
  (sizeof(((FAR pnut_ui_rows_request_t *)0)->rows) / \
   sizeof(((FAR pnut_ui_rows_request_t *)0)->rows[0]))

/* The demo's screens' ids */

#define UIDEMO_LIST     1
#define UIDEMO_DETAIL   2
#define UIDEMO_EMPTY    3
#define UIDEMO_BUSY     4
#define UIDEMO_SHEET    5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct uidemo_s
{
  struct pnut_module_s module;    /* First */
  FAR const char *what;
  struct pnut_ui_client_s client;
  bool open;
  bool opened;
  uint16_t stream;
  FAR struct pnut_timer_s *timer;
  uint32_t progress;
  bool confirm;                   /* The Confirm, once the List is shown */
  bool shown;                     /* The first screen answered */
  bool leaving;

  /* The requests: large (a List is up to 3.5 KB), so in this state, from
   * the heap, in external memory
   */

  pnut_ui_show_request_t show;
  pnut_ui_rows_request_t rows;
  pnut_ui_update_request_t update;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void uidemo_confirm(FAR struct uidemo_s *d);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_uidemo_names[] =
{
  "Ola", "Tomek", "Bank", "Base camp", "Mama", "Kasia", "Piotr", "Ania"
};

static FAR const char * const g_uidemo_lines[] =
{
  "Are we still on for Thursday?", "Invoice is in your email",
  "One-time code", "Reached the hut, all fine"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void uidemo_stop(FAR struct uidemo_s *d, int ret)
{
  pnut_loop_stop(d->module.loop, ret);
}

static void uidemo_done(FAR struct pnut_ui_client_s *client, int status,
                        FAR const pnut_ui_reply_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct uidemo_s *d = arg;

  if (status != PNUT_STATUS_OK)
    {
      printf("uidemo: answered %d, code %u\n", status,
             err != NULL ? (unsigned)err->code : 0);
    }

  if (d->leaving)
    {
      uidemo_stop(d, 0);
    }
  else if (!d->shown)
    {
      /* The first screen: without it there is no Back to leave by */

      d->shown = true;
      if (status != PNUT_STATUS_OK)
        {
          uidemo_stop(d, -EINVAL);
        }
      else if (d->confirm)
        {
          /* One large request at a time: the sheet after the List */

          d->confirm = false;
          uidemo_confirm(d);
        }
    }
}

static void uidemo_row(FAR pnut_ui_row_t *row, uint32_t i)
{
  FAR const char *name = g_uidemo_names[i % 8];

  memset(row, 0, sizeof(*row));
  row->id = 100 + i;
  row->initials = true;
  row->lead[0] = name[0];
  row->lead[1] = name[1] >= 'a' && name[1] <= 'z' ? name[1] - 'a' + 'A' :
                 name[1];
  strlcpy(row->title, name, sizeof(row->title));
  strlcpy(row->detail, g_uidemo_lines[i % 4], sizeof(row->detail));
  row->which_value = PNUT_UI_ROW_TEXT_TAG;
  snprintf(row->value.text, sizeof(row->value.text), "%02u:%02u",
           (unsigned)(9 + i % 12), (unsigned)(i * 7 % 60));
  row->unread = i % 5 == 0;
}

static void uidemo_send(FAR struct uidemo_s *d)
{
  int ret = pnut_ui_show(&d->client, &d->show, 3000, uidemo_done, d);

  if (ret < 0)
    {
      printf("uidemo: show: %d\n", ret);
      if (!d->shown)
        {
          uidemo_stop(d, ret);    /* No screen to leave by */
        }
    }
}

static void uidemo_list(FAR struct uidemo_s *d)
{
  FAR pnut_ui_show_request_t *in = &d->show;
  int i;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_LIST;
  strcpy(in->title, "Inbox");
  in->options_count = 2;
  in->options[0].id = 1;
  strcpy(in->options[0].label, "Call");
  strcpy(in->options[0].letter, "c");
  in->options[1].id = 2;
  strcpy(in->options[1].label, "Delete conversation");
  strcpy(in->options[1].letter, "x");
  in->which_content = PNUT_UI_SHOW_REQUEST_LIST_TAG;
  in->content.list.count = UIDEMO_ROWS;
  in->content.list.rows_count = sizeof(in->content.list.rows) /
                               sizeof(in->content.list.rows[0]);
  for (i = 0; i < in->content.list.rows_count; i++)
    {
      uidemo_row(&in->content.list.rows[i], i);
    }

  uidemo_send(d);
}

static void uidemo_detail(FAR struct uidemo_s *d)
{
  static FAR const char * const labels[4] =
    {
      "Mobile", "Email", "Birthday", "Last message"
    };

  static FAR const char * const values[4] =
    {
      "000 000 000", "someone@example.com", "14 March", "Mon, 18:20"
    };

  static FAR const char * const icons[4] =
    {
      "phone", "email", "", "messages"
    };

  FAR pnut_ui_show_request_t *in = &d->show;
  int i;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_DETAIL;
  strcpy(in->title, "Someone");
  strcpy(in->middle, "Call");
  in->which_content = PNUT_UI_SHOW_REQUEST_DETAIL_TAG;
  in->content.detail.items_count = 4;
  for (i = 0; i < 4; i++)
    {
      in->content.detail.items[i].id = i == 2 ? 0 : i + 1;
      strcpy(in->content.detail.items[i].label, labels[i]);
      in->content.detail.items[i].which_value = PNUT_UI_ITEM_TEXT_TAG;
      strcpy(in->content.detail.items[i].value.text, values[i]);
      strcpy(in->content.detail.items[i].icon, icons[i]);
    }

  uidemo_send(d);
}

static void uidemo_empty(FAR struct uidemo_s *d)
{
  FAR pnut_ui_show_request_t *in = &d->show;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_EMPTY;
  strcpy(in->title, "Drafts");
  strcpy(in->middle, "New");
  in->which_content = PNUT_UI_SHOW_REQUEST_EMPTY_TAG;
  strcpy(in->content.empty.heading, "No drafts");
  strcpy(in->content.empty.text, "Messages you leave unsent are kept here.");
  strcpy(in->content.empty.icon, "messages");
  uidemo_send(d);
}

static void uidemo_confirm(FAR struct uidemo_s *d)
{
  FAR pnut_ui_show_request_t *in = &d->show;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_SHEET;
  in->sheet  = true;
  in->which_content = PNUT_UI_SHOW_REQUEST_CONFIRM_TAG;
  strcpy(in->content.confirm.question, "Delete conversation with Ola?");
  strcpy(in->content.confirm.detail,
         "Its messages are removed from this phone. This can't be undone.");
  strcpy(in->content.confirm.confirm, "Delete");
  uidemo_send(d);
}

static void uidemo_busy(FAR struct uidemo_s *d)
{
  FAR pnut_ui_show_request_t *in = &d->show;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_BUSY;
  strcpy(in->title, "Demo");
  in->which_content = PNUT_UI_SHOW_REQUEST_BUSY_TAG;
  strcpy(in->content.busy.heading, "Counting");
  snprintf(in->content.busy.text, sizeof(in->content.busy.text),
           "%u of 100", (unsigned)d->progress);
  in->content.busy.cancel = true;
  in->content.busy.has_progress = true;
  in->content.busy.progress = d->progress;
  uidemo_send(d);
}

/* The Busy's bar fills a tenth a second, by Update; then the List */

static void uidemo_tick(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct uidemo_s *d = arg;
  FAR pnut_ui_update_request_t *in = &d->update;

  d->progress += 10;
  if (d->progress > 100)
    {
      pnut_timer_cancel(loop, d->timer);
      d->timer = NULL;
      uidemo_list(d);
      return;
    }

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_BUSY;
  in->which_change = PNUT_UI_UPDATE_REQUEST_BUSY_TAG;
  strcpy(in->change.busy.heading, "Counting");
  snprintf(in->change.busy.text, sizeof(in->change.busy.text), "%u of 100",
           (unsigned)d->progress);
  in->change.busy.cancel = true;
  in->change.busy.has_progress = true;
  in->change.busy.progress = d->progress;
  if (pnut_ui_update(&d->client, in, 3000, uidemo_done, d) < 0)
    {
      printf("uidemo: update not sent\n");
    }
}

static void uidemo_first(FAR struct uidemo_s *d)
{
  if (strcmp(d->what, "detail") == 0)
    {
      uidemo_detail(d);
    }
  else if (strcmp(d->what, "empty") == 0)
    {
      uidemo_empty(d);
    }
  else if (strcmp(d->what, "busy") == 0)
    {
      uidemo_busy(d);
      if (pnut_timer_start(d->module.loop, UIDEMO_STEP_MS, UIDEMO_STEP_MS,
                           uidemo_tick, d, &d->timer) < 0)
        {
          printf("uidemo: no timer: the bar stays\n");
        }
    }
  else
    {
      d->confirm = strcmp(d->what, "confirm") == 0;
      uidemo_list(d);
    }
}

static void uidemo_leave(FAR struct uidemo_s *d)
{
  pnut_ui_leave_request_t in;

  if (d->timer != NULL)
    {
      pnut_timer_cancel(d->module.loop, d->timer);
      d->timer = NULL;
    }

  memset(&in, 0, sizeof(in));
  d->leaving = true;
  if (pnut_ui_leave(&d->client, &in, 3000, uidemo_done, d) < 0)
    {
      uidemo_stop(d, 0);
    }
}

static void uidemo_rows(FAR struct uidemo_s *d, uint32_t first)
{
  FAR pnut_ui_rows_request_t *in = &d->rows;
  uint32_t i;

  memset(in, 0, sizeof(*in));
  in->screen = UIDEMO_LIST;
  in->first  = first;
  for (i = first; i < UIDEMO_ROWS && in->rows_count < UIDEMO_PAGE; i++)
    {
      uidemo_row(&in->rows[in->rows_count++], i);
    }

  if (pnut_ui_rows(&d->client, in, 3000, uidemo_done, d) < 0)
    {
      printf("uidemo: rows not sent\n");
    }
}

static void uidemo_input(FAR struct uidemo_s *d,
                         FAR const pnut_ui_input_t *in)
{
  static FAR const char * const actions[] =
    {
      "none", "row opened", "option chosen", "middle", "back", "confirmed",
      "cancelled"
    };

  printf("uidemo: screen %u: %s, row %u, option %u\n",
         (unsigned)in->screen,
         in->action < 7 ? actions[in->action] : "?",
         (unsigned)in->row, (unsigned)in->option);

  switch (in->action)
    {
      case PNUT_UI_INPUT_ACTION_BACK:
        uidemo_leave(d);
        break;

      case PNUT_UI_INPUT_ACTION_CANCELLED:
        if (in->screen == UIDEMO_BUSY)
          {
            if (d->timer != NULL)
              {
                pnut_timer_cancel(d->module.loop, d->timer);
                d->timer = NULL;
              }

            uidemo_list(d);
          }
        break;

      default:
        break;
    }
}

static void uidemo_item(FAR struct pnut_ui_client_s *client, int status,
                        FAR const pnut_ui_event_t *item, FAR void *arg)
{
  FAR struct uidemo_s *d = arg;

  if (item == NULL)
    {
      printf("uidemo: the system UI ended the session: %d\n", status);
      uidemo_stop(d, status == PNUT_STATUS_ENDED ? 0 : -EIO);
      return;
    }

  pnut_ui_open_credit(client, d->stream, 1);
  switch (item->which_kind)
    {
      case PNUT_UI_EVENT_WELCOME_TAG:
        printf("uidemo: welcome, catalogue level %u\n",
               (unsigned)item->kind.welcome.level);
        uidemo_first(d);
        break;

      case PNUT_UI_EVENT_INPUT_TAG:
        uidemo_input(d, &item->kind.input);
        break;

      case PNUT_UI_EVENT_PAGE_TAG:
        printf("uidemo: page needed from %u\n",
               (unsigned)item->kind.page.first);
        uidemo_rows(d, item->kind.page.first);
        break;

      case PNUT_UI_EVENT_STOP_TAG:
        uidemo_leave(d);
        break;

      default:
        break;
    }
}

static void uidemo_opened(FAR struct pnut_ui_client_s *client, int status,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct uidemo_s *d = arg;

  if (status != PNUT_STATUS_OK)
    {
      printf("uidemo: not opened: %d, code %u\n", status,
             err != NULL ? (unsigned)err->code : 0);
      uidemo_stop(d, status == PNUT_STATUS_DENIED ? -EPERM : -EIO);
    }
}

static void uidemo_state(FAR struct pnut_ui_client_s *client,
                         bool connected, FAR void *arg)
{
  FAR struct uidemo_s *d = arg;
  pnut_ui_open_request_t in;

  if (!connected || d->opened)
    {
      return;
    }

  memset(&in, 0, sizeof(in));
  strlcpy(in.app, UIDEMO_ID, sizeof(in.app));
  in.level = 1;
  if (pnut_ui_open(client, &in, UIDEMO_CREDIT, 3000, uidemo_opened,
                   uidemo_item, d, &d->stream) == 0)
    {
      d->opened = true;
    }
  else
    {
      printf("uidemo: cannot open\n");
      uidemo_stop(d, -EIO);
    }
}

static int uidemo_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  FAR struct uidemo_s *d = (FAR struct uidemo_s *)module;
  int ret;

  ret = pnut_ui_connect(loop, UIDEMO_SERVICE, uidemo_state, d, &d->client);
  if (ret < 0)
    {
      fprintf(stderr, "uidemo: cannot reach the system UI: %d\n", ret);
      return ret;
    }

  d->open = true;
  return OK;
}

static void uidemo_stopped(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct uidemo_s *d = (FAR struct uidemo_s *)module;

  if (d->timer != NULL)
    {
      pnut_timer_cancel(loop, d->timer);
      d->timer = NULL;
    }

  if (d->open)
    {
      pnut_ui_disconnect(&d->client);
      d->open = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct pnut_loop_s *loop;
  FAR struct uidemo_s *d;
  int ret;

  if (argc != 2 || (strcmp(argv[1], "list") != 0 &&
                    strcmp(argv[1], "detail") != 0 &&
                    strcmp(argv[1], "empty") != 0 &&
                    strcmp(argv[1], "confirm") != 0 &&
                    strcmp(argv[1], "busy") != 0))
    {
      fprintf(stderr, "usage: uidemo list|detail|empty|confirm|busy\n");
      return EXIT_FAILURE;
    }

  d = calloc(1, sizeof(*d));
  if (d == NULL)
    {
      fprintf(stderr, "uidemo: no memory\n");
      return EXIT_FAILURE;
    }

  d->what         = argv[1];
  d->module.name  = "uidemo";
  d->module.start = uidemo_start;
  d->module.stop  = uidemo_stopped;

  pnut_loop_defaults(&config);
  config.name    = "uidemo";
  config.fds     = 3;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;
  config.budget  = 0;           /* A command: no one else to serve */

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "uidemo: no loop: %d\n", ret);
      free(d);
      return EXIT_FAILURE;
    }

  ret = pnut_module_add(loop, &d->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  free(d);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
