/****************************************************************************
 * pnut-os/src/ui/wifilist.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The networks in reach, as the Wi-Fi screen lists them: a scan reports
 * every access point; the list keeps one entry a network (its name and
 * security), with its strongest signal, the strongest first.  While the
 * scan goes on the list is redrawn at most every 5 s (the design).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "wifilist.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Move entry i up while it is stronger than the one above */

static void wifilist_rise(FAR struct wifilist_s *l, int i)
{
  pnut_wifi_network_t n;
  bool k;

  while (i > 0 && l->nets[i].signal > l->nets[i - 1].signal)
    {
      n = l->nets[i - 1];
      k = l->known[i - 1];
      l->nets[i - 1]  = l->nets[i];
      l->known[i - 1] = l->known[i];
      l->nets[i]  = n;
      l->known[i] = k;
      i--;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void wifilist_start(FAR struct wifilist_s *l, uint64_t now)
{
  memset(l, 0, sizeof(*l));
  l->shown    = now;
  l->scanning = true;
}

void wifilist_add(FAR struct wifilist_s *l,
                  FAR const pnut_wifi_network_t *n)
{
  int i;

  if (n->name[0] == '\0')
    {
      return;
    }

  for (i = 0; i < l->count; i++)
    {
      if (l->nets[i].security == n->security &&
          strcmp(l->nets[i].name, n->name) == 0)
        {
          if (n->signal > l->nets[i].signal)
            {
              l->nets[i].signal = n->signal;
              wifilist_rise(l, i);
              l->dirty = true;
            }

          return;
        }
    }

  /* Full: the weakest makes room, if this one is stronger */

  if (l->count == WIFILIST_MAX)
    {
      if (n->signal <= l->nets[WIFILIST_MAX - 1].signal)
        {
          return;
        }

      l->count--;
    }

  l->nets[l->count]  = *n;
  l->known[l->count] = false;
  wifilist_rise(l, l->count++);
  l->dirty = true;
}

void wifilist_known(FAR struct wifilist_s *l,
                    FAR const pnut_wifi_known_networks_t *k)
{
  bool known;
  int i;
  int j;

  for (i = 0; i < l->count; i++)
    {
      known = false;
      for (j = 0; j < k->networks_count && !known; j++)
        {
          known = k->networks[j].security == l->nets[i].security &&
                  strcmp(k->networks[j].name, l->nets[i].name) == 0;
        }

      if (known != l->known[i])
        {
          l->known[i] = known;
          l->dirty    = true;
        }
    }
}

void wifilist_end(FAR struct wifilist_s *l)
{
  l->scanning = false;
  l->dirty    = true;
}

bool wifilist_due(FAR const struct wifilist_s *l, uint64_t now)
{
  return l->dirty && (!l->scanning || !l->drawn ||
                      now - l->shown >= WIFILIST_EVERY_MS);
}

void wifilist_shown(FAR struct wifilist_s *l, uint64_t now)
{
  l->shown = now;
  l->drawn = l->drawn || l->count > 0;
  l->dirty = false;
}
