/****************************************************************************
 * pnut-os/src/commands/settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* settings: the settings from the shell (RFC 0025), for developers.  A
 * command typed in the developer's shell may do what the system UI may
 * (RFC 0012): read and change every setting, but never learn a secret's
 * value, nor declare an owner's schema.  An owner's settings exist once
 * the owner has registered them.
 *
 *   settings list <owner>                 every key and its value
 *   settings get <owner> <key>            one value
 *   settings set <owner> <key> <value>    as its type takes it
 *   settings reset <owner> [<key>]        one, or all, to the default
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/settings.pnut.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Settings' service, and how long the command waits for it, all its
 * answers together, in milliseconds
 */

#define SETTINGS_SERVICE  "settings"
#define SETTINGS_WAIT_MS  5000

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum settings_op_e
{
  SETTINGS_LIST = 0,
  SETTINGS_GET,
  SETTINGS_SET,
  SETTINGS_RESET
};

struct settings_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_settings_client_s client;
  bool open;                      /* The client is */
  bool asked;                     /* The first question went out */
  FAR struct pnut_timer_s *timer; /* Until the command gives up */
  uint8_t op;                     /* enum settings_op_e */
  char owner[sizeof(((FAR pnut_settings_get_request_t *)0)->owner)];
  char key[sizeof(((FAR pnut_settings_get_request_t *)0)->key)];
  FAR const char *value;          /* What set gives, as typed */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void settings_listed(FAR struct pnut_settings_client_s *client,
                            int status,
                            FAR const pnut_settings_values_t *out,
                            FAR const pnut_error_t *err, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* PNUT_STATUS_'s, for an answer without an error's text */

static FAR const char *const g_settings_status[] =
{
  "done",
  "busy, try again",
  "not allowed",
  "not found",
  "invalid",
  "unavailable",
  "no answer",
  "failed"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: settings_usage
 ****************************************************************************/

static void settings_usage(void)
{
  fprintf(stderr,
          "usage: settings list <owner>\n"
          "       settings get <owner> <key>\n"
          "       settings set <owner> <key> <value>\n"
          "       settings reset <owner> [<key>]\n");
}

/****************************************************************************
 * Name: settings_end
 *
 * Description:
 *   The command is done: zero, or a failure.
 *
 ****************************************************************************/

static void settings_end(FAR struct settings_cmd_s *cmd, int status)
{
  pnut_loop_stop(cmd->module.loop, status);
}

/****************************************************************************
 * Name: settings_failed
 *
 * Description:
 *   An answer other than done: Settings' reason, or the status's.
 *
 ****************************************************************************/

static void settings_failed(FAR struct settings_cmd_s *cmd, int status,
                            FAR const pnut_error_t *err)
{
  if (err != NULL && err->text[0] != '\0')
    {
      fprintf(stderr, "settings: %s\n", err->text);
    }
  else if (status >= 0 && status < (int)(sizeof(g_settings_status) /
                                         sizeof(g_settings_status[0])))
    {
      fprintf(stderr, "settings: %s\n", g_settings_status[status]);
    }
  else
    {
      fprintf(stderr, "settings: failed: status %d\n", status);
    }

  settings_end(cmd, -EIO);
}

/****************************************************************************
 * Name: settings_print
 *
 * Description:
 *   A value, after its key if one is given.  A secret's is never given.
 *
 ****************************************************************************/

static void settings_print(FAR const char *key, bool has,
                           FAR const pnut_setting_value_t *value)
{
  if (key != NULL)
    {
      printf("%s = ", key);
    }

  if (!has)
    {
      printf("(secret)\n");
      return;
    }

  switch (value->which_value)
    {
      case PNUT_SETTING_VALUE_BOOLEAN_TAG:
        printf("%s\n", value->value.boolean ? "true" : "false");
        break;

      case PNUT_SETTING_VALUE_INTEGER_TAG:
        printf("%" PRId64 "\n", value->value.integer);
        break;

      case PNUT_SETTING_VALUE_TEXT_TAG:
        printf("%s\n", value->value.text);
        break;

      default:
        printf("(secret)\n");
        break;
    }
}

/****************************************************************************
 * Name: settings_parse
 *
 * Description:
 *   A value as typed, of the type the setting's present value has: a
 *   boolean, an integer, or text (a string, or a choice's name).  Text
 *   when the type is not known: a secret's value is never given.
 *
 ****************************************************************************/

static int settings_parse(FAR const char *text, pb_size_t which,
                          FAR pnut_setting_value_t *value)
{
  FAR char *end;

  memset(value, 0, sizeof(*value));

  switch (which)
    {
      case PNUT_SETTING_VALUE_BOOLEAN_TAG:
        if (strcmp(text, "true") == 0 || strcmp(text, "on") == 0 ||
            strcmp(text, "yes") == 0 || strcmp(text, "1") == 0)
          {
            value->value.boolean = true;
          }
        else if (strcmp(text, "false") != 0 && strcmp(text, "off") != 0 &&
                 strcmp(text, "no") != 0 && strcmp(text, "0") != 0)
          {
            fprintf(stderr, "settings: a boolean is true or false, "
                    "not \"%s\"\n", text);
            return -EINVAL;
          }

        value->which_value = PNUT_SETTING_VALUE_BOOLEAN_TAG;
        return OK;

      case PNUT_SETTING_VALUE_INTEGER_TAG:
        errno = 0;
        value->value.integer = strtoll(text, &end, 10);
        if (end == text || *end != '\0' || errno != 0)
          {
            fprintf(stderr, "settings: not an integer: \"%s\"\n", text);
            return -EINVAL;
          }

        value->which_value = PNUT_SETTING_VALUE_INTEGER_TAG;
        return OK;

      default:
        if (strlen(text) >= sizeof(value->value.text))
          {
            fprintf(stderr, "settings: longer than %zu bytes\n",
                    sizeof(value->value.text) - 1);
            return -E2BIG;
          }

        strlcpy(value->value.text, text, sizeof(value->value.text));
        value->which_value = PNUT_SETTING_VALUE_TEXT_TAG;
        return OK;
    }
}

/****************************************************************************
 * Name: settings_done
 *
 * Description:
 *   The answer to set or reset.
 *
 ****************************************************************************/

static void settings_done(FAR struct pnut_settings_client_s *client,
                          int status, FAR const pnut_settings_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct settings_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      settings_failed(cmd, status, err);
      return;
    }

  settings_end(cmd, OK);
}

/****************************************************************************
 * Name: settings_got
 *
 * Description:
 *   The setting's value: printed for get; for set, its type, which the new
 *   value is read as.  A secret's is never given, and its new value is
 *   text.
 *
 ****************************************************************************/

static void settings_got(FAR struct pnut_settings_client_s *client,
                         int status, FAR const pnut_setting_value_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct settings_cmd_s *cmd = arg;
  pnut_settings_set_request_t set;
  bool secret;
  int ret;

  secret = cmd->op == SETTINGS_SET && status != PNUT_STATUS_OK &&
           err != NULL && err->code == PNUT_SETTINGS_ERROR_CODE_SECRET;
  if (!secret && (status != PNUT_STATUS_OK || out == NULL))
    {
      settings_failed(cmd, status, err);
      return;
    }

  if (cmd->op == SETTINGS_GET)
    {
      settings_print(NULL, true, out);
      settings_end(cmd, OK);
      return;
    }

  memset(&set, 0, sizeof(set));
  strlcpy(set.owner, cmd->owner, sizeof(set.owner));
  strlcpy(set.key, cmd->key, sizeof(set.key));
  set.has_value = true;

  ret = settings_parse(cmd->value, secret ? 0 : out->which_value,
                       &set.value);
  if (ret < 0)
    {
      settings_end(cmd, ret);
      return;
    }

  ret = pnut_settings_set(&cmd->client, &set, 0, settings_done, cmd);
  if (ret < 0)
    {
      fprintf(stderr, "settings: cannot ask Settings: %d\n", ret);
      settings_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: settings_page
 *
 * Description:
 *   Ask for the page of the owner's values after a key; the first with
 *   none.
 *
 ****************************************************************************/

static void settings_page(FAR struct settings_cmd_s *cmd,
                          FAR const char *after)
{
  pnut_settings_page_request_t page;
  int ret;

  memset(&page, 0, sizeof(page));
  strlcpy(page.owner, cmd->owner, sizeof(page.owner));
  strlcpy(page.after, after, sizeof(page.after));

  ret = pnut_settings_list(&cmd->client, &page, 0, settings_listed, cmd);
  if (ret < 0)
    {
      fprintf(stderr, "settings: cannot ask Settings: %d\n", ret);
      settings_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: settings_listed
 ****************************************************************************/

static void settings_listed(FAR struct pnut_settings_client_s *client,
                            int status,
                            FAR const pnut_settings_values_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct settings_cmd_s *cmd = arg;
  char last[sizeof(out->entries[0].key)];
  pb_size_t i;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      settings_failed(cmd, status, err);
      return;
    }

  for (i = 0; i < out->entries_count; i++)
    {
      settings_print(out->entries[i].key, out->entries[i].has_value,
                     &out->entries[i].value);
    }

  if (!out->more || out->entries_count == 0)
    {
      settings_end(cmd, OK);
      return;
    }

  /* out is the client's: the next question reuses it */

  strlcpy(last, out->entries[out->entries_count - 1].key, sizeof(last));
  settings_page(cmd, last);
}

/****************************************************************************
 * Name: settings_ask
 *
 * Description:
 *   The command's first question, once connected.
 *
 ****************************************************************************/

static void settings_ask(FAR struct settings_cmd_s *cmd)
{
  pnut_settings_get_request_t get;
  pnut_settings_reset_request_t reset;
  int ret;

  switch (cmd->op)
    {
      case SETTINGS_LIST:
        settings_page(cmd, "");
        return;

      case SETTINGS_GET:
      case SETTINGS_SET:
        memset(&get, 0, sizeof(get));
        strlcpy(get.owner, cmd->owner, sizeof(get.owner));
        strlcpy(get.key, cmd->key, sizeof(get.key));
        ret = pnut_settings_get(&cmd->client, &get, 0, settings_got, cmd);
        break;

      default:
        memset(&reset, 0, sizeof(reset));
        strlcpy(reset.owner, cmd->owner, sizeof(reset.owner));
        strlcpy(reset.key, cmd->key, sizeof(reset.key));
        ret = pnut_settings_reset(&cmd->client, &reset, 0, settings_done,
                                  cmd);
        break;
    }

  if (ret < 0)
    {
      fprintf(stderr, "settings: cannot ask Settings: %d\n", ret);
      settings_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: settings_state
 *
 * Description:
 *   Connected: ask, once.  A connection lost in a call ends the call, as
 *   unavailable.
 *
 ****************************************************************************/

static void settings_state(FAR struct pnut_settings_client_s *client,
                           bool connected, FAR void *arg)
{
  FAR struct settings_cmd_s *cmd = arg;

  if (connected && !cmd->asked)
    {
      cmd->asked = true;
      settings_ask(cmd);
    }
}

/****************************************************************************
 * Name: settings_timeout
 ****************************************************************************/

static void settings_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct settings_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "settings: %s\n", cmd->asked ?
          "Settings does not answer" : "Settings is not running");
  settings_end(cmd, -ETIMEDOUT);
}

/****************************************************************************
 * Name: settings_start
 *
 * Description:
 *   A command is no service: it never says it is ready, so NxInit is
 *   never told, nor the log.
 *
 ****************************************************************************/

static int settings_start(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct settings_cmd_s *cmd = (FAR struct settings_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, SETTINGS_WAIT_MS, 0, settings_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "settings: no timer: %d\n", ret);
      return ret;
    }

  ret = pnut_settings_connect(loop, SETTINGS_SERVICE, settings_state, cmd,
                              &cmd->client);
  if (ret < 0)
    {
      fprintf(stderr, "settings: cannot reach Settings: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  cmd->open = true;
  return OK;
}

/****************************************************************************
 * Name: settings_stop
 ****************************************************************************/

static void settings_stop(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct settings_cmd_s *cmd = (FAR struct settings_cmd_s *)module;

  if (cmd->open)
    {
      pnut_settings_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/****************************************************************************
 * Name: settings_args
 *
 * Description:
 *   The command line, checked before anything is asked.
 *
 ****************************************************************************/

static int settings_args(FAR struct settings_cmd_s *cmd, int argc,
                         FAR char *argv[])
{
  if (argc < 3)
    {
      return -EINVAL;
    }

  if (strcmp(argv[1], "list") == 0 && argc == 3)
    {
      cmd->op = SETTINGS_LIST;
    }
  else if (strcmp(argv[1], "get") == 0 && argc == 4)
    {
      cmd->op = SETTINGS_GET;
    }
  else if (strcmp(argv[1], "set") == 0 && argc == 5)
    {
      cmd->op    = SETTINGS_SET;
      cmd->value = argv[4];
    }
  else if (strcmp(argv[1], "reset") == 0 && (argc == 3 || argc == 4))
    {
      cmd->op = SETTINGS_RESET;
    }
  else
    {
      return -EINVAL;
    }

  if (strlen(argv[2]) >= sizeof(cmd->owner) ||
      (argc > 3 && strlen(argv[3]) >= sizeof(cmd->key)))
    {
      fprintf(stderr, "settings: a name is at most %zu characters\n",
              sizeof(cmd->owner) - 1);
      return -ENAMETOOLONG;
    }

  strlcpy(cmd->owner, argv[2], sizeof(cmd->owner));
  if (argc > 3)
    {
      strlcpy(cmd->key, argv[3], sizeof(cmd->key));
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct settings_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  /* The client holds a page of answers: too much for the stack */

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "settings: no memory\n");
      return EXIT_FAILURE;
    }

  ret = settings_args(cmd, argc, argv);
  if (ret < 0)
    {
      if (ret == -EINVAL)
        {
          settings_usage();
        }

      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "settings";
  cmd->module.start = settings_start;
  cmd->module.stop  = settings_stop;

  /* One connection, its timer and the command's, no workers, and nothing
   * for NxInit
   */

  pnut_loop_defaults(&config);
  config.name    = "settings";
  config.fds     = 2;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "settings: no loop: %d\n", ret);
      free(cmd);
      return EXIT_FAILURE;
    }

  ret = pnut_module_add(loop, &cmd->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  free(cmd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
