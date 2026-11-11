/****************************************************************************
 * pnut-os/src/network/netstack.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_NETWORK_NETSTACK_H
#define __PNUT_OS_NETWORK_NETSTACK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "network.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's network, as network.h's operations; their arg unused */

extern const struct network_ops_s g_netstack_ops;

#endif /* __PNUT_OS_NETWORK_NETSTACK_H */
