/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_CONTENT_H
#define BLIKACHU_LIBRETRO_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum emu_system {
	EMU_SYSTEM_UNKNOWN,
	EMU_SYSTEM_GB,
	EMU_SYSTEM_GBC,
	EMU_SYSTEM_NES,
	EMU_SYSTEM_SNES,
};

struct emu_content_info {
	enum emu_system system;
	/* Borrowed bytes; SNES copier headers are excluded. */
	const uint8_t *data;
	size_t size;
};

#ifdef __cplusplus
extern "C" {
#endif
bool emu_content_extension_supported(const char *path);
/* Headers select the system, even without a known extension. A recognized
 * extension that contradicts the header is rejected. Returns -EINVAL for
 * malformed/ambiguous content, -ENOTSUP for an unrecognized format.
 */
int emu_content_identify(const char *path, const uint8_t *data, size_t size,
			struct emu_content_info *info);
#ifdef __cplusplus
}
#endif

#endif
