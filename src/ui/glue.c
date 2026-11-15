/****************************************************************************
 * pnut-os/src/ui/glue.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* LVGL on the program's event loop (RFC 0019): no thread of its own.  Its
 * timers run from one timer of the loop, set to when LVGL says the next is
 * due, or at once when LVGL makes or resumes one.  The screen's refresh is
 * not among them: display.c draws when the redraw rule says.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include <pnut/timer.h>

#include "glue.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* LVGL is one per program: so is its glue */

static FAR struct pnut_loop_s *g_loop;
static FAR struct pnut_timer_s *g_timer;
static bool g_soon;               /* g_timer runs the timers at once */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t glue_tick(void)
{
  return (uint32_t)glue_now();
}

static void glue_arm(uint32_t delay);

static void glue_run(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer, FAR void *arg)
{
  uint32_t next;

  g_timer = NULL;
  g_soon  = false;
  next = lv_timer_handler();

  /* A timer made or resumed meanwhile has asked for another run at once */

  if (!g_soon && next != LV_NO_TIMER_READY)
    {
      glue_arm(next);
    }
}

static void glue_arm(uint32_t delay)
{
  if (g_timer != NULL)
    {
      pnut_timer_cancel(g_loop, g_timer);
      g_timer = NULL;
    }

  /* No timer to spare: the run is lost, not the next kick */

  g_soon = pnut_timer_start(g_loop, delay, 0, glue_run, NULL,
                            &g_timer) == 0 && delay == 0;
}

/* LVGL made or resumed a timer: its timers run soon, once */

static void glue_resumed(FAR void *data)
{
  glue_kick();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint64_t glue_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int glue_start(FAR struct pnut_loop_s *loop)
{
  if (g_loop != NULL)
    {
      return -EBUSY;
    }

  g_loop = loop;
  lv_init();
  lv_tick_set_cb(glue_tick);
  lv_timer_handler_set_resume_cb(glue_resumed, NULL);
  glue_kick();
  return 0;
}

void glue_stop(void)
{
  if (g_loop == NULL)
    {
      return;
    }

  if (g_timer != NULL)
    {
      pnut_timer_cancel(g_loop, g_timer);
      g_timer = NULL;
    }

  lv_timer_handler_set_resume_cb(NULL, NULL);
  lv_deinit();
  g_soon = false;
  g_loop = NULL;
}

void glue_kick(void)
{
  if (g_loop != NULL && !g_soon)
    {
      glue_arm(0);
    }
}
