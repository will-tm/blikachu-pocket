/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_CORE_H
#define BLIKACHU_LIBRETRO_CORE_H

#include <libretro.h>
#include "content.h"

/* Use the ABI declarations in libretro.h for both prototypes and pointers. */
#define LR_CORE_FUNCTIONS(X) \
	X(init) X(deinit) X(api_version) X(get_system_info) X(get_system_av_info) \
	X(set_environment) X(set_video_refresh) X(set_audio_sample) \
	X(set_audio_sample_batch) X(set_input_poll) X(set_input_state) \
	X(set_controller_port_device) X(reset) X(run) X(serialize_size) \
	X(serialize) X(unserialize) X(load_game) X(unload_game) \
	X(get_region) X(get_memory_data) X(get_memory_size)

struct lr_core {
	const char *name;
#define LR_CORE_FIELD(name) __typeof__(&retro_##name) name;
	LR_CORE_FUNCTIONS(LR_CORE_FIELD)
#undef LR_CORE_FIELD
};

#ifdef __cplusplus
extern "C" {
#endif
const struct lr_core *lr_core_for_system(enum emu_system system);
#ifdef __cplusplus
}
#endif

#endif
