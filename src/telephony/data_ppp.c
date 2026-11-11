/****************************************************************************
 * pnut-os/src/telephony/data_ppp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Mobile data's link on the device: NuttX's pppd on a thread of its own
 * (RFC 0030 lets it bring one), on the pseudo-terminal Telephony gives it.
 * Telephony has dialled already, so pppd gets no chat script; it tells of
 * the link through its notify hook, and that goes to Telephony's loop as
 * records on a pipe.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

/* The device's only: the tests on the computer give a fake link */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#ifdef CONFIG_NETUTILS_PPPD

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netutils/netlib.h>
#include <netutils/pppd.h>

#include "data.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_PNUT_TELEPHONY_PPP_STACKSIZE
#  define CONFIG_PNUT_TELEPHONY_PPP_STACKSIZE 6144
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* stop and running are shared with pppd's thread: stored and loaded with
 * the compiler's atomics (pppd reads stop through its volatile pointer)
 */

struct data_ppp_s
{
  struct pppd_settings_s settings;
  volatile bool stop;
  bool running;                     /* The thread has not ended yet */
  int fd;                           /* Telephony's pipe */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int data_ppp_start(FAR void *arg, FAR const char *tty, int fd);
static void data_ppp_stop(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One link at a time: Telephony starts the next only after the last one's
 * DATA_LINK_ENDED
 */

static struct data_ppp_s g_ppp;

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct data_link_ops_s g_data_ppp =
{
  data_ppp_start,
  data_ppp_stop,
  &g_ppp
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: data_ppp_tell
 *
 * Description:
 *   A record to Telephony's loop: smaller than a pipe's atomic write.
 *
 ****************************************************************************/

static void data_ppp_tell(FAR struct data_ppp_s *ppp,
                          FAR const struct data_event_s *ev)
{
  ssize_t n;

  do
    {
      n = write(ppp->fd, ev, sizeof(*ev));
    }
  while (n < 0 && errno == EINTR);
}

/****************************************************************************
 * Name: data_ppp_notify
 *
 * Description:
 *   pppd's notice, on its thread.  The interface gets a host-only netmask
 *   before Telephony hears of it: until Network chooses the link (RFC
 *   0015), Wi-Fi then keeps the traffic while it is up.
 *
 ****************************************************************************/

static void data_ppp_notify(FAR void *arg, enum pppd_event_e event,
                            FAR const struct pppd_link_s *link)
{
  FAR struct data_ppp_s *ppp = arg;
  struct data_event_s ev;
  struct in_addr mask;

  memset(&ev, 0, sizeof(ev));
  if (event == PPPD_LINK_UP && link != NULL)
    {
      mask.s_addr = INADDR_BROADCAST;
      netlib_set_ipv4netmask(link->ifname, &mask);

      ev.event = DATA_LINK_UP;
      strlcpy(ev.ifname, link->ifname, sizeof(ev.ifname));
      ev.local  = link->local;
      ev.peer   = link->peer;
      ev.dns[0] = link->dns[0];
      ev.dns[1] = link->dns[1];
    }
  else
    {
      ev.event = DATA_LINK_DOWN;
    }

  data_ppp_tell(ppp, &ev);
}

/****************************************************************************
 * Name: data_ppp_thread
 ****************************************************************************/

static FAR void *data_ppp_thread(FAR void *arg)
{
  FAR struct data_ppp_s *ppp = arg;
  struct data_event_s ev;

  pppd(&ppp->settings);
  __atomic_store_n(&ppp->running, false, __ATOMIC_RELEASE);

  memset(&ev, 0, sizeof(ev));
  ev.event = DATA_LINK_ENDED;
  data_ppp_tell(ppp, &ev);
  return NULL;
}

/****************************************************************************
 * Name: data_ppp_start
 ****************************************************************************/

static int data_ppp_start(FAR void *arg, FAR const char *tty, int fd)
{
  FAR struct data_ppp_s *ppp = arg;
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  if (strlen(tty) >= sizeof(ppp->settings.ttyname))
    {
      return -ENAMETOOLONG;
    }

  /* One link at a time: one Telephony gave up waiting for still runs */

  if (__atomic_load_n(&ppp->running, __ATOMIC_ACQUIRE))
    {
      return -EBUSY;
    }

  memset(&ppp->settings, 0, sizeof(ppp->settings));
  strlcpy(ppp->settings.ttyname, tty, sizeof(ppp->settings.ttyname));
  ppp->settings.quiet  = true;
  ppp->settings.stop   = &ppp->stop;
  ppp->settings.notify = data_ppp_notify;
  ppp->settings.arg    = ppp;
  ppp->fd              = fd;
  __atomic_store_n(&ppp->stop, false, __ATOMIC_RELAXED);
  __atomic_store_n(&ppp->running, true, __ATOMIC_RELAXED);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_PNUT_TELEPHONY_PPP_STACKSIZE);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&tid, &attr, data_ppp_thread, ppp);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      __atomic_store_n(&ppp->running, false, __ATOMIC_RELAXED);
      return -ret;
    }

  pthread_setname_np(tid, "pppd");
  return 0;
}

/****************************************************************************
 * Name: data_ppp_stop
 ****************************************************************************/

static void data_ppp_stop(FAR void *arg)
{
  FAR struct data_ppp_s *ppp = arg;

  __atomic_store_n(&ppp->stop, true, __ATOMIC_RELEASE);
}

#endif /* CONFIG_NETUTILS_PPPD */
