/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_LIFETIME_H
#define BLIKACHU_LIBRETRO_LIFETIME_H

#include "content.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Acquire/reset core memory and run its constructors. Call core deinit before
 * end(), which runs destructors and releases the shared workspace.
 */
int lr_lifetime_begin(enum emu_system system);
void lr_lifetime_end(void);
#ifdef __cplusplus
}
#endif
#endif
