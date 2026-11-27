/* SPDX-License-Identifier: Apache-2.0 */
#include "core.h"

#define LR_DECLARE(name) extern __typeof__(retro_##name) lr_gambatte_##name;
LR_CORE_FUNCTIONS(LR_DECLARE)
#undef LR_DECLARE
#define LR_DECLARE(name) extern __typeof__(retro_##name) lr_fceumm_##name;
LR_CORE_FUNCTIONS(LR_DECLARE)
#undef LR_DECLARE
#define LR_DECLARE(name) extern __typeof__(retro_##name) lr_snes9x2005_##name;
LR_CORE_FUNCTIONS(LR_DECLARE)
#undef LR_DECLARE

static const struct lr_core gambatte = {
	.name = "Gambatte",
#define LR_FUNCTION(name) .name = lr_gambatte_##name,
	LR_CORE_FUNCTIONS(LR_FUNCTION)
#undef LR_FUNCTION
};
static const struct lr_core fceumm = {
	.name = "FCEUmm",
#define LR_FUNCTION(name) .name = lr_fceumm_##name,
	LR_CORE_FUNCTIONS(LR_FUNCTION)
#undef LR_FUNCTION
};
static const struct lr_core snes9x2005 = {
	.name = "Snes9x2005",
#define LR_FUNCTION(name) .name = lr_snes9x2005_##name,
	LR_CORE_FUNCTIONS(LR_FUNCTION)
#undef LR_FUNCTION
};

const struct lr_core *lr_core_for_system(enum emu_system system)
{
	switch (system) {
	case EMU_SYSTEM_GB:
	case EMU_SYSTEM_GBC:
		return &gambatte;
	case EMU_SYSTEM_NES:
		return &fceumm;
	case EMU_SYSTEM_SNES:
		return &snes9x2005;
	default:
		return NULL;
	}
}
