/****************************************************************************
 * pnut-os/src/ui/tpl_detail.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's Detail (RFC 0037, the canvas's Cat-Detail): items of a label in
 * capitals over a value, in a column that scrolls; those the app numbers
 * take the focus, and the primary action opens the one focused, its icon
 * at its right saying what opening does.  With none focused it does
 * nothing, as in a List.  Where no item further takes the focus, W and S
 * scroll a row.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>

#include "apptpl.h"
#include "blocks.h"
#include "keymap.h"
#include "text.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct tpl_detail_s *tpl_detail_of(FAR struct screen_s *s)
{
  return &((FAR struct app_scr_s *)s->state)->t.detail;
}

/* The item focused: its index, or -1 */

static int tpl_detail_focused(FAR struct screen_s *s)
{
  FAR struct tpl_detail_s *t = tpl_detail_of(s);

  return t->list.focus != NULL ? block_id(t->list.focus) - 1 : -1;
}

static void tpl_detail_open(FAR struct screen_s *s)
{
  FAR struct tpl_detail_s *t = tpl_detail_of(s);
  int i = tpl_detail_focused(s);

  if (i >= 0 && app_primary_on(s))
    {
      app_input(s, PNUT_UI_INPUT_ACTION_ROW_OPENED, t->ids[i], 0);
    }
}

static int tpl_detail_check(FAR const pnut_ui_show_request_t *in)
{
  FAR const pnut_ui_detail_t *d = &in->content.detail;
  bool found = in->focus == 0;
  int i;
  int j;

  for (i = 0; i < d->items_count; i++)
    {
      for (j = 0; d->items[i].id != 0 && j < i; j++)
        {
          if (d->items[j].id == d->items[i].id)
            {
              return -EINVAL;     /* Two items of one id */
            }
        }

      found = found || (in->focus != 0 && d->items[i].id == in->focus);
    }

  return found ? 0 : -EINVAL;
}

static int tpl_detail_build(FAR struct screen_s *s,
                            FAR const pnut_ui_show_request_t *in)
{
  FAR char *label = s->ui->texts[0];
  FAR char *value = s->ui->texts[1];
  FAR struct tpl_detail_s *t = tpl_detail_of(s);
  FAR const pnut_ui_detail_t *d = &in->content.detail;
  FAR const pnut_ui_item_t *item;
  FAR lv_obj_t *row;
  int i;

  list_init(&t->list, s->root);
  for (i = 0; i < d->items_count; i++)
    {
      item = &d->items[i];
      t->ids[i] = item->id;
      row = block_item(t->list.box, item->id != 0 ? i + 1 : 0,
                       app_text(item->label, label, UI_TEXT_MAX),
                       item->which_value == PNUT_UI_ITEM_TEXT_TAG ?
                       app_text(item->value.text, value, UI_TEXT_MAX) : "",
                       item->id != 0 ? app_icon(item->icon) : NULL);
      if (in->focus != 0 && item->id == in->focus)
        {
          list_focus(&t->list, row);
        }
    }

  if (t->list.focus == NULL)
    {
      list_move(&t->list, 1);
    }

  s->focus = tpl_detail_focused(s) + 1;
  return 0;
}

static bool tpl_detail_key(FAR struct screen_s *s, uint8_t action)
{
  FAR struct tpl_detail_s *t = tpl_detail_of(s);

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        if (!list_move(&t->list, action == UI_ACT_DOWN ? 1 : -1))
          {
            /* Items that take no focus beyond: a row more of them */

            lv_obj_scroll_by_bounded(t->list.box, 0,
                                     action == UI_ACT_DOWN ? -BLOCK_ROW_H :
                                     BLOCK_ROW_H, LV_ANIM_OFF);
          }

        s->focus = tpl_detail_focused(s) + 1;
        return true;

      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        tpl_detail_open(s);
        return true;

      default:
        return false;
    }
}

/* A tap on an item that takes the focus: focused, and opened */

static void tpl_detail_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct tpl_detail_s *t = tpl_detail_of(s);
  FAR lv_obj_t *row = list_find(&t->list, id);

  if (row != NULL)
    {
      list_focus(&t->list, row);
      s->focus = id;
      tpl_detail_open(s);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct app_tpl_s g_tpl_detail =
{
  tpl_detail_check,
  tpl_detail_build,
  tpl_detail_key,
  tpl_detail_event,
  NULL,
  NULL,
  NULL,
  UI_T_OPEN
};
