/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIBRETRO_TEST_KERNEL_H
#define LIBRETRO_TEST_KERNEL_H
#include <stdio.h>
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define vsnprintk vsnprintf
#endif
