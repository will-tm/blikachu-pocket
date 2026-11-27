/* SPDX-License-Identifier: Apache-2.0 */
#include "libretro/content.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void snes_header(uint8_t *p, bool hirom)
{
	p[0x15] = hirom ? 0x21 : 0x20;
	p[0x17] = 5;
	p[0x1c] = 0x34;
	p[0x1d] = 0x12;
	p[0x1e] = 0xcb;
	p[0x1f] = 0xed;
	p[0x3d] = 0x80;
}

int main(void)
{
	struct emu_content_info info;
	uint8_t *rom = calloc(1, 0x20000 + 512);

	assert(rom);
	assert(emu_content_extension_supported("/NAND:/Game.GBC"));
	assert(emu_content_extension_supported("game.SMC"));
	assert(!emu_content_extension_supported("/folder.nes/game"));
	assert(!emu_content_extension_supported("game.gbc.state"));
	assert(!emu_content_extension_supported(NULL));
	assert(emu_content_identify("bad.nes", NULL, 0, &info) == -EINVAL);
	assert(emu_content_identify("bad.nes", rom, 1, NULL) == -EINVAL);
	assert(emu_content_identify("unknown", rom, 0x8000, &info) == -ENOTSUP);
	assert(emu_content_identify("game.gb", rom, 0x8000, &info) == -EINVAL);

	memcpy(rom, "NES\x1a", 4);
	rom[4] = 1;
	assert(emu_content_identify("game.NES", rom, 16 + 16384, &info) == 0);
	assert(info.system == EMU_SYSTEM_NES && info.data == rom);
	assert(emu_content_identify("unknown", rom, 16 + 16384, &info) == 0);
	assert(emu_content_identify("wrong.gbc", rom, 16 + 16384, &info) == -EINVAL);
	assert(info.system == EMU_SYSTEM_UNKNOWN && !info.data && !info.size);
	for (size_t n = 0; n < 16; ++n) {
		assert(emu_content_identify("game.nes", rom, n, &info) == -EINVAL);
	}
	assert(emu_content_identify("game.nes", rom, 16384, &info) == -EINVAL);
	rom[6] = 4;
	assert(emu_content_identify("game.nes", rom, 16 + 16384, &info) == -EINVAL);
	assert(emu_content_identify("game.nes", rom, 16 + 512 + 16384, &info) == 0);
	rom[6] = 0;
	rom[7] = 8;
	rom[9] = 15;
	rom[4] = 14 << 2;
	assert(emu_content_identify("game.nes", rom, 16 + 16384, &info) == 0);
	rom[4] = 255;
	assert(emu_content_identify("game.nes", rom, 16 + 16384, &info) == -EINVAL);

	memset(rom, 0, 0x20000 + 512);
	snes_header(rom + 0x7fc0, false);
	assert(emu_content_identify("game.sfc", rom, 0x8000, &info) == 0);
	assert(info.system == EMU_SYSTEM_SNES && info.size == 0x8000);
	assert(emu_content_identify("no-extension", rom, 0x8000, &info) == 0);
	assert(emu_content_identify("game.sfc", rom, 0x7fff, &info) == -EINVAL);
	snes_header(rom + 0xffc0, true);
	assert(emu_content_identify("ambiguous.sfc", rom, 0x10000, &info) == -EINVAL);
	memset(rom + 0x7fc0, 0, 64);
	assert(emu_content_identify("hirom.sfc", rom, 0x10000, &info) == 0);
	memmove(rom + 512, rom, 0x10000);
	memset(rom, 0, 512);
	assert(emu_content_identify("header.smc", rom, 0x10200, &info) == 0);
	assert(info.data == rom + 512 && info.size == 0x10000);
	rom[512 + 0xffdc] ^= 1;
	assert(emu_content_identify("broken.smc", rom, 0x10200, &info) == -EINVAL);

	/* Public boot-logo bytes and a generated, checksum-valid header. */
	static const uint8_t logo[48] = {
		0xce,0xed,0x66,0x66,0xcc,0x0d,0x00,0x0b,0x03,0x73,0x00,0x83,
		0x00,0x0c,0x00,0x0d,0x00,0x08,0x11,0x1f,0x88,0x89,0x00,0x0e,
		0xdc,0xcc,0x6e,0xe6,0xdd,0xdd,0xd9,0x99,0xbb,0xbb,0x67,0x63,
		0x6e,0x0e,0xec,0xcc,0xdd,0xdc,0x99,0x9f,0xbb,0xb9,0x33,0x3e,
	};
	memset(rom, 0, 0x20000 + 512);
	memcpy(rom + 0x104, logo, sizeof(logo));
	rom[0x14d] = (uint8_t)-25;
	assert(emu_content_identify("test.gb", rom, 0x8000, &info) == 0);
	assert(info.system == EMU_SYSTEM_GB);
	rom[0x143] = 0x80;
	rom[0x14d] -= 0x80;
	assert(emu_content_identify("test.gbc", rom, 0x8000, &info) == 0);
	assert(info.system == EMU_SYSTEM_GBC);
	assert(emu_content_identify("test.gb", rom, 0x8000, &info) == 0);
	assert(emu_content_identify(NULL, rom, 0x8000, &info) == 0);
	rom[0x14d] ^= 1;
	assert(emu_content_identify("broken.gb", rom, 0x8000, &info) == -EINVAL);

	free(rom);
	puts("libretro content detection: passed");
	return 0;
}
