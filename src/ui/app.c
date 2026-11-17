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
#include "apps.h"
#include "apptpl.h"
#include "blocks.h"
#include "describe.h"
#include "host.h"
#include "icons.h"
#include "keymap.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The sheet's frame (Cat-Confirm): above the softkeys, 6 px from the
 * left and 10 from the right, a 2 px border and the canvas's 4 px offset
 * shadow; its parts in a column, 10 px in, 8 apart
 */

#define APP_SHEET_X       6
#define APP_SHEET_W       (240 - APP_SHEET_X - 10)
#define APP_SHEET_FOOT    40
#define APP_SHEET_PAD     10
#define APP_SHEET_GAP     8
#define APP_SHEET_SHADOW  4

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

      case PNUT_UI_SHOW_REQUEST_DETAIL_TAG:
        return &g_tpl_detail;

      case PNUT_UI_SHOW_REQUEST_EMPTY_TAG:
        return &g_tpl_empty;

      case PNUT_UI_SHOW_REQUEST_BUSY_TAG:
        return &g_tpl_busy;

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

  /* A Busy screen has Cancel alone, when the app allows it (Cat-Busy) */

  if (st->tpl == &g_tpl_busy)
    {
      st->keys = block_softkeys(s->root, "", "",
                                st->cancel ? UI_T_CANCEL : "");
    }
  else
    {
      st->keys = block_softkeys(s->root,
                                st->noptions > 0 ? UI_T_OPTIONS : "",
                                st->middle_off ? "" : middle, UI_T_BACK);
    }

  /* Under an open sheet, whose own softkeys show */

  if (st->sheet.screen != 0)
    {
      lv_obj_add_flag(st->keys, LV_OBJ_FLAG_HIDDEN);
    }
}

/* An event about one of the app's screens or its sheet */

static void app_input_on(FAR struct screen_s *s, uint32_t screen,
                         int action, uint32_t row, uint32_t option)
{
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_INPUT_TAG;
  ev.kind.input.screen = screen;
  ev.kind.input.action = action;
  ev.kind.input.row    = row;
  ev.kind.input.option = option;
  host_event(&s->ui->host, &ev);
}

/* The sheet's softkeys over the screen's: a Confirm's answer and Cancel;
 * a Busy's Cancel, when the app allows it.  Made again on an Update (a
 * request, never LVGL's handlers: deleted at once, as app_softkeys()).
 */

static void app_sheet_keys(FAR struct screen_s *s)
{
  FAR struct app_sheet_s *sh = &app_state(s)->sheet;

  if (sh->keys != NULL)
    {
      lv_obj_delete(sh->keys);
    }

  sh->keys = block_softkeys(s->root, sh->busy ? "" : sh->confirm, "",
                            !sh->busy || sh->cancel ? UI_T_CANCEL : "");
}

/* The sheet off, the screen's softkeys back.  Its objects go after LVGL
 * is done with the tap that may have closed it.
 */

static void app_sheet_close(FAR struct screen_s *s)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR struct app_sheet_s *sh = &st->sheet;

  if (sh->screen == 0)
    {
      return;
    }

  lv_obj_delete_async(sh->keys);
  lv_obj_delete_async(sh->box);
  lv_obj_delete_async(sh->shadow);
  memset(sh, 0, sizeof(*sh));
  if (st->keys != NULL)
    {
      lv_obj_remove_flag(st->keys, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The shadow under the sheet's frame, as large as the frame is once laid
 * out: a Busy's longer line grows the frame upwards
 */

static void app_sheet_shadow(FAR struct app_sheet_s *sh)
{
  lv_obj_update_layout(sh->box);
  lv_obj_set_pos(sh->shadow, lv_obj_get_x(sh->box) + APP_SHEET_SHADOW,
                 lv_obj_get_y(sh->box) + APP_SHEET_SHADOW);
  lv_obj_set_size(sh->shadow, lv_obj_get_width(sh->box),
                  lv_obj_get_height(sh->box));
}

/* A sheet over the screen (Cat-Confirm's frame): a Confirm's heading,
 * question and what it means; a Busy's parts.  One at a time: a new one
 * replaces the one open.
 */

static void app_sheet_open(FAR struct screen_s *s,
                           FAR const pnut_ui_show_request_t *in)
{
  FAR char *text = s->ui->text_long;
  FAR struct app_scr_s *st = app_state(s);
  FAR struct app_sheet_s *sh = &st->sheet;
  FAR const pnut_ui_confirm_t *c = &in->content.confirm;
  FAR lv_obj_t *label;

  app_sheet_close(s);
  ui_options_shut(s->ui, s);

  sh->screen = in->screen;
  sh->busy   = in->which_content == PNUT_UI_SHOW_REQUEST_BUSY_TAG;
  sh->cancel = sh->busy && in->content.busy.cancel;
  strlcpy(sh->confirm, !sh->busy && c->confirm[0] != '\0' ?
                       app_text(c->confirm, text, sizeof(sh->confirm)) :
                       UI_T_OK, sizeof(sh->confirm));

  sh->shadow = lv_obj_create(s->root);
  theme_box(sh->shadow);
  lv_obj_set_style_bg_color(sh->shadow, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(sh->shadow, LV_OPA_COVER, 0);

  sh->box = lv_obj_create(s->root);
  theme_box(sh->box);
  lv_obj_set_size(sh->box, APP_SHEET_W, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(sh->box, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(sh->box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(sh->box, 2, 0);
  lv_obj_set_style_border_color(sh->box, lv_color_black(), 0);
  lv_obj_set_style_pad_all(sh->box, APP_SHEET_PAD, 0);
  lv_obj_set_style_pad_bottom(sh->box, APP_SHEET_PAD + 2, 0);
  lv_obj_set_style_pad_row(sh->box, APP_SHEET_GAP, 0);
  lv_obj_set_flex_flow(sh->box, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(sh->box, LV_OBJ_FLAG_CLICKABLE);    /* Taps stop here */
  lv_obj_align(sh->box, LV_ALIGN_BOTTOM_LEFT, APP_SHEET_X, -APP_SHEET_FOOT);

  if (sh->busy)
    {
      tpl_busy_parts(s, &sh->parts, sh->box, &in->content.busy);
    }
  else
    {
      label = lv_label_create(sh->box);
      theme_label(label, &g_ui_mono_bold_10);
      lv_label_set_text(label, UI_T_CONFIRM_HEAD);

      label = lv_label_create(sh->box);
      theme_label(label, &g_ui_sans_bold_16);
      lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(label, lv_pct(100));
      lv_label_set_text(label, app_text(c->question, text, UI_TEXT_LONG));

      label = lv_label_create(sh->box);
      theme_label(label, &g_ui_sans_12);
      lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(label, lv_pct(100));
      lv_label_set_text(label, app_text(c->detail, text, UI_TEXT_LONG));
    }

  app_sheet_shadow(sh);

  if (st->keys != NULL)
    {
      lv_obj_add_flag(st->keys, LV_OBJ_FLAG_HIDDEN);
    }

  app_sheet_keys(s);
}

/* A key while the sheet is open: its own.  A Confirm is answered by its
 * left softkey, Enter or E, and cancelled by Cancel or Back; a Busy is
 * cancelled, when the app allows it; the sheet closes as it is answered.
 */

static void app_sheet_key(FAR struct screen_s *s, uint8_t action)
{
  FAR struct app_sheet_s *sh = &app_state(s)->sheet;
  uint32_t screen = sh->screen;
  bool cancel = action == UI_ACT_BACK || action == UI_ACT_SOFT_RIGHT;
  bool confirm = !sh->busy && (action == UI_ACT_OPEN ||
                               action == UI_ACT_SOFT_LEFT);

  if (confirm || (cancel && (!sh->busy || sh->cancel)))
    {
      app_sheet_close(s);
      app_input_on(s, screen, confirm ? PNUT_UI_INPUT_ACTION_CONFIRMED :
                                        PNUT_UI_INPUT_ACTION_CANCELLED,
                   0, 0);
    }
}

/* The screen from a Show that holds together */

static void app_build_from(FAR struct screen_s *s,
                           FAR const pnut_ui_show_request_t *in)
{
  FAR char *title = s->ui->texts[0];
  FAR struct app_scr_s *st = app_state(s);
  FAR struct host_app_s *app = host_app(&s->ui->host);

  st->screen = in->screen;
  st->tpl    = app_template(in);
  strlcpy(st->middle, in->middle, sizeof(st->middle));
  st->middle_off = in->middle_off;
  app_options(s, in);

  ui_header(s, app != NULL ? app_icon(app->manifest.icon) : NULL,
            app_text(in->title, title, UI_TEXT_MAX));
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
  uint32_t id = 0;
  int item;

  /* With the row or item focused, when the template has one */

  if (st->tpl == &g_tpl_list)
    {
      row = applist_focused(&st->t.list.list);
      id  = row != NULL ? row->id : 0;
    }
  else if (st->tpl == &g_tpl_detail && st->t.detail.list.focus != NULL)
    {
      item = block_id(st->t.detail.list.focus) - 1;
      id   = item >= 0 ? st->t.detail.ids[item] : 0;
    }

  app_input(s, PNUT_UI_INPUT_ACTION_OPTION_CHOSEN, id,
            st->option_ids[index]);
}

static bool app_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct app_scr_s *st = app_state(s);
  FAR const char *items[APP_OPTIONS];
  int i;

  if (st->sheet.screen != 0)
    {
      app_sheet_key(s, action);
      return true;
    }

  /* A Busy screen: Cancel or Back cancels, when the app allows it */

  if (st->tpl == &g_tpl_busy)
    {
      if (st->cancel &&
          (action == UI_ACT_BACK || action == UI_ACT_SOFT_RIGHT))
        {
          app_input(s, PNUT_UI_INPUT_ACTION_CANCELLED, 0, 0);
        }

      return true;
    }

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

  if (st->sheet.screen == 0 && st->tpl != NULL && st->tpl->event != NULL)
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
  app_input_on(s, app_state(s)->screen, action, row, option);
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

FAR const char *app_text(FAR const char *in, FAR char *out, size_t size)
{
  describe_name(in, out, size);
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

  /* A sheet: a Confirm or a Busy over the app's screen, with an id of its
   * own
   */

  if (in->sheet)
    {
      switch (in->which_content)
        {
          case PNUT_UI_SHOW_REQUEST_CONFIRM_TAG:
            break;

          case PNUT_UI_SHOW_REQUEST_BUSY_TAG:
            if (g_tpl_busy.check(in) < 0)
              {
                return -EINVAL;
              }

            break;

          default:
            return -EINVAL;       /* Not a sheet's template */
        }

      s = app_screen(ui);
      if (in->screen == 0 || s == NULL ||
          app_state(s)->screen == in->screen)
        {
          return -EINVAL;
        }

      app_sheet_open(s, in);
      ui_place(ui);
      return 0;
    }

  if (tpl == NULL || in->screen == 0 || tpl->check(in) < 0)
    {
      return -EINVAL;
    }

  /* The first screen goes on top, or in place of the UI's Busy while
   * the app opened; beneath the SIM PIN, which is the system's (RFC 0037,
   * layer 5)
   */

  s = app_screen(ui);
  if (s == NULL)
    {
      s = app_opening_screen(ui, host_app(&ui->host));
    }

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
  FAR struct screen_s *s = app_screen(ui);
  FAR struct app_scr_s *st;

  if (s == NULL)
    {
      return 0;
    }

  /* The sheet's: a Busy's text, progress and cancel */

  st = app_state(s);
  if (st->sheet.screen != 0 && st->sheet.screen == in->screen)
    {
      if (in->which_change != PNUT_UI_UPDATE_REQUEST_BUSY_TAG ||
          !st->sheet.busy ||
          (in->change.busy.has_progress && in->change.busy.progress > 100))
        {
          return -EINVAL;
        }

      tpl_busy_set(s, &st->sheet.parts, &in->change.busy);
      st->sheet.cancel = in->change.busy.cancel;
      app_sheet_shadow(&st->sheet);
      app_sheet_keys(s);
      ui_place(ui);
      return 0;
    }

  if (st->screen != in->screen)
    {
      return 0;                   /* Another screen's: dropped */
    }

  switch (in->which_change)
    {
      case PNUT_UI_UPDATE_REQUEST_TITLE_TAG:
        block_header_title(s->header,
                           app_text(in->change.title, ui->texts[0],
                                    UI_TEXT_MAX));
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

        if (st->tpl == &g_tpl_busy)
          {
            app_softkeys(s);      /* Cancel may come or go */
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

void app_launch(FAR struct ui_s *ui, FAR const struct ui_app_s *a)
{
  int ret;

  if (a->settings)
    {
      ui_push(ui, &g_scr_settings, a);
    }
  else if (a->id == NULL)
    {
      ui_push(ui, &g_scr_unavailable, a);
    }
  else
    {
      /* With no room, or while the app stops, the press does nothing;
       * with no manifest, the app is not there; disabled, the owner is
       * asked to try again
       */

      ret = host_start(&ui->host, a->id);
      if (ret == -ENOENT || ret == -EINVAL)
        {
          ui_push(ui, &g_scr_unavailable, a);
        }
      else if (ret == -EACCES)
        {
          ui_push(ui, &g_scr_stopped, a);
        }
    }
}

void app_gone(FAR struct ui_s *ui)
{
  FAR struct screen_s *s = app_screen(ui);

  if (s != NULL)
    {
      ui_remove(ui, s);
    }
}
