/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_WORKSPACE_H
#define BLIKACHU_LIBRETRO_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

#define LR_WORKSPACE_BYTES (3584U * 1024U)
#define LR_STAGING_BYTES LR_WORKSPACE_BYTES

enum lr_workspace_owner {
	LR_WORKSPACE_FREE,
	LR_WORKSPACE_CORE,
	LR_WORKSPACE_STAGING,
};

/* Nonblocking, exclusive, and nonrecursive. Core teardown must precede
 * releasing its claim; a paused core still owns its working memory.
 */
void *lr_workspace_claim(enum lr_workspace_owner owner);
bool lr_workspace_release(enum lr_workspace_owner owner);
/* The caller must already hold a claim before accessing these bytes. */
void *lr_workspace_data(void);
bool lr_workspace_core_owned(void);

#endif
