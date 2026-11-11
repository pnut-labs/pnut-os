/****************************************************************************
 * pnut-os/src/telephony/at.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include <pnut/log.h>

#include "at.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The lines that end a command with an error; and those that end a call's
 * command (ATD, ATA) so, which come unasked otherwise
 */

static FAR const char *const g_at_errors[] =
{
  "ERROR", "+CME ERROR", "+CMS ERROR", NULL
};

static FAR const char *const g_at_calls[] =
{
  "NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE", NULL
};

/* Lines the modem sends unasked.  An answer may begin as one does
 * ("+CPIN:" to AT+CPIN?): its command's prefix tells.
 */

static FAR const char *const g_at_urcs[] =
{
  "RING", "+CLIP:", "+CMTI:", "+CMT:", "*ATREADY", "RDY", "+CPIN:",
  "+CREG:", "+CEREG:", "+CGREG:", "NO CARRIER", "VOICE CALL:", "+CGEV:",
  "SMS DONE", "PB DONE", "+CTZV:", "+CTZE:", "+CTZEU:", "*PSUTTZ", "DST:",
  "MISSED_CALL", "+CLCC:", "+CSQ:", "+CPSI:", "+SIMCARD:", NULL
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: at_starts
 ****************************************************************************/

static bool at_starts(FAR const char *line, FAR const char *const *list)
{
  for (; *list != NULL; list++)
    {
      if (strncmp(line, *list, strlen(*list)) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: at_append
 *
 * Description:
 *   Keep a line of the answer.  One that finds no room is left out whole.
 *
 ****************************************************************************/

static void at_append(FAR struct at_s *at, FAR const char *line)
{
  size_t n = strlen(line);

  if (at->rlen + n + 2 <= sizeof(at->reply))
    {
      memcpy(at->reply + at->rlen, line, n);
      at->rlen += n;
      at->reply[at->rlen++] = '\n';
      at->reply[at->rlen]   = '\0';
    }
}

/****************************************************************************
 * Name: at_timeout
 ****************************************************************************/

static void at_finish(FAR struct at_s *at, int result);

static void at_timeout(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct at_s *at = arg;

  at->timer = NULL;
  at_finish(at, -ETIMEDOUT);
}

/****************************************************************************
 * Name: at_next
 *
 * Description:
 *   Send the first command, if none is out.
 *
 ****************************************************************************/

static void at_next(FAR struct at_s *at)
{
  FAR struct at_command_s *cmd;
  int ret;

  while (!at->sent && !at->escaping && at->cancelling == 0 &&
         !at->held && at->count > 0)
    {
      cmd       = &at->queue[at->head];
      at->rlen  = 0;
      at->reply[0] = '\0';

      /* Out from before the send: room told from inside it (the channel
       * flushing after an earlier refusal) must not send it again
       */

      at->sent = true;
      ret = at->ops->send(at->arg, cmd->command, strlen(cmd->command));
      if (ret == -EAGAIN)
        {
          at->sent = false;
          return;                       /* at_room() sends it */
        }

      if (ret >= 0)
        {
          pnut_log(at->module, PNUT_LOG_DEBUG, "> %.*s",
                   (cmd->flags & AT_SECRET) ? 8 :
                   (int)strcspn(cmd->command, "\r"),
                   (cmd->flags & AT_SECRET) ? "(secret)" : cmd->command);
        }

      if (ret >= 0)
        {
          ret = pnut_timer_start(at->loop, cmd->timeout, 0, at_timeout, at,
                                 &at->timer);
          if (ret >= 0)
            {
              return;
            }

          at->timer = NULL;
        }

      /* Not sent, or not timed: it fails, and the next is tried */

      at_finish(at, ret);
      return;
    }
}

/****************************************************************************
 * Name: at_finish
 *
 * Description:
 *   The command out has ended: tell its handler, and send the next.
 *
 ****************************************************************************/

static void at_finish(FAR struct at_s *at, int result)
{
  FAR struct at_command_s *cmd;
  at_done_t done;
  FAR void *arg;
  bool esc;

  if (!at->sent || at->count == 0)
    {
      return;
    }

  if (at->timer != NULL)
    {
      pnut_timer_cancel(at->loop, at->timer);
      at->timer = NULL;
    }

  cmd  = &at->queue[at->head];
  done = cmd->done;
  arg  = cmd->arg;
  if (cmd->flags & AT_SECRET)
    {
      explicit_bzero(cmd->command, sizeof(cmd->command));
    }

  /* A text command that failed while the modem may be in its text
   * input: out of it.  Nothing of this command is waited for any more
   * (texting cleared first), since the send may give the channel room,
   * and at_room() is called from inside it; sent is still set, so
   * at_next() sends nothing from there either.
   */

  esc = cmd->text != NULL && result < 0 &&
        (at->texting || (cmd->prompted && !cmd->ended) ||
         (!cmd->prompted && result == -ETIMEDOUT));
  at->texting = false;
  if (esc && at->ops->send(at->arg, "\x1b", 1) == -EAGAIN)
    {
      at->escaping = true;        /* at_room() sends it, before any */
    }

  at->head    = (at->head + 1) % AT_QUEUE;
  at->count--;
  at->sent    = false;

  if (done != NULL)
    {
      done(arg, result, at->reply);
    }

  at_next(at);
}

/****************************************************************************
 * Name: at_text
 *
 * Description:
 *   The prompt came: the command's text, then Ctrl-Z; with no room on the
 *   channel, once there is.
 *
 ****************************************************************************/

static void at_text(FAR struct at_s *at)
{
  FAR struct at_command_s *cmd = &at->queue[at->head];
  size_t n = strlen(cmd->text);
  int ret;

  cmd->prompted = true;
  at->texting   = false;
  if (!cmd->text_sent)
    {
      ret = at->ops->send(at->arg, cmd->text, n);
      if (ret == -EAGAIN)
        {
          at->texting = true;     /* at_room() sends it */
          return;
        }

      if (ret < 0)
        {
          at_finish(at, ret);
          return;
        }

      cmd->text_sent = true;
      pnut_log(at->module, PNUT_LOG_DEBUG, "> (text, %zu)", n);
    }

  ret = at->ops->send(at->arg, "\x1a", 1);
  if (ret == -EAGAIN)
    {
      at->texting = true;         /* Ctrl-Z alone, then */
      return;
    }

  if (ret < 0)
    {
      at_finish(at, ret);
      return;
    }

  cmd->ended = true;
}

/****************************************************************************
 * Name: at_line
 *
 * Description:
 *   A whole line from the modem: part of the answer to the command out, its
 *   end, or a line sent unasked.
 *
 ****************************************************************************/

static void at_line(FAR struct at_s *at, FAR const char *line)
{
  FAR struct at_command_s *cmd;
  size_t n;

  /* A line of blanks says nothing; nor does a prompt ("> "), which
   * at_input() takes when a text command waits for it, and which would
   * otherwise begin the next answer
   */

  if (line[strspn(line, " \t")] == '\0' || strcmp(line, "> ") == 0)
    {
      return;
    }

  if (at->sent && at->count > 0)
    {
      cmd = &at->queue[at->head];

      /* The command's echo, until echo is off: not logged, as a secret's
       * would show
       */

      n = strlen(line);
      if (strncmp(cmd->command, line, n) == 0 && cmd->command[n] == '\r')
        {
          return;
        }
    }

  pnut_log(at->module, PNUT_LOG_DEBUG, "< %s", line);

  if (at->sent && at->count > 0)
    {
      cmd = &at->queue[at->head];

      if (strcmp(line, "OK") == 0)
        {
          at_finish(at, OK);
          return;
        }

      if (at_starts(line, g_at_errors) ||
          (at_starts(line, g_at_calls) &&
           (strncmp(cmd->command, "ATD", 3) == 0 ||
            strncmp(cmd->command, "ATA", 3) == 0)))
        {
          at_append(at, line);
          at_finish(at, -EIO);
          return;
        }

      if ((cmd->prefix[0] != '\0' &&
           strncmp(line, cmd->prefix, strlen(cmd->prefix)) == 0) ||
          !at_starts(line, g_at_urcs))
        {
          at_append(at, line);
          return;
        }
    }

  at->ops->urc(at->arg, line);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void at_init(FAR struct at_s *at, FAR struct pnut_module_s *module,
             FAR const struct at_ops_s *ops, FAR void *arg)
{
  memset(at, 0, sizeof(*at));
  at->module = module;
  at->loop   = module->loop;
  at->ops    = ops;
  at->arg    = arg;
}

/****************************************************************************
 * Name: at_queue
 *
 * Description:
 *   A command queued, with its text or none.
 *
 ****************************************************************************/

static int at_queue(FAR struct at_s *at, FAR const char *command,
                    FAR const char *text, FAR const char *prefix,
                    uint32_t timeout, uint8_t flags, at_done_t done,
                    FAR void *arg)
{
  FAR struct at_command_s *cmd;
  size_t n = strlen(command);

  if (n > AT_COMMAND_MAX ||
      (prefix != NULL && strlen(prefix) >= AT_PREFIX_MAX))
    {
      return -E2BIG;
    }

  if (at->count >= AT_QUEUE)
    {
      return -EBUSY;
    }

  cmd = &at->queue[(at->head + at->count) % AT_QUEUE];
  memcpy(cmd->command, command, n);
  cmd->command[n]     = '\r';
  cmd->command[n + 1] = '\0';
  strlcpy(cmd->prefix, prefix != NULL ? prefix : "", sizeof(cmd->prefix));
  cmd->text      = text;
  cmd->prompted  = false;
  cmd->text_sent = false;
  cmd->ended     = false;
  cmd->timeout  = timeout > 0 ? timeout : AT_TIMEOUT;
  cmd->flags    = flags;
  cmd->done     = done;
  cmd->arg      = arg;
  at->count++;

  at_next(at);
  return OK;
}

int at_command(FAR struct at_s *at, FAR const char *command,
               FAR const char *prefix, uint32_t timeout, uint8_t flags,
               at_done_t done, FAR void *arg)
{
  return at_queue(at, command, NULL, prefix, timeout, flags, done, arg);
}

int at_command_text(FAR struct at_s *at, FAR const char *command,
                    FAR const char *text, FAR const char *prefix,
                    uint32_t timeout, at_done_t done, FAR void *arg)
{
  if (text == NULL)
    {
      return -EINVAL;
    }

  return at_queue(at, command, text, prefix, timeout, 0, done, arg);
}

void at_input(FAR struct at_s *at, FAR const char *buf, size_t len)
{
  size_t i;
  char c;

  for (i = 0; i < len; i++)
    {
      c = buf[i];
      if (c == '\r' || c == '\n')
        {
          if (at->llen > 0)
            {
              at->line[at->llen] = '\0';
              at->llen = 0;
              at->long_line = false;
              at_line(at, at->line);
            }
          else
            {
              at->long_line = false;
            }

          continue;
        }

      if (at->llen < sizeof(at->line) - 1)
        {
          at->line[at->llen++] = c;
        }
      else
        {
          at->long_line = true;           /* The rest is cut off */
        }

      /* The prompt for a text, which no line end follows */

      if (at->llen == 2 && at->line[0] == '>' && at->line[1] == ' ' &&
          at->sent && at->count > 0 &&
          at->queue[at->head].text != NULL &&
          !at->queue[at->head].prompted)
        {
          at->llen = 0;
          at_text(at);
        }
    }
}

void at_room(FAR struct at_s *at)
{
  if (at->escaping)
    {
      /* Cleared before the send, as at_next() does: room told from
       * inside it must not send it again
       */

      at->escaping = false;
      if (at->ops->send(at->arg, "\x1b", 1) == -EAGAIN)
        {
          at->escaping = true;
          return;
        }
    }

  if (at->texting && at->sent && at->count > 0)
    {
      at_text(at);
      return;
    }

  at_next(at);
}

void at_cancel(FAR struct at_s *at, int result)
{
  FAR struct at_command_s *cmd;
  at_done_t done;
  FAR void *arg;
  uint8_t n = at->count;

  at->llen      = 0;
  at->long_line = false;

  if (at->timer != NULL)
    {
      pnut_timer_cancel(at->loop, at->timer);
      at->timer = NULL;
    }

  /* Those the handlers queue meanwhile stay, and wait until the end.  A
   * handler that cancelled again would leave the count taken here too
   * high: none does (a handler cancelled returns at once).
   */

  at->cancelling++;
  at->sent     = false;
  at->texting  = false;
  at->escaping = false;

  while (n-- > 0 && at->count > 0)
    {
      cmd  = &at->queue[at->head];
      done = cmd->done;
      arg  = cmd->arg;
      if (cmd->flags & AT_SECRET)
        {
          explicit_bzero(cmd->command, sizeof(cmd->command));
        }

      at->head = (at->head + 1) % AT_QUEUE;
      at->count--;
      at->rlen     = 0;
      at->reply[0] = '\0';

      if (done != NULL)
        {
          done(arg, result, at->reply);
        }
    }

  at->cancelling--;
  at_next(at);
}

/****************************************************************************
 * Name: at_hold
 ****************************************************************************/

void at_hold(FAR struct at_s *at, bool hold)
{
  at->held = hold;
  if (!hold)
    {
      at_next(at);
    }
}

/****************************************************************************
 * Name: at_idle
 ****************************************************************************/

bool at_idle(FAR const struct at_s *at)
{
  return at->count == 0;
}
