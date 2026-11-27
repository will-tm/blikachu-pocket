/* SPDX-License-Identifier: Apache-2.0 */
#include "libretro/workspace.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

static void *exercise(void *arg)
{
	enum lr_workspace_owner owner = (enum lr_workspace_owner)(uintptr_t)arg;

	for (unsigned i = 0; i < 10000; ++i) {
		uint8_t *p;

		while (!(p = lr_workspace_claim(owner))) {
			sched_yield();
		}
		assert(lr_workspace_core_owned() == (owner == LR_WORKSPACE_CORE));
		p[0] = (uint8_t)owner;
		p[LR_WORKSPACE_BYTES - 1] = (uint8_t)owner;
		sched_yield();
		assert(p[0] == owner && p[LR_WORKSPACE_BYTES - 1] == owner);
		assert(lr_workspace_release(owner));
	}
	return NULL;
}

int main(void)
{
	assert(!lr_workspace_claim(LR_WORKSPACE_FREE));
	assert(!lr_workspace_release(LR_WORKSPACE_FREE));
	assert(!lr_workspace_release(LR_WORKSPACE_CORE));
	assert(LR_STAGING_BYTES <= LR_WORKSPACE_BYTES);
	void *p = lr_workspace_claim(LR_WORKSPACE_CORE);

	assert(p && p == lr_workspace_data() && !((uintptr_t)p & 31));
	assert(!lr_workspace_claim(LR_WORKSPACE_CORE));
	assert(!lr_workspace_claim(LR_WORKSPACE_STAGING));
	assert(!lr_workspace_release(LR_WORKSPACE_STAGING));
	assert(lr_workspace_release(LR_WORKSPACE_CORE));
	assert(lr_workspace_claim(LR_WORKSPACE_STAGING) == p);
	assert(lr_workspace_release(LR_WORKSPACE_STAGING));
	pthread_t core, update;

	assert(!pthread_create(&core, NULL, exercise, (void *)(uintptr_t)LR_WORKSPACE_CORE));
	assert(!pthread_create(&update, NULL, exercise, (void *)(uintptr_t)LR_WORKSPACE_STAGING));
	assert(!pthread_join(core, NULL));
	assert(!pthread_join(update, NULL));
	puts("libretro workspace: ownership and 20000 concurrent claims passed");
	return 0;
}
