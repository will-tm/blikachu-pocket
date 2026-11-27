/*
 * `bench` — peanut-gb CPU benchmark for the BL618 bring-up build.
 *
 * Answers "is 320 MHz enough?" with the same emulator configuration as
 * the real firmware (ENABLE_SOUND + ENABLE_LCD + full CGB support,
 * minigb_apu per-frame sample generation, and the same 1.5x indexed →
 * RGB565 scaler emu.c uses). The ROM is embedded at build time via the
 * BENCH_ROM env var (see CMakeLists.txt) so it executes from XIP flash
 * exactly like a ROM launched from /NAND: does on the real firmware;
 * `bench run <frames> psram` re-runs it from a PSRAM copy to separate
 * CPU cost from flash-fetch cost.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "storage.h"

#define ENABLE_SOUND            1
#define ENABLE_LCD              1
#define PEANUT_FULL_GBC_SUPPORT 1

/* No SRAM code placement on this port yet — measure the honest
 * XIP-resident baseline first.
 */
#define __not_in_flash_func(name) name

/* Stub for fhoedemakers's HSTX-display helper that we never read. */
#define RGB555_TO_RGB444(x) 0

#include "minigb_apu.h"
static struct minigb_apu_ctx g_apu;
static audio_sample_t g_apu_buf[AUDIO_SAMPLES_TOTAL];

static uint8_t audio_read(const uint16_t addr)
{
	return minigb_apu_audio_read(&g_apu, addr);
}

static void audio_write(const uint16_t addr, const uint8_t val)
{
	minigb_apu_audio_write(&g_apu, addr, val);
}

#include "peanut_gb.h"

#ifdef HAVE_BENCH_ROM
extern const uint8_t bench_rom_start[];
extern const uint8_t bench_rom_end[];
#endif

#define GB_W 160
#define GB_H 144
#define SCALE_W 240
#define SCALE_H 216

#define CART_RAM_MAX (32 * 1024)

static struct gb_s gb;
static uint8_t g_cart_ram[CART_RAM_MAX];
static uint8_t g_idx_fb[GB_H][GB_W];
static uint16_t g_fb565[SCALE_H][SCALE_W];
static uint16_t g_pal[0x40];
static const uint8_t *g_rom;
static jmp_buf g_bench_jmp;

static uint8_t cb_rom_read(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	return g_rom[addr];
}

static uint8_t cb_ram_read(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	return g_cart_ram[addr];
}

static void cb_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val)
{
	(void)g;
	g_cart_ram[addr] = val;
}

static void cb_error(struct gb_s *g, const enum gb_error_e e, const uint16_t addr)
{
	(void)g;
	(void)addr;
	longjmp(g_bench_jmp, (int)e + 1);
}

static void cb_lcd_line(struct gb_s *g, const uint8_t pixels[160], const uint_fast8_t line)
{
	(void)g;
	if (line < GB_H) {
		memcpy(g_idx_fb[line], pixels, GB_W);
	}
}

/* Same conversion + scaler shape as emu.c (cgb555_to_be565 +
 * scale_15x_row word-packing).
 */
static inline uint16_t cgb555_to_be565(uint16_t src)
{
	uint16_t r5 = (src >> 10) & 0x1F;
	uint16_t g5 = (src >> 5) & 0x1F;
	uint16_t b5 = src & 0x1F;
	uint16_t g6 = (g5 << 1) | (g5 >> 4);
	return __builtin_bswap16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
}

static void scale_15x_row(const uint8_t *src, const uint16_t *pal, uint16_t *dst)
{
	uint32_t *dst32 = (uint32_t *)dst;
	int x_in = 0;

	for (int x_out = 0; x_out < SCALE_W; x_out += 6) {
		uint32_t a = pal[src[x_in]];
		uint32_t b = pal[src[x_in + 1]];
		uint32_t c = pal[src[x_in + 2]];
		uint32_t d = pal[src[x_in + 3]];

		*dst32++ = a | (a << 16);
		*dst32++ = b | (c << 16);
		*dst32++ = c | (d << 16);
		x_in += 4;
	}
}

static void scale_frame(void)
{
	if (gb.cgb.cgbMode) {
		for (int i = 0; i < 0x40; i++) {
			g_pal[i] = cgb555_to_be565(gb.cgb.fixPalette[i]);
		}
	} else {
		static const uint16_t dmg[4] = {0xFFFF, 0xB5D6, 0x52AA, 0x0000};

		for (int i = 0; i < 0x40; i++) {
			g_pal[i] = dmg[i & 3];
		}
	}

	int y_out = 0;

	for (int y = 0; y < GB_H; y += 2, y_out += 3) {
		scale_15x_row(g_idx_fb[y], g_pal, g_fb565[y_out]);
		memcpy(g_fb565[y_out + 1], g_fb565[y_out], SCALE_W * sizeof(uint16_t));
		scale_15x_row(g_idx_fb[y + 1], g_pal, g_fb565[y_out + 2]);
	}
}

static int bench_pass(const struct shell *sh, const char *tag, uint32_t frames, bool scale)
{
	int jmp;

	memset(g_cart_ram, 0xFF, sizeof(g_cart_ram));
	minigb_apu_audio_init(&g_apu);

	jmp = setjmp(g_bench_jmp);
	if (jmp != 0) {
		shell_error(sh, "%s: emulation fault %d at frame — aborted", tag, jmp - 1);
		return -EIO;
	}

	if (gb_init(&gb, cb_rom_read, cb_ram_read, cb_ram_write, cb_error, NULL) != GB_INIT_NO_ERROR) {
		shell_error(sh, "gb_init failed");
		return -EIO;
	}
	gb_init_lcd(&gb, cb_lcd_line);

	/* Warm-up: get past the boot logo & into the intro so the timed
	 * window measures real rendering, and tap START occasionally so
	 * the intro keeps advancing instead of idling on a static title.
	 */
	for (int i = 0; i < 120; i++) {
		gb.direct.joypad = (i % 90 < 5) ? (uint8_t)~0x08 : 0xFF; /* START */
		gb_run_frame(&gb);
		minigb_apu_audio_callback(&g_apu, g_apu_buf);
	}

	int64_t t0 = k_uptime_get();

	for (uint32_t i = 0; i < frames; i++) {
		gb.direct.joypad = (i % 90 < 5) ? (uint8_t)~0x08 : 0xFF; /* START */
		gb_run_frame(&gb);
		minigb_apu_audio_callback(&g_apu, g_apu_buf);
		if (scale) {
			scale_frame();
		}
	}

	int64_t ms = k_uptime_get() - t0;
	uint32_t fps_x10 = (uint32_t)((uint64_t)frames * 10000ULL / (uint64_t)ms);

	shell_print(sh, "%-18s %5u frames in %6lld ms -> %3u.%u fps %s",
		    tag, frames, ms, fps_x10 / 10, fps_x10 % 10,
		    (fps_x10 >= 597) ? "(>= 59.7: OK)" : "(< 59.7: TOO SLOW)");
	return 0;
}

static int cmd_bench_run(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t frames = 600;
	bool use_psram = false;
	const char *path = NULL;
	size_t rom_size = 0;

	static char pathbuf[96];

	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "psram") == 0) {
			use_psram = true;
		} else if (argv[i][0] == '/') {
			/* Join the remaining args: unquoted paths with
			 * spaces ("/NAND:/Tetris DX.gbc") arrive split.
			 */
			pathbuf[0] = '\0';
			for (size_t j = i; j < argc; j++) {
				if (j > i) {
					strncat(pathbuf, " ", sizeof(pathbuf) - strlen(pathbuf) - 1);
				}
				strncat(pathbuf, argv[j], sizeof(pathbuf) - strlen(pathbuf) - 1);
			}
			path = pathbuf;
			break;
		} else {
			frames = (uint32_t)strtoul(argv[i], NULL, 0);
		}
	}
	if (frames == 0) {
		frames = 600;
	}

	if (path != NULL) {
		/* Same flow as the real firmware's `emu run`: resolve the
		 * FATFS file to its XIP window, defrag + retry if the
		 * cluster chain isn't contiguous.
		 */
		const char *err = NULL;

		g_rom = resolve_file_xip(path, &rom_size, &err);
		if (g_rom == NULL && err == STORAGE_ERR_FRAGMENTED) {
			shell_print(sh, "%s is fragmented — defragging...", path);
			if (storage_defrag_file(path) == 0) {
				g_rom = resolve_file_xip(path, &rom_size, &err);
			}
		}
		if (g_rom == NULL) {
			shell_error(sh, "%s: %s", path, err ? err : "resolve failed");
			return -EIO;
		}
	} else {
#ifdef HAVE_BENCH_ROM
		g_rom = bench_rom_start;
		rom_size = (size_t)(bench_rom_end - bench_rom_start);
#else
		shell_error(sh, "no ROM embedded — pass a /NAND: path");
		return -EINVAL;
#endif
	}

	if (use_psram) {
		uint8_t *psram = (uint8_t *)DT_REG_ADDR(DT_NODELABEL(psram));
		const uint8_t *src = g_rom;

		shell_print(sh, "copying ROM (%u KiB) to PSRAM...", (unsigned)(rom_size / 1024));
		memcpy(psram, src, rom_size);
		if (memcmp(psram, src, rom_size) != 0) {
			shell_error(sh, "PSRAM copy verify failed");
			return -EIO;
		}
		g_rom = psram;
	}

	shell_print(sh, "ROM: %.16s  %u KiB  cgb=0x%02x  src=%s%s",
		    (const char *)&g_rom[0x134],
		    (unsigned)(rom_size / 1024), g_rom[0x143],
		    use_psram ? "PSRAM" : "XIP flash",
		    path ? " (FATFS)" : "");

	int err = bench_pass(sh, "emu+apu", frames, false);

	if (err == 0) {
		err = bench_pass(sh, "emu+apu+scale", frames, true);
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bench,
	SHELL_CMD_ARG(run, NULL, "bench run [frames] [psram] [/NAND:/rom.gb]", cmd_bench_run, 1, 8),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(bench, &sub_bench, "peanut-gb benchmark", NULL);

