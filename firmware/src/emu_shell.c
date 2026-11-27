/*
 * Settings (`cfg`) and emulator (`gb`) shell commands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include "emu.h"
#include "gfx.h"
#include "hud.h"
#include "input.h"
#include "menu.h"
#include "settings.h"
#include "storage.h"

LOG_MODULE_REGISTER(emu_shell, LOG_LEVEL_INF);

/* ---- ROM management ---- */

static void shell_print_cb(void *ctx, const char *line)
{
	const struct shell *sh = ctx;
	shell_print(sh, "%s", line);
}

/* ---- `cfg` group: persisted key/value settings ---- */

static int cmd_cfg_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	settings_list(shell_print_cb, (void *)sh);
	return 0;
}

static int cmd_cfg_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char val[64];
	int rc = settings_get_str(argv[1], val, sizeof(val));

	if (rc) {
		shell_error(sh, "%s: %d", argv[1], rc);
		return rc;
	}
	shell_print(sh, "%s=%s", argv[1], val);
	return 0;
}

static int cmd_cfg_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int rc = settings_set_str(argv[1], argv[2]);

	if (rc) {
		shell_error(sh, "set failed: %d", rc);
		return rc;
	}
	shell_print(sh, "%s=%s", argv[1], argv[2]);
	return 0;
}

static int cmd_cfg_del(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int rc = settings_delete(argv[1]);

	if (rc) {
		shell_error(sh, "delete failed: %d", rc);
		return rc;
	}
	shell_print(sh, "deleted %s", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cfg,
	SHELL_CMD(list, NULL, "list all settings", cmd_cfg_list),
	SHELL_CMD_ARG(get, NULL, "<key>", cmd_cfg_get, 2, 0),
	SHELL_CMD_ARG(set, NULL, "<key> <value>", cmd_cfg_set, 3, 0),
	SHELL_CMD_ARG(del, NULL, "<key>", cmd_cfg_del, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(cfg, &sub_cfg, "Persisted settings (/NAND:/.blikachu.cfg)", NULL);

static int cmd_bright(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "brightness: %d/%d", gfx_backlight_get(), GFX_BRIGHT_MAX);
		return 0;
	}

	char *end = NULL;
	long v = strtol(argv[1], &end, 10);

	if (!end || *end || v < 1 || v > GFX_BRIGHT_MAX) {
		shell_error(sh, "usage: bright [1-%d]", GFX_BRIGHT_MAX);
		return -EINVAL;
	}
	(void)gfx_backlight_set((int)v);
	(void)settings_set_int("bright", gfx_backlight_get());
	hud_show_level("LCD", gfx_backlight_get(), GFX_BRIGHT_MAX);
	emu_buzzer_blip(1320, 20);
	shell_print(sh, "brightness: %d/%d", gfx_backlight_get(), GFX_BRIGHT_MAX);
	return 0;
}
SHELL_CMD_ARG_REGISTER(bright, NULL, "[1-10] — LCD backlight brightness", cmd_bright, 1, 1);

/* Inject a synthetic key tap through the input dispatch path (menu /
 * emu handler — unlike `gb btn`, which pokes the emulator joypad word
 * directly). Debug tool for exercising input handling without hands on
 * the device.
 */
static const struct {
	const char *name;
	uint16_t code;
} key_table[] = {
	{"up", INPUT_KEY_UP},   {"down", INPUT_KEY_DOWN}, {"left", INPUT_KEY_LEFT},
	{"right", INPUT_KEY_RIGHT}, {"a", INPUT_BTN_A},   {"b", INPUT_BTN_B},
	{"start", INPUT_BTN_START}, {"select", INPUT_BTN_SELECT},
};

static int cmd_key(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	for (size_t i = 0; i < ARRAY_SIZE(key_table); i++) {
		if (strcasecmp(argv[1], key_table[i].name) == 0) {
			input_inject(key_table[i].code, true);
			k_msleep(50);
			input_inject(key_table[i].code, false);
			return 0;
		}
	}
	shell_error(sh, "unknown key '%s'", argv[1]);
	return -EINVAL;
}
SHELL_CMD_ARG_REGISTER(key, NULL, "<up|down|left|right|a|b|start|select> — inject key tap",
		       cmd_key, 2, 0);

/* ---- `gb` subcommand group: emulator lifecycle + control ----
 *
 * `gb run <name|path> [frames]` resolves the file, kicks off the emu
 * worker thread, and returns to the prompt immediately. While running,
 * a shell bypass intercepts q / Ctrl-C / Esc to call emu_quit() even if
 * the parser is otherwise busy. No periodic logs — `gb stats` is the
 * way to ask how things are going.
 */

static int cmd_gb_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	menu_list(shell_print_cb, (void *)sh);
	return 0;
}

/* Shared launch core (shell + webui): resolve, defrag if needed,
 * build the save path, start. Returns 0 or -errno.
 */
int emu_shell_launch(const char *spec)
{
	char path[96];
	int n = spec[0] == '/' ? snprintf(path, sizeof(path), "%s", spec) :
		snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", spec);
	if (n < 0 || n >= sizeof(path)) return -ENAMETOOLONG;
	return emu_run(path, 0);
}

static int cmd_gb_run(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: gb run <name|path> [frames]");
		return -EINVAL;
	}
	uint32_t frames = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;

	/* If the first arg is a pure decimal integer, treat it as a ROM
	 * index from `gb list`.
	 */
	const char *spec = argv[1];
	char resolved[ROM_NAME_MAX];
	char *endp;
	long idx = strtol(spec, &endp, 10);
	if (endp != spec && *endp == '\0') {
		int rc = menu_get_rom((int)idx, resolved, sizeof(resolved));
		if (rc) {
			shell_error(sh, "rom #%ld: %s", idx,
				    rc == -ENOENT ? "no ROMs found" : "out of range");
			return rc;
		}
		spec = resolved;
	}

	char path[96];
	int n = spec[0] == '/' ? snprintf(path, sizeof(path), "%s", spec) :
		snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", spec);
	if (n < 0 || n >= sizeof(path)) return -ENAMETOOLONG;

	int rc = emu_run(path, frames);
	if (rc) {
		shell_error(sh, "emu_run: %d", rc);
		return rc;
	}
	shell_print(sh, "running %s (%u frames%s) — `gb stop` to kill", path, frames,
		    frames ? "" : " unlimited");
	return 0;
}

static int cmd_gb_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_quit();
	emu_wait();
	input_set_handler(NULL);
	menu_refresh();
	return 0;
}

static int cmd_gb_pause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_pause();
	return 0;
}

static int cmd_gb_resume(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_resume();
	return 0;
}

static int cmd_gb_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_reset();
	return 0;
}


static int cmd_gb_save(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc >= 2) ? argv[1] : NULL;
	int rc = emu_state_save(path);
	if (rc) {
		shell_error(sh, "save: %d", rc);
		return rc;
	}
	shell_print(sh, "state save -> %s", path ? path : emu_default_save_path());
	return 0;
}

static int cmd_gb_restore(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc >= 2) ? argv[1] : NULL;
	int rc = emu_state_restore(path);
	if (rc) {
		shell_error(sh, "restore: %d", rc);
		return rc;
	}
	shell_print(sh, "state restore <- %s", path ? path : emu_default_save_path());
	return 0;
}

static int cmd_gb_apu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_apu_dump(shell_print_cb, (void *)sh);
	return 0;
}

static int cmd_gb_mute(const struct shell *sh, size_t argc, char **argv)
{
	bool on = strcmp(argv[1], "on") == 0;

	if (!on && strcmp(argv[1], "off") != 0) {
		shell_error(sh, "usage: gb mute <on|off>");
		return -EINVAL;
	}
	emu_buzzer_mute(on);
	shell_print(sh, "buzzer mute %s", on ? "on" : "off");
	return 0;
}

static int cmd_gb_vol(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "volume: %d/11", emu_buzzer_volume_get());
		return 0;
	}

	char *end = NULL;
	long v = strtol(argv[1], &end, 10);

	if (!end || *end || v < 0 || v > 11) {
		shell_error(sh, "usage: gb vol [0-11]");
		return -EINVAL;
	}
	emu_buzzer_volume((int)v);
	(void)settings_set_int("vol", (int)v);
	hud_show_level("VOL", (int)v, 11);
	emu_buzzer_blip(880, 40);
	shell_print(sh, "volume: %ld/11%s", v,
		    v >= 9 ? " (careful: this duty collapsed the rail on the stock buzzer)" : "");
	return 0;
}

static int cmd_gb_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	struct emu_stats s;
	emu_get_stats(&s);
	shell_print(sh,
		    "running=%d paused=%d frame=%u presented=%u unchanged=%u strips=%u late=%u skip=%u core_skip=%u temiss=%u "
		    "run=%llu ms fps=%u.%03u",
		    s.running, s.paused, s.frame, s.presented, s.unchanged, s.strips_written, s.late_frames, s.render_skips, s.core_skips, s.te_misses,
		    (unsigned long long)s.run_ms, s.mfps / 1000, s.mfps % 1000);
	shell_print(sh, "push=%u us (max %u, scale %u) te_period=%u us emu=%u us (max %u)",
		    s.push_us, s.push_us_max, s.scale_us, s.te_period_us,
		    s.emu_us, s.emu_us_max);

	shell_print(sh, "state operations=%u result=%d irq_off_frames=%u",
		    s.state_done, s.state_result, s.irq_off_frames);
	size_t stack_used, stack_capacity, heap_peak, heap_capacity;
	emu_get_resources(&stack_used, &stack_capacity, &heap_peak, &heap_capacity);
	shell_print(sh, "stack=%zu/%zu heap_peak=%zu/%zu", stack_used, stack_capacity, heap_peak, heap_capacity);
	if (s.error) {
		shell_print(sh, "last emu error: %d", s.error);
	}
	return 0;
}

static int cmd_display_mode(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	enum emu_display_mode mode;
	if (!strcmp(argv[1], "on")) mode = EMU_DISPLAY_SERIAL;
	else if (!strcmp(argv[1], "off")) mode = EMU_DISPLAY_CONCURRENT;
	else if (!strcmp(argv[1], "dma")) mode = EMU_DISPLAY_DMA;
	else if (!strcmp(argv[1], "headless")) mode = EMU_DISPLAY_HEADLESS;
	else return -EINVAL;
	struct emu_stats s;
	emu_get_stats(&s);
	if (s.running) return -EBUSY;
	emu_display_mode_set(mode);
	shell_print(sh, "display schedule %s", argv[1]);
	return 0;
}
SHELL_CMD_ARG_REGISTER(lrdisplay, NULL, "off/on/dma/headless — display diagnostic schedule", cmd_display_mode, 2, 0);

static const struct {
	const char *name;
	uint8_t mask;
} gb_btn_table[] = {
	{"a", 0x01},     {"b", 0x02},    {"select", 0x04}, {"start", 0x08},
	{"right", 0x10}, {"left", 0x20}, {"up", 0x40},     {"down", 0x80},
};

static uint8_t gb_btn_lookup(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(gb_btn_table); i++) {
		if (strcasecmp(name, gb_btn_table[i].name) == 0) {
			return gb_btn_table[i].mask;
		}
	}
	return 0;
}

static int cmd_gb_btn(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t mask = gb_btn_lookup(argv[1]);
	if (!mask) {
		shell_error(sh,
			    "unknown button '%s' "
			    "(a|b|start|select|up|down|left|right)",
			    argv[1]);
		return -EINVAL;
	}
	uint32_t hold_ms = 100;
	if (argc >= 3) {
		char *end = NULL;
		long v = strtol(argv[2], &end, 10);
		if (!end || *end || v < 0 || v > 60000) {
			shell_error(sh, "invalid hold_ms '%s'", argv[2]);
			return -EINVAL;
		}
		hold_ms = (uint32_t)v;
	}
	emu_button(mask, true);
	if (hold_ms > 0) {
		k_msleep(hold_ms);
	}
	emu_button(mask, false);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_gb, SHELL_CMD(list, NULL, "list ROMs available on /NAND:", cmd_gb_list),
	SHELL_CMD_ARG(run, NULL, "<name|path> [frames] — start a ROM", cmd_gb_run, 2, 1),
	SHELL_CMD(stop, NULL, "stop the running ROM", cmd_gb_stop),
	SHELL_CMD(pause, NULL, "pause the running ROM", cmd_gb_pause),
	SHELL_CMD(resume, NULL, "resume after pause", cmd_gb_resume),
	SHELL_CMD(reset, NULL, "reset CPU/PPU/APU (keep ROM + cart RAM)", cmd_gb_reset),
SHELL_CMD_ARG(btn, NULL, "<a|b|start|select|up|down|left|right> [hold_ms] — tap a button",
		      cmd_gb_btn, 2, 1),
	SHELL_CMD_ARG(save, NULL, "[path] — full save state (default core/ROM .lrstate)",
		      cmd_gb_save, 1, 1),
	SHELL_CMD_ARG(restore, NULL, "[path] — restore full save state", cmd_gb_restore, 1,
		      1),
	SHELL_CMD(info, NULL, "running, paused, frame, fps, ...", cmd_gb_info),
	SHELL_CMD(apu, NULL, "PCM audio diagnostics", cmd_gb_apu),
	SHELL_CMD_ARG(mute, NULL, "<on|off> — force the buzzer silent", cmd_gb_mute, 2, 0),
	SHELL_CMD_ARG(vol, NULL, "[0-10] — buzzer volume (PWM duty)", cmd_gb_vol, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(gb, &sub_gb, "Game emulator commands", NULL);
