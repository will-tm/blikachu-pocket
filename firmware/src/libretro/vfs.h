/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_VFS_H
#define BLIKACHU_LIBRETRO_VFS_H
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
const struct retro_vfs_interface *lr_vfs_interface(void);
/* Close OS handles before releasing the core's allocation arena. */
void lr_vfs_close_all(void);
#ifdef __cplusplus
}
#endif
#endif
