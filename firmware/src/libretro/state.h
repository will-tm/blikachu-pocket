/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_STATE_H
#define BLIKACHU_LIBRETRO_STATE_H
#include "session.h"

/* Core/ROM-specific files are separate from legacy Peanut-GB saves. */
int lr_state_path(char *path, size_t capacity, const struct lr_session *session,
		  uint32_t rom_crc, const char *suffix);
int lr_state_file(struct lr_session *session, const char *path, uint32_t rom_crc, bool restore);
int lr_battery_file(struct lr_session *session, uint32_t rom_crc, bool restore);
#endif
