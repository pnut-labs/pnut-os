/****************************************************************************
 * pnut-os/src/ui/tpl_list.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's List (RFC 0037, the canvas's Cat-List): pages of six rows, the
 * focus inverted, the page in the header; rows pulled from the app ahead
 * of the focus (applist.c).  The rows of a page are six blocks, filled
 * again as the page turns.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include "apptpl.h"
#include "blocks.h"
#include "keymap.h"
#include "text.h"
#include "theme.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct tpl_list_s *tpl_list_of(FAR struct screen_s *s)
{
  return &((FAR struct app_scr_s *)s->state)->t.list;
}

static int tpl_list_trailer(int trailer)
{
  switch (trailer)
    {
      case PNUT_UI_ROW_TRAILER_CHEVRON:
        return BLOCK_TRAILER_CHEVRON;

      case PNUT_UI_ROW_TRAILER_ON:
        return BLOCK_TRAILER_ON;

      case PNUT_UI_ROW_TRAILER_OFF:
        return BLOCK_TRAILER_OFF;

      default:
        return BLOCK_TRAILER_NONE;
    }
}

/* A lead of initials, cut to its first two characters (UTF-8) */

static FAR const char *tpl_list_initials(FAR const char *in, FAR char *out,
                                         size_t size)
{
  size_t i = 0;
  int chars = 0;

  while (in[i] != '\0' && i + 1 < size)
    {
      if (((uint8_t)in[i] & 0xc0) != 0x80 && ++chars > 2)
        {
          break;
        }

      out[i] = in[i];
      i++;
    }

  out[i] = '\0';
  return out;
}

/* A row's block from the app's row: every text as drawn */

static void tpl_list_fill(FAR struct screen_s *s, FAR lv_obj_t *block,
                          FAR const pnut_ui_row_t *row)
{
  char two[sizeof(row->lead)];
  FAR char *lead = s->ui->texts[0];
  FAR char *title = s->ui->texts[1];
  FAR char *detail = s->ui->texts[2];
  FAR char *value = s->ui->texts[3];

  block_app_row_set(block,
                    row->initials ?
                    app_text(tpl_list_initials(row->lead, two, sizeof(two)),
                             lead, UI_TEXT_MAX) :
                    app_icon(row->lead),
                    row->initials,
                    app_text(row->title, title, UI_TEXT_MAX),
                    app_text(row->detail, detail, UI_TEXT_MAX),
                    row->which_value == PNUT_UI_ROW_TEXT_TAG ?
                    app_text(row->value.text, value, UI_TEXT_MAX) : NULL,
                    row->unread, tpl_list_trailer(row->trailer));
}

/* The page of the focus, and the header's page; a page asked for when
 * the focus nears the end of the rows loaded
 */

static void tpl_list_draw(FAR struct screen_s *s)
{
  FAR struct tpl_list_s *t = tpl_list_of(s);
  FAR struct applist_s *l = &t->list;
  uint16_t first = applist_page(l) * APPLIST_PER_PAGE;
  uint32_t ask;
  int i;

  for (i = 0; i < APPLIST_PER_PAGE; i++)
    {
      if (first + i < l->loaded)
        {
          tpl_list_fill(s, t->rows[i], &l->rows[first + i]);
          block_focus(t->rows[i], first + i == l->focus);
          lv_obj_remove_flag(t->rows[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          block_focus(t->rows[i], false);
          lv_obj_add_flag(t->rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  block_header_page(s->header, applist_page(l), applist_npages(l));
  s->focus = l->loaded > 0 ? l->focus : 0;

  if (applist_need(l, &ask))
    {
      app_page_needed(s, ask);
    }
}

static void tpl_list_open(FAR struct screen_s *s)
{
  FAR const pnut_ui_row_t *row = applist_focused(&tpl_list_of(s)->list);

  if (row != NULL && app_primary_on(s))
    {
      app_input(s, PNUT_UI_INPUT_ACTION_ROW_OPENED, row->id, 0);
    }
}

static int tpl_list_check(FAR const pnut_ui_show_request_t *in)
{
  return applist_check(in->content.list.count, in->content.list.rows,
                       in->content.list.rows_count, in->focus);
}

static int tpl_list_build(FAR struct screen_s *s,
                          FAR const pnut_ui_show_request_t *in)
{
  FAR struct tpl_list_s *t = tpl_list_of(s);
  FAR lv_obj_t *box;
  int ret;
  int i;

  ret = applist_init(&t->list, in->content.list.count,
                     in->content.list.rows, in->content.list.rows_count,
                     in->focus);
  if (ret < 0)
    {
      return ret;
    }

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(box, 240, APPLIST_PER_PAGE * BLOCK_ROW_H);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < APPLIST_PER_PAGE; i++)
    {
      t->rows[i] = block_app_row(box, i + 1);
    }

  tpl_list_draw(s);
  return 0;
}

static bool tpl_list_key(FAR struct screen_s *s, uint8_t action)
{
  FAR struct applist_s *l = &tpl_list_of(s)->list;

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        if (applist_move(l, action == UI_ACT_DOWN ? 1 : -1))
          {
            tpl_list_draw(s);
          }

        return true;

      case UI_ACT_PAGE_NEXT:
      case UI_ACT_PAGE_PREV:
        if (applist_page_move(l, action == UI_ACT_PAGE_NEXT ? 1 : -1))
          {
            tpl_list_draw(s);
          }

        return true;

      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        tpl_list_open(s);
        return true;

      default:
        return false;
    }
}

/* A tap on a row of the page: focused, and opened */

static void tpl_list_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct applist_s *l = &tpl_list_of(s)->list;
  uint16_t index;

  if (id < 1 || id > APPLIST_PER_PAGE)
    {
      return;
    }

  index = applist_page(l) * APPLIST_PER_PAGE + id - 1;
  if (index < l->loaded)
    {
      l->focus = index;
      tpl_list_draw(s);
      tpl_list_open(s);
    }
}

static int tpl_list_update(FAR struct screen_s *s,
                           FAR const pnut_ui_update_request_t *in)
{
  FAR struct applist_s *l = &tpl_list_of(s)->list;

  if (in->which_change != PNUT_UI_UPDATE_REQUEST_ROW_TAG)
    {
      return -EINVAL;
    }

  if (in->change.row.id != 0 && applist_update(l, &in->change.row) >= 0)
    {
      tpl_list_draw(s);
    }

  return 0;
}

static void tpl_list_rows(FAR struct screen_s *s,
                          FAR const pnut_ui_rows_request_t *in)
{
  FAR struct applist_s *l = &tpl_list_of(s)->list;

  if (applist_rows(l, in->first, in->rows, in->rows_count) == 0)
    {
      tpl_list_draw(s);
    }
}

static void tpl_list_leave(FAR struct screen_s *s)
{
  applist_free(&tpl_list_of(s)->list);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct app_tpl_s g_tpl_list =
{
  tpl_list_check,
  tpl_list_build,
  tpl_list_key,
  tpl_list_event,
  tpl_list_update,
  tpl_list_rows,
  tpl_list_leave,
  UI_T_OPEN
};
