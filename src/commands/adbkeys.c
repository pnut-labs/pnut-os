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
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/stat.h>

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

/* A line's fingerprint into out (48 bytes and a NUL), or "not a key" */

static void adbkeys_fingerprint(FAR const char *line, FAR char *out)
{
  uint8_t key[ADBKEYS_KEY_BYTES];
  uint8_t md5[16];
  uint32_t bits = 0;
  size_t len = 0;
  int nbits = 0;
  int v;
  int i;

  for (; (v = adbkeys_b64(*line)) >= 0; line++)
    {
      bits   = (bits << 6) | (uint32_t)v;
      nbits += 6;
      if (nbits >= 8)
        {
          nbits -= 8;
          if (len >= sizeof(key))
            {
              len = 0;            /* Longer than a key */
              break;
            }

          key[len++] = (uint8_t)(bits >> nbits);
        }
    }

  if (len != sizeof(key))
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

/* Whether path holds this line already */

static bool adbkeys_has(FAR const char *path, FAR const char *line)
{
  FAR struct adbkeys_s *keys = malloc(sizeof(*keys));
  bool has = false;
  int i;

  if (keys == NULL)
    {
      return false;
    }

  adbkeys_read(path, keys);
  for (i = 0; i < keys->count && !has; i++)
    {
      has = strcmp(keys->line[i], line) == 0;
    }

  free(keys);
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
  FAR struct adbkeys_s *waiting;
  char fingerprint[49];
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

  if (!adbkeys_has(ADBKEYS_FILE, waiting->line[n - 1]))
    {
      len = strlen(waiting->line[n - 1]);
      waiting->line[n - 1][len] = '\n';
      st.st_size = -1;
      fd = open(ADBKEYS_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                0600);
      if (fd < 0 || fstat(fd, &st) < 0 ||
          adbkeys_write(fd, waiting->line[n - 1], len + 1) < 0 ||
          fsync(fd) < 0)
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
      waiting->line[n - 1][len] = '\0';
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

/* Every computer let in forgotten, once the owner says yes */

static int adbkeys_forget(void)
{
  FAR struct adbkeys_s *keys;
  char answer[8];
  bool yes;
  int count;
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

  printf("Forgotten: none is let in\n");
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
