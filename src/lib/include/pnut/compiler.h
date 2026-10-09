/****************************************************************************
 * pnut-os/src/lib/include/pnut/compiler.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_COMPILER_H
#define __PNUT_OS_LIB_PNUT_COMPILER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#ifdef __NuttX__
#  include <nuttx/compiler.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* What NuttX's compiler.h provides, for the build on the computer */

#ifndef __NuttX__
#  define FAR
#  define CODE
#  define printf_like(a, b) __attribute__((format(printf, a, b)))
#endif

#ifndef OK
#  define OK 0
#endif

#endif /* __PNUT_OS_LIB_PNUT_COMPILER_H */
