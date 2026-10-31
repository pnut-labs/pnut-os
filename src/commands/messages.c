/****************************************************************************
 * pnut-os/src/commands/messages.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* messages: the messages store from the shell (RFC 0029), for developers.
 * A command typed in the developer's shell may do what the system UI may.
 *
 *   messages                              the conversations
 *   messages show <conversation> [<n>]    its newest n messages (20)
 *   messages send <transport> <peer> <text>...
 *                                         a message queued to send, and
 *                                         how long the store took
 *   messages read <conversation>          mark it read
 *   messages delete <conversation> [<message>]
 *                                         a message, or all of it
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
#include <time.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/msg.h>
#include <pnut/timer.h>

#include "pnut/messages.pnut.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Messages' service, and how long the command waits for it */

#define MESSAGES_SERVICE  "messages"
#define MESSAGES_WAIT_MS  15000
#define MESSAGES_COUNT    20      /* Messages shown at most */

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum messages_op_e
{
  MESSAGES_LIST = 0,
  MESSAGES_SHOW,
  MESSAGES_SEND,
  MESSAGES_READ,
  MESSAGES_DELETE
};

struct messages_cmd_s
{
  struct pnut_module_s module;    /* First: the module is the command */
  struct pnut_messages_client_s client;
  bool open;                      /* The client is */
  bool asked;                     /* The first question went out */
  FAR struct pnut_timer_s *timer; /* Until the command gives up */
  uint8_t op;                     /* enum messages_op_e */
  uint64_t conversation;
  uint64_t message;
  uint32_t shown;
  uint32_t show;                  /* At most */
  uint64_t asked_at;              /* When the send went out, in ms */
  pnut_messages_send_request_t send;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char *const g_messages_status[] =
{
  "done", "busy", "denied", "not found", "invalid", "unavailable",
  "no answer in time", "failed"
};

static FAR const char *const g_messages_state[] =
{
  "unread", "read", "queued", "sending", "sent", "delivered", "failed",
  "not known to be sent"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#define MESSAGES_NAME(table, i) \
  ((i) < sizeof(table) / sizeof(table[0]) ? table[i] : "?")

/****************************************************************************
 * Name: messages_now
 *
 * Description:
 *   Milliseconds, monotonic.
 *
 ****************************************************************************/

static uint64_t messages_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: messages_usage
 ****************************************************************************/

static void messages_usage(void)
{
  fprintf(stderr,
          "usage: messages                             the conversations\n"
          "       messages show <conversation> [<n>]   its newest n\n"
          "       messages send <transport> <peer> <text>...\n"
          "       messages read <conversation>         mark it read\n"
          "       messages delete <conversation> [<message>]\n");
}

/****************************************************************************
 * Name: messages_end
 ****************************************************************************/

static void messages_end(FAR struct messages_cmd_s *cmd, int status)
{
  pnut_loop_stop(cmd->module.loop, status);
}

/****************************************************************************
 * Name: messages_failed
 *
 * Description:
 *   An error answer: its status, and the service's reason.
 *
 ****************************************************************************/

static void messages_failed(FAR struct messages_cmd_s *cmd, int status,
                            FAR const pnut_error_t *err)
{
  fprintf(stderr, "messages: %s%s%s\n",
          status >= 0 && status < (int)(sizeof(g_messages_status) /
                                        sizeof(g_messages_status[0])) ?
          g_messages_status[status] : "failed",
          err != NULL && err->text[0] != '\0' ? ": " : "",
          err != NULL ? err->text : "");
  messages_end(cmd, -EIO);
}

/****************************************************************************
 * Name: messages_list_done
 ****************************************************************************/

static void messages_list(FAR struct messages_cmd_s *cmd, uint64_t after);

static void messages_list_done(FAR struct pnut_messages_client_s *client,
                               int status,
                               FAR const pnut_messages_conversations_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;
  FAR const pnut_messages_conversation_t *c;
  pb_size_t i;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      messages_failed(cmd, status, err);
      return;
    }

  for (i = 0; i < out->conversations_count; i++)
    {
      c = &out->conversations[i];
      printf("%-4" PRIu64 " %s %s  %" PRIu32 " messages, %" PRIu32
             " unread  %s\n", c->id, c->transport, c->peer, c->count,
             c->unread, c->latest);
      cmd->shown++;
    }

  if (out->more && out->conversations_count > 0)
    {
      messages_list(cmd, out->conversations[out->conversations_count -
                                             1].id);
      return;
    }

  if (cmd->shown == 0)
    {
      printf("No conversations\n");
    }

  messages_end(cmd, OK);
}

static void messages_list(FAR struct messages_cmd_s *cmd, uint64_t after)
{
  pnut_messages_page_request_t page;
  int ret;

  memset(&page, 0, sizeof(page));
  page.after = after;
  ret = pnut_messages_conversations(&cmd->client, &page, 0,
                                    messages_list_done, cmd);
  if (ret < 0)
    {
      fprintf(stderr, "messages: cannot ask: %d\n", ret);
      messages_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: messages_show_done
 ****************************************************************************/

static void messages_show(FAR struct messages_cmd_s *cmd, uint64_t after);

static void messages_show_done(FAR struct pnut_messages_client_s *client,
                               int status,
                               FAR const pnut_messages_page_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;
  FAR const pnut_messages_message_t *m;
  pb_size_t i;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      messages_failed(cmd, status, err);
      return;
    }

  for (i = 0; i < out->messages_count && cmd->shown < cmd->show; i++)
    {
      m = &out->messages[i];
      printf("%-4" PRIu64 " %s %s%s%s%s%s\n  %s\n", m->id,
             m->out ? "out" : "in ",
             MESSAGES_NAME(g_messages_state, (unsigned)m->status),
             m->reason[0] != '\0' ? ": " : "", m->reason,
             m->sender[0] != '\0' ? ", from " : "", m->sender, m->text);
      cmd->shown++;
    }

  if (out->more && out->messages_count > 0 && cmd->shown < cmd->show)
    {
      messages_show(cmd, out->messages[out->messages_count - 1].id);
      return;
    }

  messages_end(cmd, OK);
}

static void messages_show(FAR struct messages_cmd_s *cmd, uint64_t after)
{
  pnut_messages_read_request_t read;
  int ret;

  memset(&read, 0, sizeof(read));
  read.conversation = cmd->conversation;
  read.after        = after;
  ret = pnut_messages_read(&cmd->client, &read, 0, messages_show_done,
                           cmd);
  if (ret < 0)
    {
      fprintf(stderr, "messages: cannot ask: %d\n", ret);
      messages_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: messages_sent, messages_done
 ****************************************************************************/

static void messages_sent(FAR struct pnut_messages_client_s *client,
                          int status, FAR const pnut_messages_sent_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      messages_failed(cmd, status, err);
      return;
    }

  printf("Queued: message %" PRIu64 " in conversation %" PRIu64
         ", stored in %" PRIu64 " ms\n", out->message, out->conversation,
         messages_now() - cmd->asked_at);
  messages_end(cmd, OK);
}

static void messages_done(FAR struct pnut_messages_client_s *client,
                          int status, FAR const pnut_messages_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;

  if (status != PNUT_STATUS_OK)
    {
      messages_failed(cmd, status, err);
      return;
    }

  printf("Done\n");
  messages_end(cmd, OK);
}

/****************************************************************************
 * Name: messages_ask
 ****************************************************************************/

static void messages_ask(FAR struct messages_cmd_s *cmd)
{
  pnut_messages_conversation_request_t conv;
  pnut_messages_delete_request_t del;
  int ret = OK;

  switch (cmd->op)
    {
      case MESSAGES_SHOW:
        messages_show(cmd, 0);
        return;

      case MESSAGES_SEND:
        cmd->asked_at = messages_now();
        ret = pnut_messages_send(&cmd->client, &cmd->send, 0,
                                 messages_sent, cmd);
        break;

      case MESSAGES_READ:
        memset(&conv, 0, sizeof(conv));
        conv.conversation = cmd->conversation;
        ret = pnut_messages_mark_read(&cmd->client, &conv, 0,
                                      messages_done, cmd);
        break;

      case MESSAGES_DELETE:
        memset(&del, 0, sizeof(del));
        del.conversation = cmd->conversation;
        del.message      = cmd->message;
        ret = pnut_messages_delete(&cmd->client, &del, 0, messages_done,
                                   cmd);
        break;

      default:
        messages_list(cmd, 0);
        return;
    }

  if (ret < 0)
    {
      fprintf(stderr, "messages: cannot ask: %d\n", ret);
      messages_end(cmd, ret);
    }
}

/****************************************************************************
 * Name: messages_state
 ****************************************************************************/

static void messages_state(FAR struct pnut_messages_client_s *client,
                           bool connected, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;

  if (connected && !cmd->asked)
    {
      cmd->asked = true;
      messages_ask(cmd);
    }
}

/****************************************************************************
 * Name: messages_timeout
 ****************************************************************************/

static void messages_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct messages_cmd_s *cmd = arg;

  cmd->timer = NULL;
  fprintf(stderr, "messages: %s\n", cmd->asked ?
          "Messages does not answer" : "Messages is not running");
  messages_end(cmd, -ETIMEDOUT);
}

/****************************************************************************
 * Name: messages_start
 ****************************************************************************/

static int messages_start(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct messages_cmd_s *cmd = (FAR struct messages_cmd_s *)module;
  int ret;

  ret = pnut_timer_start(loop, MESSAGES_WAIT_MS, 0, messages_timeout, cmd,
                         &cmd->timer);
  if (ret < 0)
    {
      fprintf(stderr, "messages: no timer: %d\n", ret);
      return ret;
    }

  ret = pnut_messages_connect(loop, MESSAGES_SERVICE, messages_state, cmd,
                              &cmd->client);
  if (ret < 0)
    {
      fprintf(stderr, "messages: cannot reach Messages: %d\n", ret);
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
      return ret;
    }

  cmd->open = true;
  return OK;
}

/****************************************************************************
 * Name: messages_stop
 ****************************************************************************/

static void messages_stop(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct messages_cmd_s *cmd = (FAR struct messages_cmd_s *)module;

  if (cmd->open)
    {
      pnut_messages_disconnect(&cmd->client);
      cmd->open = false;
    }

  if (cmd->timer != NULL)
    {
      pnut_timer_cancel(loop, cmd->timer);
      cmd->timer = NULL;
    }
}

/****************************************************************************
 * Name: messages_number
 ****************************************************************************/

static int messages_number(FAR const char *arg, FAR uint64_t *value)
{
  FAR char *end;

  *value = strtoull(arg, &end, 10);
  return *end == '\0' && *value > 0 ? OK : -EINVAL;
}

/****************************************************************************
 * Name: messages_args
 ****************************************************************************/

static int messages_args(FAR struct messages_cmd_s *cmd, int argc,
                         FAR char *argv[])
{
  uint64_t n;
  size_t len = 0;
  int i;

  cmd->show = MESSAGES_COUNT;
  if (argc == 1)
    {
      cmd->op = MESSAGES_LIST;
      return OK;
    }

  if (strcmp(argv[1], "show") == 0 && (argc == 3 || argc == 4))
    {
      cmd->op = MESSAGES_SHOW;
      if (messages_number(argv[2], &cmd->conversation) < 0)
        {
          return -EINVAL;
        }

      if (argc == 4)
        {
          if (messages_number(argv[3], &n) < 0 || n > UINT32_MAX)
            {
              return -EINVAL;
            }

          cmd->show = (uint32_t)n;
        }

      return OK;
    }

  if (strcmp(argv[1], "send") == 0 && argc >= 5)
    {
      cmd->op = MESSAGES_SEND;
      if (strlcpy(cmd->send.transport, argv[2],
                  sizeof(cmd->send.transport)) >=
          sizeof(cmd->send.transport) ||
          strlcpy(cmd->send.peer, argv[3], sizeof(cmd->send.peer)) >=
          sizeof(cmd->send.peer))
        {
          fprintf(stderr, "messages: the transport or peer is too long\n");
          return -E2BIG;
        }

      /* The text's words, joined with a space */

      for (i = 4; i < argc; i++)
        {
          if (i > 4)
            {
              len = strlcat(cmd->send.text, " ", sizeof(cmd->send.text));
            }

          len = strlcat(cmd->send.text, argv[i], sizeof(cmd->send.text));
          if (len >= sizeof(cmd->send.text))
            {
              fprintf(stderr, "messages: the text is too long\n");
              return -E2BIG;
            }
        }

      return OK;
    }

  if (strcmp(argv[1], "read") == 0 && argc == 3)
    {
      cmd->op = MESSAGES_READ;
      return messages_number(argv[2], &cmd->conversation);
    }

  if (strcmp(argv[1], "delete") == 0 && (argc == 3 || argc == 4))
    {
      cmd->op = MESSAGES_DELETE;
      if (messages_number(argv[2], &cmd->conversation) < 0 ||
          (argc == 4 && messages_number(argv[3], &cmd->message) < 0))
        {
          return -EINVAL;
        }

      return OK;
    }

  return -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  FAR struct messages_cmd_s *cmd;
  FAR struct pnut_loop_s *loop;
  int ret;

  /* The client holds a page of messages, 3 KB: not for the stack */

  cmd = calloc(1, sizeof(*cmd));
  if (cmd == NULL)
    {
      fprintf(stderr, "messages: no memory\n");
      return EXIT_FAILURE;
    }

  ret = messages_args(cmd, argc, argv);
  if (ret < 0)
    {
      if (ret == -EINVAL)
        {
          messages_usage();
        }

      free(cmd);
      return EXIT_FAILURE;
    }

  cmd->module.name  = "messages";
  cmd->module.start = messages_start;
  cmd->module.stop  = messages_stop;

  /* One connection, its timer and the command's, no workers, and nothing
   * for NxInit
   */

  pnut_loop_defaults(&config);
  config.name    = "messages";
  config.fds     = 2;
  config.timers  = 4;
  config.jobs    = 0;
  config.workers = 0;
  config.initctl = NULL;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "messages: no loop: %d\n", ret);
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
