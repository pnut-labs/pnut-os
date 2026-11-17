/****************************************************************************
 * pnut-os/tests/unit/fake_app.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* A fake app and a fake Service states, on one loop with the ui module,
 * which draws into a frame in memory (RFC 0037).  The app is the test's
 * own task, so Who is asked about this process's pid.  For one test
 * program: its functions are static, its state global.  A test sets
 * g_who and g_named, writes a manifest, and runs steps at times.
 */

#ifndef __PNUT_OS_TESTS_UNIT_FAKE_APP_H
#define __PNUT_OS_TESTS_UNIT_FAKE_APP_H

/* Each test uses some of the helpers below */

#pragma GCC diagnostic ignored "-Wunused-function"

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <pb_encode.h>

#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/manifest.pb.h"
#include "pnut/services.pnut.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/ui.pnut.h"

#include "host.h"
#include "input.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FAKE_WIDTH    240
#define FAKE_HEIGHT   320
#define FAKE_STRIDE   (FAKE_WIDTH / 8)
#define FAKE_EVENTS   64
#define FAKE_NOTES    "org.example.notes"

/* The time the screens show: 2026-09-22 12:07:00 in Warsaw */

#define FAKE_NOW      1790078820
#define FAKE_RULE     "CET-1CEST,M3.5.0,M10.5.0/3"

/* Where the images go: tests/unit/Makefile names its build directory's
 * screens/
 */

#ifndef SCREENS_DIR
#  define SCREENS_DIR  "screens"
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct step_s
{
  uint32_t at;
  CODE void (*run)(void);
};

/* What Service states answers Who */

enum who_e
{
  WHO_NAMED,
  WHO_SHELL,
  WHO_NOTFOUND,
  WHO_SILENT
};

/* A fake app's connection, and what it saw */

struct app_s
{
  struct pnut_ui_client_s client;
  bool connected;
  bool up;
  uint16_t stream;
  int opened;                     /* Open's status, or -1 before */
  uint32_t code;                  /* The last error's UiError code */
  int ended;                      /* The stream's end status, or -1 */
  pnut_ui_event_t events[FAKE_EVENTS];
  int nevents;
  int replies;
  int status;                     /* The last answer's */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

static uint8_t g_frame[FAKE_STRIDE * FAKE_HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static FAR struct pnut_loop_s *g_loop;
static struct pnut_services_server_s g_services;
static FAR struct pnut_publisher_s *g_time;
static FAR struct pnut_publisher_s *g_tel;
static enum who_e g_who;
static char g_named[64];
static int g_whos;

/* Service states' Start and Stop, as the system UI asked them: how many,
 * and the last name each
 */

static int g_starts;
static int g_stops;
static char g_started[64];
static char g_stopped[64];

/* The first Start fails after this many ms (0: it succeeds at once), as
 * Service states answers when NxInit does; the name every manifest gives
 */

static int g_start_fail_first;
static struct pnut_request_s g_start_req;
static FAR const char *g_manifest_name;

/* The apps' timings the UI runs with; all 0: the defaults */

static struct ui_times_s g_times;
static char g_dir[] = "/tmp/pnut-app-XXXXXX";
static char g_apps[64];
static struct app_s g_app;
static struct app_s g_app2;
static time_t g_now = FAKE_NOW;

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static int fake_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                       int16_t h, bool full)
{
  return 0;
}

static const struct display_ops_s g_fake_ops =
{
  fake_update
};

static time_t fake_now(void)
{
  return g_now;
}

/* Service states' Who, as the test sets it */

static void fake_who(FAR struct pnut_services_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_services_who_request_t *in,
                     FAR void *arg)
{
  pnut_services_who_reply_t out;

  g_whos++;
  assert_int_equal(in->pid, getpid());
  memset(&out, 0, sizeof(out));
  switch (g_who)
    {
      case WHO_NAMED:
        strlcpy(out.name, g_named, sizeof(out.name));
        pnut_services_who_reply(server, req, PNUT_STATUS_OK, &out);
        break;

      case WHO_SHELL:
        out.shell = true;
        pnut_services_who_reply(server, req, PNUT_STATUS_OK, &out);
        break;

      case WHO_NOTFOUND:
        pnut_services_fail(server, req, PNUT_STATUS_NOTFOUND,
                           PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE,
                           "no such task");
        break;

      default:
        break;                    /* Never answered */
    }
}

static void fake_start_fail(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_services_fail(&g_services, &g_start_req, PNUT_STATUS_UNAVAILABLE,
                     PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE,
                     "late failure");
}

static void fake_start(FAR struct pnut_services_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_services_name_request_t *in,
                       FAR void *arg)
{
  pnut_services_reply_t out;

  g_starts++;
  strlcpy(g_started, in->name, sizeof(g_started));
  if (g_start_fail_first > 0 && g_starts == 1)
    {
      g_start_req = *req;
      assert_int_equal(pnut_timer_start(g_loop, g_start_fail_first, 0,
                                        fake_start_fail, NULL, NULL), 0);
      return;
    }
  memset(&out, 0, sizeof(out));
  pnut_services_start_reply(server, req, PNUT_STATUS_OK, &out);
}

static void fake_stop(FAR struct pnut_services_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_services_name_request_t *in,
                      FAR void *arg)
{
  pnut_services_reply_t out;

  g_stops++;
  strlcpy(g_stopped, in->name, sizeof(g_stopped));
  memset(&out, 0, sizeof(out));
  pnut_services_stop_reply(server, req, PNUT_STATUS_OK, &out);
}

static const struct pnut_services_handlers_s g_fake_services =
{
  .start = fake_start,
  .stop  = fake_stop,
  .who   = fake_who
};

/* A manifest in the apps' folder */

static void write_manifest(FAR const char *id, FAR const char *icon,
                           int ntemplates, FAR const int *templates)
{
  pnut_app_manifest_t m;
  uint8_t buf[256];
  pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));
  char path[160];
  FILE *f;
  int i;

  memset(&m, 0, sizeof(m));
  strlcpy(m.icon, icon, sizeof(m.icon));
  strlcpy(m.name, g_manifest_name, sizeof(m.name));
  m.templates_count = ntemplates;
  for (i = 0; i < ntemplates; i++)
    {
      m.templates[i] = templates[i];
    }

  assert_true(pb_encode(&out, PNUT_APP_MANIFEST_FIELDS, &m));
  snprintf(path, sizeof(path), "%s/%s.manifest", g_apps, id);
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(buf, 1, out.bytes_written, f),
                   out.bytes_written);
  fclose(f);
}

/* The fake app */

static void app_state_changed(FAR struct pnut_ui_client_s *client,
                              bool connected, FAR void *arg)
{
  FAR struct app_s *app = arg;

  app->connected = connected;
}

static void app_opened(FAR struct pnut_ui_client_s *client, int status,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct app_s *app = arg;

  app->opened = status;
  app->code = err != NULL ? err->code : 0;
  app->up = status == PNUT_STATUS_OK;
}

static void app_item(FAR struct pnut_ui_client_s *client, int status,
                     FAR const pnut_ui_event_t *item, FAR void *arg)
{
  FAR struct app_s *app = arg;

  if (item == NULL)
    {
      app->ended = status;
      app->up = false;
      return;
    }

  assert_true(app->nevents < FAKE_EVENTS);
  app->events[app->nevents++] = *item;
}

static void app_done(FAR struct pnut_ui_client_s *client, int status,
                     FAR const pnut_ui_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct app_s *app = arg;

  app->replies++;
  app->status = status;
  app->code = err != NULL ? err->code : 0;
}

/* An app connected, as its task would be once started: one Service
 * states names is started first, as Menu starts it
 */

static void app_connect_unstarted(FAR struct app_s *app)
{
  memset(app, 0, sizeof(*app));
  app->opened = -1;
  app->ended  = -1;
  assert_int_equal(pnut_ui_connect(g_loop, HOST_SERVICE, app_state_changed,
                                   app, &app->client), 0);
}

static void app_connect(FAR struct app_s *app)
{
  if (g_who == WHO_NAMED)
    {
      host_start(&g_ui.host, g_named);
    }

  app_connect_unstarted(app);
}

static void app_open(FAR struct app_s *app, FAR const char *id,
                     uint16_t credit)
{
  pnut_ui_open_request_t in;

  memset(&in, 0, sizeof(in));
  if (id != NULL)
    {
      strlcpy(in.app, id, sizeof(in.app));
    }

  in.level = 1;
  assert_int_equal(pnut_ui_open(&app->client, &in, credit, 3000,
                                app_opened, app_item, app, &app->stream),
                   0);
}

static void app_send_show(FAR struct app_s *app,
                          FAR const pnut_ui_show_request_t *in)
{
  assert_int_equal(pnut_ui_show(&app->client, in, 3000, app_done, app), 0);
}

static void app_send_update(FAR struct app_s *app,
                            FAR const pnut_ui_update_request_t *in)
{
  assert_int_equal(pnut_ui_update(&app->client, in, 3000, app_done, app),
                   0);
}

static void app_send_rows(FAR struct app_s *app,
                          FAR const pnut_ui_rows_request_t *in)
{
  assert_int_equal(pnut_ui_rows(&app->client, in, 3000, app_done, app), 0);
}

static void app_leave(FAR struct app_s *app)
{
  pnut_ui_leave_request_t in;

  memset(&in, 0, sizeof(in));
  assert_int_equal(pnut_ui_leave(&app->client, &in, 3000, app_done, app),
                   0);
}

/* The events the app saw since index from, of an input's action: how
 * many, and the last one's
 */

static int app_inputs(FAR const struct app_s *app, int from, int action,
                      FAR const pnut_ui_input_t **last)
{
  int n = 0;
  int i;

  *last = NULL;
  for (i = from; i < app->nevents; i++)
    {
      if (app->events[i].which_kind == PNUT_UI_EVENT_INPUT_TAG &&
          app->events[i].kind.input.action == action)
        {
          *last = &app->events[i].kind.input;
          n++;
        }
    }

  return n;
}

/* Keys and taps, as Input gives them */

static void key(uint32_t code, bool special)
{
  input_key(&g_input, code, special, true);
  input_key(&g_input, code, special, false);
}

static void tap(int16_t x, int16_t y)
{
  input_touch(&g_input, x, y, true);
  input_touch(&g_input, x, y, false);
}

/* The frame as a PBM picture (P4: a set bit black, the panel's white) */

static void save(FAR const char *name)
{
  char path[128];
  FILE *f;
  size_t i;

  display_draw_now(&g_ui.display);
  mkdir(SCREENS_DIR, 0755);
  snprintf(path, sizeof(path), "%s/%s.pbm", SCREENS_DIR, name);
  f = fopen(path, "wb");
  assert_non_null(f);
  fprintf(f, "P4\n%d %d\n", FAKE_WIDTH, FAKE_HEIGHT);
  for (i = 0; i < sizeof(g_frame); i++)
    {
      fputc((uint8_t)~g_frame[i], f);
    }

  fclose(f);
}

static void step_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR const struct step_s *step = arg;

  if (step->run != NULL)
    {
      step->run();
    }
  else
    {
      pnut_loop_stop(loop, 0);
    }
}

/* Warsaw's time, for the header */

static void publish_time(void)
{
  pnut_time_change_t time;

  memset(&time, 0, sizeof(time));
  strcpy(time.zone, "Europe/Warsaw");
  strcpy(time.rule, FAKE_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &time), 0);
}

/* The modem on, and the SIM in a state (a PIN wanted raises the SIM PIN
 * screen)
 */

static void publish_sim(int sim)
{
  pnut_telephony_info_t tel;

  memset(&tel, 0, sizeof(tel));
  tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.sim   = sim;
  tel.tries = 3;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);
}

/* Run the steps with the ui module, Service states' fake and the apps'
 * folder, Who answering as set
 */

static void run(FAR const struct step_s *steps, int nsteps)
{
  struct pnut_loop_config_s lconfig;
  struct display_config_s screen;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  int i;

  pnut_loop_defaults(&lconfig);
  lconfig.rundir = g_dir;
  lconfig.timers = 64;            /* Each step is one, beside the UI's */
  assert_int_equal(pnut_loop_create(&lconfig, &g_loop), 0);
  assert_int_equal(pnut_time_change_advertise(g_loop, &g_time), 0);
  assert_int_equal(pnut_telephony_info_advertise(g_loop, &g_tel), 0);
  publish_time();
  assert_int_equal(pnut_services_serve(g_loop, "services", 0,
                                       &g_fake_services, NULL,
                                       &g_services), 0);

  memset(g_frame, 0xff, sizeof(g_frame));
  memset(&screen, 0, sizeof(screen));
  screen.frame  = g_frame;
  screen.stride = FAKE_STRIDE;
  screen.width  = FAKE_WIDTH;
  screen.height = FAKE_HEIGHT;
  screen.ops    = &g_fake_ops;

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &g_ui.display;
  input_init(&g_input, &iconfig);

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.screen   = &screen;
  uconfig.input    = &g_input;
  uconfig.now      = fake_now;
  uconfig.apps     = g_apps;
  uconfig.services = "services";
  uconfig.times    = g_times;
  ui_init(&g_ui, &uconfig);
  assert_int_equal(pnut_module_add(g_loop, &g_ui.module), 0);
  assert_int_equal(pnut_module_add(g_loop, &g_input.module), 0);

  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(g_loop, steps[i].at, 0,
                                        step_timer, (FAR void *)&steps[i],
                                        NULL), 0);
    }

  assert_int_equal(pnut_loop_run(g_loop), 0);
  pnut_ui_disconnect(&g_app.client);
  pnut_ui_disconnect(&g_app2.client);
  pnut_services_close(&g_services);
  pnut_topic_unadvertise(g_time);
  pnut_topic_unadvertise(g_tel);
  pnut_loop_destroy(g_loop);
}

static int fake_setup(void **state)
{
  strcpy(g_dir, "/tmp/pnut-app-XXXXXX");
  assert_non_null(mkdtemp(g_dir));
  snprintf(g_apps, sizeof(g_apps), "%s/apps", g_dir);
  assert_int_equal(mkdir(g_apps, 0755), 0);
  g_who = WHO_NAMED;
  strcpy(g_named, FAKE_NOTES);
  g_whos = 0;
  g_starts = 0;
  g_stops = 0;
  g_started[0] = '\0';
  g_stopped[0] = '\0';
  memset(&g_times, 0, sizeof(g_times));
  g_start_fail_first = 0;
  g_manifest_name = "Notes";
  memset(&g_app, 0, sizeof(g_app));
  memset(&g_app2, 0, sizeof(g_app2));
  return 0;
}

static int fake_teardown(void **state)
{
  char cmd[64];

  snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
  assert_int_equal(system(cmd), 0);
  return 0;
}

#endif /* __PNUT_OS_TESTS_UNIT_FAKE_APP_H */
