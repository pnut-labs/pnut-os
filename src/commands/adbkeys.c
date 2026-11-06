/****************************************************************************
 * pnut-os/src/commands/adbkeys.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* adbkeys: the computers adb lets in (RFC 0012), from the shell, until the
 * system UI asks on the screen.  adbd keeps the key a new computer offers
 * in <keys>.new, beside the others waiting; one is let in once its line is
 * in <keys>.  Each key is shown with its fingerprint, the MD5 of the key
 * as Android shows it; on the computer:
 *
 *   cut -d' ' -f1 ~/.android/adbkey.pub | base64 -d | md5sum
 *
 *   adbkeys              the computers let in, and those waiting
 *   adbkeys allow <n>    let in the one waiting with that number
 *   adbkeys clear        drop those waiting
 *   adbkeys forget       let none in any more (asked first)
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <netutils/md5.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ADBKEYS_FILE      CONFIG_ADBD_AUTH_KEYS
#define ADBKEYS_NEW       CONFIG_ADBD_AUTH_KEYS ".new"
#define ADBKEYS_TMP       CONFIG_ADBD_AUTH_KEYS ".new.tmp"

/* A key line: Android's RSAPublicKey in base64 (700 characters), a space,
 * a comment
 */

#define ADBKEYS_KEY_BYTES 524
#define ADBKEYS_LINE      832
#define ADBKEYS_MAX       16      /* Lines read from a file */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct adbkeys_s
{
  char line[ADBKEYS_MAX][ADBKEYS_LINE];
  int count;
  int all;                        /* Lines in the file, beyond those too */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void adbkeys_usage(void)
{
  fprintf(stderr,
          "Usage: adbkeys\n"
          "       adbkeys allow <n>\n"
          "       adbkeys clear\n"
          "       adbkeys forget\n");
}

static int adbkeys_b64(char c)
{
  if (c >= 'A' && c <= 'Z')
    {
      return c - 'A';
    }

  if (c >= 'a' && c <= 'z')
    {
      return c - 'a' + 26;
    }

  if (c >= '0' && c <= '9')
    {
      return c - '0' + 52;
    }

  if (c == '+')
    {
      return 62;
    }

  return c == '/' ? 63 : -1;
}

/* A line's key, decoded as adbd decodes it (to the first character that
 * is not base64): false unless it is a key's size exactly
 */

static bool adbkeys_decode(FAR const char *line,
                           FAR uint8_t key[ADBKEYS_KEY_BYTES])
{
  uint32_t bits = 0;
  size_t len = 0;
  int nbits = 0;
  int v;

  for (; (v = adbkeys_b64(*line)) >= 0; line++)
    {
      bits   = (bits << 6) | (uint32_t)v;
      nbits += 6;
      if (nbits >= 8)
        {
          nbits -= 8;
          if (len >= ADBKEYS_KEY_BYTES)
            {
              return false;       /* Longer than a key */
            }

          key[len++] = (uint8_t)(bits >> nbits);
        }
    }

  return len == ADBKEYS_KEY_BYTES;
}

/* A key in base64, padded, as adbkey.pub has it: 700 characters and a
 * NUL into out
 */

static void adbkeys_encode(FAR const uint8_t key[ADBKEYS_KEY_BYTES],
                           FAR char *out)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t v;
  size_t i;

  for (i = 0; i + 2 < ADBKEYS_KEY_BYTES; i += 3)
    {
      v = ((uint32_t)key[i] << 16) | ((uint32_t)key[i + 1] << 8) |
          key[i + 2];
      *out++ = alphabet[(v >> 18) & 63];
      *out++ = alphabet[(v >> 12) & 63];
      *out++ = alphabet[(v >> 6) & 63];
      *out++ = alphabet[v & 63];
    }

  if (i < ADBKEYS_KEY_BYTES)
    {
      v = (uint32_t)key[i] << 16;
      if (i + 1 < ADBKEYS_KEY_BYTES)
        {
          v |= (uint32_t)key[i + 1] << 8;
        }

      *out++ = alphabet[(v >> 18) & 63];
      *out++ = alphabet[(v >> 12) & 63];
      *out++ = i + 1 < ADBKEYS_KEY_BYTES ? alphabet[(v >> 6) & 63] : '=';
      *out++ = '=';
    }

  *out = '\0';
}

/* A line's fingerprint into out (48 bytes and a NUL), or "not a key" */

static void adbkeys_fingerprint(FAR const char *line, FAR char *out)
{
  uint8_t key[ADBKEYS_KEY_BYTES];
  uint8_t md5[16];
  int i;

  if (!adbkeys_decode(line, key))
    {
      strlcpy(out, "not a key", 49);
      return;
    }

  md5_sum(key, sizeof(key), md5);
  for (i = 0; i < 16; i++)
    {
      sprintf(out + 3 * i, "%02x:", md5[i]);
    }

  out[47] = '\0';
}

/* A line's comment, after its first space, printable characters alone */

static void adbkeys_comment(FAR const char *line, FAR char *out,
                            size_t size)
{
  FAR const char *c = strchr(line, ' ');
  size_t n = 0;

  for (c = c != NULL ? c + 1 : ""; *c != '\0' && *c != '\n' &&
       n + 1 < size; c++)
    {
      out[n++] = *c >= 0x20 && *c <= 0x7e ? *c : '?';
    }

  out[n] = '\0';
}

/* A file's lines, without their newlines, the first ADBKEYS_MAX of them;
 * blank lines left out, and a line longer than ADBKEYS_LINE cut there
 */

static void adbkeys_read(FAR const char *path, FAR struct adbkeys_s *keys)
{
  char spill[ADBKEYS_LINE];
  FAR char *line;
  FAR FILE *f;
  size_t n;
  int c;

  keys->count = 0;
  keys->all   = 0;
  f = fopen(path, "r");
  if (f == NULL)
    {
      return;
    }

  for (; ; )
    {
      line = keys->count < ADBKEYS_MAX ? keys->line[keys->count] : spill;
      if (fgets(line, ADBKEYS_LINE, f) == NULL)
        {
          break;
        }

      n = strcspn(line, "\r\n");
      if (line[n] == '\0')
        {
          /* Longer than a key line: the rest of it read past */

          while ((c = fgetc(f)) != EOF && c != '\n')
            {
            }
        }

      line[n] = '\0';
      if (n > 0)
        {
          keys->all++;
          if (line != spill)
            {
              keys->count++;
            }
        }
    }

  fclose(f);
}

/* Whether path holds a key of this fingerprint already: every line of it
 * read
 */

static bool adbkeys_has(FAR const char *path, FAR const char *fingerprint)
{
  char line[ADBKEYS_LINE];
  char other[49];
  bool has = false;
  FAR FILE *f;
  int c;

  f = fopen(path, "r");
  if (f == NULL)
    {
      return false;
    }

  while (!has && fgets(line, sizeof(line), f) != NULL)
    {
      if (strchr(line, '\n') == NULL)
        {
          while ((c = fgetc(f)) != EOF && c != '\n')
            {
            }
        }

      adbkeys_fingerprint(line, other);
      has = strcmp(other, fingerprint) == 0;
    }

  fclose(f);
  return has;
}

static void adbkeys_show(FAR const char *title,
                         FAR const struct adbkeys_s *keys)
{
  char fingerprint[49];
  char comment[64];
  int i;

  printf("%s:\n", title);
  if (keys->count == 0)
    {
      printf("  none\n");
    }

  for (i = 0; i < keys->count; i++)
    {
      adbkeys_fingerprint(keys->line[i], fingerprint);
      adbkeys_comment(keys->line[i], comment, sizeof(comment));
      printf("  %d  %s  %s\n", i + 1, fingerprint, comment);
    }

  if (keys->all > keys->count)
    {
      printf("  ... and %d more\n", keys->all - keys->count);
    }
}

/* All of buf written to fd, or a negated errno value */

static int adbkeys_write(int fd, FAR const char *buf, size_t len)
{
  ssize_t n;

  while (len > 0)
    {
      n = write(fd, buf, len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return n < 0 ? -errno : -EIO;
        }

      buf += n;
      len -= (size_t)n;
    }

  return 0;
}

/* One waiting key let in: added to the keys, then the waiting file
 * written again without it (a new file renamed over the old)
 */

static int adbkeys_allow(int n)
{
  uint8_t key[ADBKEYS_KEY_BYTES];
  char line[ADBKEYS_LINE];
  FAR struct adbkeys_s *waiting;
  char fingerprint[49];
  char comment[64];
  struct stat st;
  int ret = EXIT_FAILURE;
  size_t len;
  int fd;
  int i;

  waiting = malloc(sizeof(*waiting));
  if (waiting == NULL)
    {
      fprintf(stderr, "adbkeys: no memory\n");
      return EXIT_FAILURE;
    }

  adbkeys_read(ADBKEYS_NEW, waiting);
  if (n < 1 || n > waiting->count)
    {
      fprintf(stderr, "adbkeys: no key waiting as %d\n", n);
      goto out;
    }

  adbkeys_fingerprint(waiting->line[n - 1], fingerprint);
  if (strcmp(fingerprint, "not a key") == 0)
    {
      fprintf(stderr, "adbkeys: %d is not a key\n", n);
      goto out;
    }

  if (waiting->all > waiting->count)
    {
      fprintf(stderr, "adbkeys: more than %d lines wait: adbkeys clear, "
              "and offer the key again\n", ADBKEYS_MAX);
      goto out;
    }

  /* Added once, the line and its newline in one write, cut back if that
   * fails half way
   */

  if (!adbkeys_has(ADBKEYS_FILE, fingerprint))
    {
      /* The line let in is the key fingerprinted, encoded again, and its
       * comment as shown: nothing else of the line waiting
       */

      adbkeys_decode(waiting->line[n - 1], key);
      adbkeys_encode(key, line);
      len = strlen(line);
      adbkeys_comment(waiting->line[n - 1], comment, sizeof(comment));
      if (comment[0] != '\0')
        {
          line[len++] = ' ';
          len += strlcpy(line + len, comment, sizeof(line) - len);
        }

      line[len++] = '\n';
      st.st_size = -1;
      fd = open(ADBKEYS_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                0600);
      if (fd < 0 || fstat(fd, &st) < 0 ||
          adbkeys_write(fd, line, len) < 0 || fsync(fd) < 0)
        {
          fprintf(stderr, "adbkeys: cannot write %s: %d\n", ADBKEYS_FILE,
                  -errno);
          if (fd >= 0)
            {
              if (st.st_size >= 0)
                {
                  ftruncate(fd, st.st_size);  /* No half line left */
                }

              close(fd);
            }

          goto out;
        }

      close(fd);
    }

  printf("Let in: %s\n", fingerprint);
  syslog(LOG_NOTICE, "adbkeys: a computer let in, key fingerprint %s\n",
         fingerprint);
  ret = EXIT_SUCCESS;

  /* The rest still waiting */

  if (waiting->count == 1)
    {
      unlink(ADBKEYS_NEW);
      goto out;
    }

  fd = open(ADBKEYS_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      unlink(ADBKEYS_NEW);
      goto dropped;
    }

  for (i = 0; i < waiting->count; i++)
    {
      if (i != n - 1 &&
          (adbkeys_write(fd, waiting->line[i], strlen(waiting->line[i]))
           < 0 || adbkeys_write(fd, "\n", 1) < 0))
        {
          break;
        }
    }

  if (i < waiting->count || fsync(fd) < 0)
    {
      close(fd);
      unlink(ADBKEYS_TMP);
      unlink(ADBKEYS_NEW);
      goto dropped;
    }

  close(fd);
  if (rename(ADBKEYS_TMP, ADBKEYS_NEW) >= 0)
    {
      goto out;
    }

  unlink(ADBKEYS_TMP);
  unlink(ADBKEYS_NEW);

dropped:
  fprintf(stderr, "adbkeys: the other keys waiting were dropped: their "
          "computers offer them again when they connect\n");

out:
  free(waiting);
  return ret;
}

/* adbd started again, so that no computer stays connected: its pid from
 * NxInit's control socket ("state adbd": "ok adbd ready <pid>"), then
 * SIGTERM, on which adbd closes every connection and exits, and NxInit
 * starts it again.  0, or a negated errno value; -ESRCH when it does not
 * run, so nobody is connected.
 */

static int adbkeys_restart(void)
{
#ifdef CONFIG_SYSTEM_NXINIT_CONTROL
  static const char command[] = "state adbd\n";
  struct sockaddr_un addr;
  struct timeval tv;
  char buf[128];
  char state[16];
  FAR char *line;
  size_t len = 0;
  int lines = 0;
  ssize_t n;
  long pid;
  int fd;

  fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_LOCAL;
  strlcpy(addr.sun_path, CONFIG_SYSTEM_NXINIT_CONTROL_PATH,
          sizeof(addr.sun_path));
  tv.tv_sec  = 3;
  tv.tv_usec = 0;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
      connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      adbkeys_write(fd, command, sizeof(command) - 1) < 0)
    {
      n = -errno;
      close(fd);
      return (int)n;
    }

  /* The version line, then the answer */

  while (lines < 2 && len < sizeof(buf) - 1)
    {
      n = read(fd, buf + len, sizeof(buf) - 1 - len);
      if (n <= 0)
        {
          break;
        }

      for (; n > 0; n--)
        {
          lines += buf[len++] == '\n';
        }
    }

  close(fd);
  buf[len] = '\0';

  /* No answer, or an error (NxInit busy): not known to be stopped */

  line = strchr(buf, '\n');
  if (line == NULL || lines < 2 ||
      sscanf(line + 1, "ok adbd %15s %ld", state, &pid) != 2)
    {
      return -EIO;
    }

  if (pid <= 0 ||
      (strcmp(state, "ready") != 0 && strcmp(state, "starting") != 0))
    {
      return -ESRCH;              /* Not running: nobody connected */
    }

  return kill((pid_t)pid, SIGTERM) < 0 ? -errno : 0;
#else
  return -ENOSYS;
#endif
}

/* Every computer let in forgotten, once the owner says yes */

static int adbkeys_forget(void)
{
  FAR struct adbkeys_s *keys;
  char answer[8];
  bool yes;
  int count;
  int ret;
  int c;

  keys = malloc(sizeof(*keys));
  if (keys == NULL)
    {
      fprintf(stderr, "adbkeys: no memory\n");
      return EXIT_FAILURE;
    }

  adbkeys_read(ADBKEYS_FILE, keys);
  count = keys->all;
  free(keys);
  if (count == 0)
    {
      printf("No computer is let in\n");
      return EXIT_SUCCESS;
    }

  printf("Forget the %d computer%s let in? [y/N] ", count,
         count == 1 ? "" : "s");
  fflush(stdout);
  answer[0] = '\0';
  yes = fgets(answer, sizeof(answer), stdin) != NULL &&
        (answer[0] == 'y' || answer[0] == 'Y');
  if (answer[0] != '\0' && strchr(answer, '\n') == NULL)
    {
      /* A longer answer read to its end, not left for the shell */

      while ((c = getchar()) != EOF && c != '\n')
        {
        }
    }

  if (!yes)
    {
      printf("Kept\n");
      return EXIT_SUCCESS;
    }

  if (unlink(ADBKEYS_FILE) < 0 && errno != ENOENT)
    {
      fprintf(stderr, "adbkeys: cannot remove %s: %d\n", ADBKEYS_FILE,
              -errno);
      return EXIT_FAILURE;
    }

  /* Computers still connected let go too */

  ret = adbkeys_restart();
  if (ret == 0)
    {
      printf("Forgotten: none is let in, and adbd started again\n");
    }
  else if (ret == -ESRCH)
    {
      printf("Forgotten: none is let in\n");
    }
  else
    {
      printf("Forgotten: none gets in again; a computer still connected "
             "stays until adbd starts again: %d\n", ret);
    }

  syslog(LOG_NOTICE, "adbkeys: the %d computers let in forgotten\n",
         count);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR struct adbkeys_s *keys;
  FAR char *end;
  long n;

  if (argc == 1)
    {
      keys = malloc(sizeof(*keys));
      if (keys == NULL)
        {
          fprintf(stderr, "adbkeys: no memory\n");
          return EXIT_FAILURE;
        }

      adbkeys_read(ADBKEYS_FILE, keys);
      adbkeys_show("Let in", keys);
      adbkeys_read(ADBKEYS_NEW, keys);
      adbkeys_show("Waiting", keys);
      free(keys);
      return EXIT_SUCCESS;
    }

  if (argc == 3 && strcmp(argv[1], "allow") == 0)
    {
      n = strtol(argv[2], &end, 10);
      if (*end != '\0' || n < 1 || n > ADBKEYS_MAX)
        {
          adbkeys_usage();
          return EXIT_FAILURE;
        }

      return adbkeys_allow((int)n);
    }

  if (argc == 2 && strcmp(argv[1], "clear") == 0)
    {
      if (unlink(ADBKEYS_NEW) < 0 && errno != ENOENT)
        {
          fprintf(stderr, "adbkeys: cannot remove %s: %d\n", ADBKEYS_NEW,
                  -errno);
          return EXIT_FAILURE;
        }

      printf("None waiting\n");
      return EXIT_SUCCESS;
    }

  if (argc == 2 && strcmp(argv[1], "forget") == 0)
    {
      return adbkeys_forget();
    }

  adbkeys_usage();
  return EXIT_FAILURE;
}
