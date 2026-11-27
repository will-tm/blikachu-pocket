/* SPDX-License-Identifier: Apache-2.0 */
#include "workspace.h"

#include <stdatomic.h>
#include <stdint.h>

#if defined(CONFIG_SOC_SERIES_BL61X)
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(core_ram)) == LR_WORKSPACE_BYTES);
extern uint8_t __lr_workspace_start[];
#define workspace __lr_workspace_start
#else
static _Alignas(32) uint8_t workspace[LR_WORKSPACE_BYTES];
#endif
static atomic_int owner;

bool lr_workspace_core_owned(void)
{
	return atomic_load(&owner) == LR_WORKSPACE_CORE;
}

void *lr_workspace_data(void)
{
	return workspace;
}

void *lr_workspace_claim(enum lr_workspace_owner next)
{
	int expected = LR_WORKSPACE_FREE;

	if (next != LR_WORKSPACE_CORE && next != LR_WORKSPACE_STAGING) {
		return NULL;
	}
	return atomic_compare_exchange_strong(&owner, &expected, next) ? workspace : NULL;
}

bool lr_workspace_release(enum lr_workspace_owner previous)
{
	int expected = previous;

	if (previous != LR_WORKSPACE_CORE && previous != LR_WORKSPACE_STAGING) {
		return false;
	}
	return atomic_compare_exchange_strong(&owner, &expected, LR_WORKSPACE_FREE);
}
