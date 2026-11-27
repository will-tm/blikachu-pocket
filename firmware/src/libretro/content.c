/* SPDX-License-Identifier: Apache-2.0 */
#include "content.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

static enum emu_system extension_system(const char *path)
{
	const char *name = path ? strrchr(path, '/') : NULL;
	const char *dot = path ? strrchr(name ? name + 1 : path, '.') : NULL;

	if (!dot) {
		return EMU_SYSTEM_UNKNOWN;
	}
	if (!strcasecmp(dot, ".gb")) {
		return EMU_SYSTEM_GB;
	}
	if (!strcasecmp(dot, ".gbc")) {
		return EMU_SYSTEM_GBC;
	}
	if (!strcasecmp(dot, ".nes")) {
		return EMU_SYSTEM_NES;
	}
	if (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc")) {
		return EMU_SYSTEM_SNES;
	}
	return EMU_SYSTEM_UNKNOWN;
}

bool emu_content_extension_supported(const char *path)
{
	return extension_system(path) != EMU_SYSTEM_UNKNOWN;
}

static bool gb_header(const uint8_t *data, size_t size)
{
	static const uint8_t logo[] = {
		0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b,
		0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
		0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e,
		0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
		0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc,
		0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,
	};
	uint8_t checksum = 0;

	if (size < 0x8000 || memcmp(data + 0x104, logo, sizeof(logo))) {
		return false;
	}
	for (size_t i = 0x134; i <= 0x14c; ++i) {
		checksum = checksum - data[i] - 1;
	}
	return checksum == data[0x14d];
}

static bool nes_size(uint8_t low, unsigned high, size_t unit, size_t *size)
{
	if (high != 0x0f) {
		*size = ((size_t)high * 256 + low) * unit;
		return true;
	}
	unsigned exponent = low >> 2;
	size_t multiplier = (low & 3) * 2 + 1;

	if (exponent >= sizeof(size_t) * CHAR_BIT ||
	    multiplier > (SIZE_MAX >> exponent)) {
		return false;
	}
	*size = multiplier << exponent;
	return true;
}

static bool nes_header(const uint8_t *data, size_t size)
{
	size_t prg, chr;
	bool nes2;

	if (size < 16) {
		return false;
	}
	nes2 = (data[7] & 0x0c) == 0x08;
	if (!nes_size(data[4], nes2 ? data[9] & 15 : 0, 16384, &prg) ||
	    !nes_size(data[5], nes2 ? data[9] >> 4 : 0, 8192, &chr) || !prg) {
		return false;
	}
	size -= 16;
	if (data[6] & 4) {
		if (size < 512) {
			return false;
		}
		size -= 512;
	}
	return prg <= size && chr <= size - prg;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static bool snes_header(const uint8_t *data, size_t size, size_t offset,
			bool hirom)
{
	if (offset > size || size - offset < 64) {
		return false;
	}
	const uint8_t *h = data + offset;
	unsigned mode = h[0x15];

	/* Ordinary LoROM/HiROM, with either slow or fast bus timing. */
	return (mode == (hirom ? 0x21 : 0x20) || mode == (hirom ? 0x31 : 0x30)) &&
	       h[0x17] >= 5 && h[0x17] <= 13 &&
	       (le16(h + 0x1c) ^ le16(h + 0x1e)) == 0xffff &&
	       le16(h + 0x3c) >= 0x8000 && le16(h + 0x3c) != 0xffff;
}

int emu_content_identify(const char *path, const uint8_t *data, size_t size,
			struct emu_content_info *info)
{
	enum emu_system hint = extension_system(path);
	struct emu_content_info found = { .data = data, .size = size };

	if (!info) {
		return -EINVAL;
	}
	memset(info, 0, sizeof(*info));
	if (!data || !size) {
		return -EINVAL;
	}
	if (size >= 4 && !memcmp(data, "NES\x1a", 4)) {
		if (!nes_header(data, size)) {
			return -EINVAL;
		}
		found.system = EMU_SYSTEM_NES;
	} else if (gb_header(data, size)) {
		found.system = data[0x143] & 0x80 ? EMU_SYSTEM_GBC : EMU_SYSTEM_GB;
	} else {
		size_t header = size % 0x8000 == 512 ? 512 : 0;
		size_t bytes = size - header;

		if (bytes >= 0x8000 && bytes <= 8 * 1024 * 1024 && !(bytes % 0x8000)) {
			bool lo = snes_header(data + header, bytes, 0x7fc0, false);
			bool hi = snes_header(data + header, bytes, 0xffc0, true);

			if (lo && hi) {
				return -EINVAL;
			}
			if (lo || hi) {
				found.system = EMU_SYSTEM_SNES;
				found.data += header;
				found.size = bytes;
			}
		}
	}
	if (found.system == EMU_SYSTEM_UNKNOWN) {
		return hint == EMU_SYSTEM_UNKNOWN ? -ENOTSUP : -EINVAL;
	}
	bool gb_family = (hint == EMU_SYSTEM_GB || hint == EMU_SYSTEM_GBC) &&
		(found.system == EMU_SYSTEM_GB || found.system == EMU_SYSTEM_GBC);

	if (hint != EMU_SYSTEM_UNKNOWN && hint != found.system && !gb_family) {
		return -EINVAL;
	}
	*info = found;
	return 0;
}
