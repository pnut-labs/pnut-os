/****************************************************************************
 * pnut-os/src/ui/app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The open app's screen (RFC 0037): one screen on the system UI's stack,
 * the app's top screen as its last Show described it.  The header carries
 * the app's icon (its manifest's) and the screen's title; the softkeys,
 * Options when the app gave any, the primary action, Back.  Back and the
 * primary action go to the app; the template draws the content and takes
 * the other keys.  A new Show replaces the screen whole.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include <pnut/log.h>

#include "app.h"
#include "apptpl.h"
#include "blocks.h"
#include "describe.h"
#include "host.h"
#include "icons.h"
#include "keymap.h"
#include "text.h"
#include "ui.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct app_icon_s
{
  FAR const char *name;
  FAR const char *glyph;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct app_icon_s g_app_icons[] =
{
  UI_ICON_TABLE
};

/* The letters the key map steers with (RFC 0019's suggested map): never
 * an option's
 */

static const char g_app_steering[] = "wasdqe";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct app_scr_s *app_state(FAR struct screen_s *s)
{
  return s->state;
}

/* The template a Show names, or NULL */

static FAR const struct app_tpl_s *
app_template(FAR const pnut_ui_show_request_t *in)
{
  switch (in->which_content)
    {
      case PNUT_UI_SHOW_REQUEST_LIST_TAG:
        return &g_tpl_list;

      default:
        return NULL;
    }
}

/* The app's screen on the stack, or NULL */

static FAR struct screen_s *app_screen(FAR struct ui_s *ui)
{
  int i;

  for (i = ui->depth - 1; i >= 0; i--)
    {
      if (ui->stack[i]->cls == &g_scr_app)
        {
          return ui->stack[i];
        }
    }

  return NULL;
}

/* The options and their letters: a letter that steers, one that is no
 * letter, or one used already in the menu is left off, and logged
 */

static void app_options(FAR struct screen_s *s,
                        FAR const pnut_ui_show_request_t *in)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR struct host_app_s *app = host_app(&s->ui->host);
  char c;
  int i;
  int j;

  st->noptions = in->options_count;
  for (i = 0; i < st->noptions; i++)
    {
      st->option_ids[i] = in->options[i].id;
      strlcpy(st->options[i], in->options[i].label,
              sizeof(st->options[i]));

      c = in->options[i].letter[0];
      if (c >= 'A' && c <= 'Z')
        {
          c = c - 'A' + 'a';
        }

      if (c != '\0' && (c < 'a' || c > 'z' ||
                        strchr(g_app_steering, c) != NULL))
        {
          c = '\0';
        }

      for (j = 0; c != '\0' && j < i; j++)
        {
          if (st->letters[j] == c)
            {
              c = '\0';
            }
        }

      if (c == '\0' && in->options[i].letter[0] != '\0')
        {
          pnut_log(&s->ui->module, PNUT_LOG_WARNING,
                   "app %s: option %d drawn without its letter",
                   app != NULL ? app->id : "?", i);
        }

      st->letters[i] = c;
    }
}

/* The softkeys as the screen stands: Options when the app gave any, the
 * primary action's label (blank while it is off), Back; made again on a
 * change (the update comes from a request, never from LVGL's handlers)
 */

static void app_softkeys(FAR struct screen_s *s)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR const char *middle = st->middle[0] != '\0' ? st->middle :
                           st->tpl != NULL ? st->tpl->middle : "";

  if (st->keys != NULL)
    {
      lv_obj_delete(st->keys);
    }

  st->keys = block_softkeys(s->root, st->noptions > 0 ? UI_T_OPTIONS : "",
                            st->middle_off ? "" : middle, UI_T_BACK);
}

/* The screen from a Show that holds together */

static void app_build_from(FAR struct screen_s *s,
                           FAR const pnut_ui_show_request_t *in)
{
  static char title[APP_TEXT_MAX];
  FAR struct app_scr_s *st = app_state(s);
  FAR struct host_app_s *app = host_app(&s->ui->host);

  st->screen = in->screen;
  st->tpl    = app_template(in);
  strlcpy(st->middle, in->middle, sizeof(st->middle));
  st->middle_off = in->middle_off;
  app_options(s, in);

  ui_header(s, app != NULL ? app_icon(app->manifest.icon) : NULL,
            app_text(in->title, title));
  if (st->tpl->build(s, in) < 0)
    {
      st->tpl = NULL;             /* No memory: nothing to keep */
    }

  app_softkeys(s);
}

static void app_build(FAR struct screen_s *s)
{
  app_build_from(s, s->arg);
  s->arg = NULL;                  /* The request is the server's buffer */
}

static void app_chosen(FAR struct screen_s *s, int index)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR const pnut_ui_row_t *row = NULL;

  if (st->tpl == &g_tpl_list)
    {
      row = applist_focused(&st->t.list.list);
    }

  app_input(s, PNUT_UI_INPUT_ACTION_OPTION_CHOSEN,
            row != NULL ? row->id : 0, st->option_ids[index]);
}

static bool app_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR const char *items[APP_OPTIONS];
  int i;

  switch (action)
    {
      case UI_ACT_BACK:
      case UI_ACT_SOFT_RIGHT:
        app_input(s, PNUT_UI_INPUT_ACTION_BACK, 0, 0);
        return true;

      case UI_ACT_OPTIONS:
      case UI_ACT_SOFT_LEFT:
        if (st->noptions > 0)
          {
            for (i = 0; i < st->noptions; i++)
              {
                items[i] = st->options[i];
              }

            ui_options_lettered(s, items, st->letters, st->noptions,
                                app_chosen);
          }

        return true;

      default:
        if (st->tpl != NULL)
          {
            st->tpl->key(s, action);
          }

        return true;              /* The app's screen is never popped */
    }
}

static void app_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct app_scr_s *st = app_state(s);

  if (st->tpl != NULL && st->tpl->event != NULL)
    {
      st->tpl->event(s, id);
    }
}

static void app_leave(FAR struct screen_s *s)
{
  FAR struct app_scr_s *st = app_state(s);

  if (st->tpl != NULL && st->tpl->leave != NULL)
    {
      st->tpl->leave(s);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_app =
{
  "app",
  app_build,
  app_key,
  app_event,
  NULL,
  app_leave,
  sizeof(struct app_scr_s)
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void app_input(FAR struct screen_s *s, int action, uint32_t row,
               uint32_t option)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_INPUT_TAG;
  ev.kind.input.screen = app_state(s)->screen;
  ev.kind.input.action = action;
  ev.kind.input.row    = row;
  ev.kind.input.option = option;
  host_event(&s->ui->host, &ev);
}

void app_page_needed(FAR struct screen_s *s, uint32_t first)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_PAGE_TAG;
  ev.kind.page.screen = app_state(s)->screen;
  ev.kind.page.first  = first;
  host_event(&s->ui->host, &ev);
}

bool app_primary_on(FAR struct screen_s *s)
{
  return !app_state(s)->middle_off;
}

FAR const char *app_text(FAR const char *in, FAR char *out)
{
  describe_name(in, out, APP_TEXT_MAX);
  return out;
}

FAR const char *app_icon(FAR const char *name)
{
  size_t i;

  for (i = 0; name != NULL && name[0] != '\0' &&
              i < sizeof(g_app_icons) / sizeof(g_app_icons[0]); i++)
    {
      if (strcmp(g_app_icons[i].name, name) == 0)
        {
          return g_app_icons[i].glyph;
        }
    }

  return NULL;
}

int app_show(FAR struct ui_s *ui, FAR const pnut_ui_show_request_t *in)
{
  FAR const struct app_tpl_s *tpl = app_template(in);
  FAR struct screen_s *s;
  int ret;

  if (tpl == NULL || in->screen == 0 || in->sheet ||
      tpl->check(in) < 0)
    {
      return -EINVAL;
    }

  /* The first screen goes on top; beneath the SIM PIN, which is the
   * system's (RFC 0037, layer 5)
   */

  s = app_screen(ui);
  if (s != NULL)
    {
      ret = ui_replace(ui, s, &g_scr_app, in);
    }
  else if (ui_top(ui) != NULL && ui_top(ui)->cls == &g_scr_pin)
    {
      ret = ui_insert(ui, ui->depth - 1, &g_scr_app, in);
    }
  else
    {
      ret = ui_push(ui, &g_scr_app, in);
    }

  /* No room for the template's content: no screen, and the app told */

  s = app_screen(ui);
  if (ret == 0 && s != NULL && app_state(s)->tpl == NULL)
    {
      ui_remove(ui, s);
      ret = -ENOMEM;
    }

  return ret;
}

int app_update(FAR struct ui_s *ui, FAR const pnut_ui_update_request_t *in)
{
  static char title[APP_TEXT_MAX];
  FAR struct screen_s *s = app_screen(ui);
  FAR struct app_scr_s *st;

  if (s == NULL || app_state(s)->screen != in->screen)
    {
      return 0;                   /* Another screen's: dropped */
    }

  st = app_state(s);
  switch (in->which_change)
    {
      case PNUT_UI_UPDATE_REQUEST_TITLE_TAG:
        block_header_title(s->header, app_text(in->change.title, title));
        break;

      case PNUT_UI_UPDATE_REQUEST_MIDDLE_TAG:
        strlcpy(st->middle, in->change.middle.label, sizeof(st->middle));
        st->middle_off = in->change.middle.off;
        app_softkeys(s);
        break;

      default:
        if (st->tpl == NULL || st->tpl->update == NULL ||
            st->tpl->update(s, in) < 0)
          {
            return -EINVAL;
          }

        break;
    }

  ui_place(ui);
  return 0;
}

int app_rows(FAR struct ui_s *ui, FAR const pnut_ui_rows_request_t *in)
{
  FAR struct screen_s *s = app_screen(ui);

  if (s != NULL && app_state(s)->screen == in->screen &&
      app_state(s)->tpl != NULL && app_state(s)->tpl->rows != NULL)
    {
      app_state(s)->tpl->rows(s, in);
      ui_place(ui);
    }

  return 0;                       /* Not the screen's: dropped */
}

void app_gone(FAR struct ui_s *ui)
{
  FAR struct screen_s *s = app_screen(ui);

  if (s != NULL)
    {
      ui_remove(ui, s);
    }
}
