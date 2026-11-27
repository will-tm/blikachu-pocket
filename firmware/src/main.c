/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/display.h>
#include <zephyr/pm/device.h>

#include <zephyr/usb/class/usbd_msc.h>

#include "bootanim.h"
#include "charger.h"
#include "emu.h"
#include <libretro.h>
#include "gfx.h"
#include "hud.h"
#include "input.h"
#include "menu.h"
#include "settings.h"
#include "storage.h"
#include "usbd_setup.h"

LOG_MODULE_REGISTER(blikachu, LOG_LEVEL_INF);

#include "platform.h"
#include "storage_shell.h"

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Rebooting...");
	storage_unmount();
	/* Let the CDC TX drain so the message actually reaches the host. */
	k_msleep(100);
	platform_reboot();
	return 0;
}
SHELL_CMD_REGISTER(reboot, NULL, "Soft-reboot back into the firmware (warm reset)", cmd_reboot);

/* Power-off: blank the display, unmount storage and drop the power latch. */
#include "sleep_icon.h" /* generated from assets/sleep.png */

static int enter_deep_sleep(void)
{
	emu_buzzer_blip(440, 120);
	emu_quit();
	emu_wait();
	storage_unmount();

	/* Goodnight splash for 2 s before the panel goes dark (also
	 * covers CDC TX drain and emu wind-down).
	 */
	if (gfx_init() == 0) {
		gfx_clear(COLOR_BLACK);
		/* Full-panel art: center against the physical 240x240 panel
		 * (gfx_draw_image applies the +LCD_VIEW_Y viewport offset,
		 * compensate so panel row 0 is reachable).
		 */
		(void)gfx_draw_image_rle((LCD_PANEL_W - SLEEP_ICON_W) / 2,
			(LCD_PANEL_H - SLEEP_ICON_H) / 2 - LCD_VIEW_Y, SLEEP_ICON_W,
			SLEEP_ICON_H, sleep_icon_rle, ARRAY_SIZE(sleep_icon_rle));
	}
	k_msleep(2000);

	/* LCD off: panel display-off + backlight (GPIO28) to zero. */
	const struct device *disp = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
	int rc_blank = -1;
	int rc_pm = -1;

	if (disp && device_is_ready(disp)) {
		rc_blank = display_blanking_on(disp);
		/* SLPIN: stop the panel booster/oscillator (~mA -> µA).
		 * The 3V3 rail stays up in deep sleep, so panel idle draw
		 * is part of the sleep floor.
		 */
		rc_pm = pm_device_action_run(disp, PM_DEVICE_ACTION_SUSPEND);
	}
	gfx_backlight_off();
	LOG_INF("sleep: blank=%d pm=%d", rc_blank, rc_pm);
	k_msleep(150); /* drain the log over CDC before the link dies */

	/* Enter the platform's deepest state; only returns on refusal
	 * (e.g. externally powered) — re-light and carry on.
	 */
	int rc = platform_deep_sleep();

	(void)gfx_backlight_set(gfx_backlight_get());
	if (disp && device_is_ready(disp)) {
		(void)pm_device_action_run(disp, PM_DEVICE_ACTION_RESUME);
		(void)display_blanking_off(disp);
	}
	return rc;
}

static int cmd_sleep(const struct shell *sh, size_t argc, char **argv)
{
	/* `sleep dry`: run the LCD-off sequence but stay awake — lets the
	 * screen state be inspected with the console alive (debug).
	 */
	if (argc > 1 && strcmp(argv[1], "dry") == 0) {
		const struct device *disp = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
		int rc_blank = -1;
		int rc_pm = -1;

		if (disp && device_is_ready(disp)) {
			rc_blank = display_blanking_on(disp);
			rc_pm = pm_device_action_run(disp, PM_DEVICE_ACTION_SUSPEND);
		}
		gfx_backlight_off();
		shell_print(sh, "dry sleep: blank=%d pm=%d — restoring in 8 s", rc_blank, rc_pm);
		k_sleep(K_SECONDS(8));
		(void)gfx_backlight_set(gfx_backlight_get());
		if (disp && device_is_ready(disp)) {
			(void)pm_device_action_run(disp, PM_DEVICE_ACTION_RESUME);
			(void)display_blanking_off(disp);
		}
		shell_print(sh, "restored");
		return 0;
	}

	shell_print(sh, "Deep sleep: wake on SELECT/B/A/START...");
	int rc = enter_deep_sleep();

	shell_error(sh, "sleep failed: %d", rc);
	return rc;
}
SHELL_CMD_REGISTER(sleep, NULL, "Deepest sleep (no RAM retention), wake on SELECT/B/A/START",
		   cmd_sleep);

/* ---- Global button hotkeys ----
 *
 * Observes raw input events independently of whoever owns the input
 * handler (menu or emulator); synthetic `gb btn` presses do NOT pass
 * through here (they poke the joypad word directly).
 *
 *  - START held >1.5 s      → deep sleep
 *  - SELECT held >1.5 s     → toggle USB mass-storage mode (menu only)
 *  - SELECT + UP / DOWN     → volume up / down (0..11)
 *  - SELECT + LEFT / RIGHT  → backlight dimmer / brighter (1..10)
 *
 * Volume/brightness are persisted through the settings store, debounced
 * so button-mashing doesn't hammer the flash.
 */
#define LONGPRESS_TIMEOUT K_MSEC(1500)

static void sleep_longpress_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* Power-on grace: on v2 the power button doubles as a key and
	 * must be held until the latch engages — don't let that same
	 * press read as "START held, sleep" (which on BL618 releases
	 * the latch = instant power-off after boot).
	 */
	if (k_uptime_get() < 5000) {
		LOG_INF("START held during boot — ignoring (power-on press)");
		return;
	}
	LOG_INF("START held — entering deep sleep");
	(void)enter_deep_sleep();
}
static K_WORK_DELAYABLE_DEFINE(g_sleep_longpress, sleep_longpress_work_fn);

/* Idle auto-sleep: 5 minutes without a button press while the MENU is
 * showing AND running on battery. Rearmed by every physical input
 * event (the hotkey filter sees them all); a running game, active
 * USB-MSC session, or external power (charger PG) postpones rather
 * than sleeps.
 */
#define IDLE_SLEEP_TIMEOUT K_MINUTES(5)

static void idle_sleep_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(g_idle_sleep, idle_sleep_work_fn);

static void idle_sleep_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	struct emu_stats s;

	emu_get_stats(&s);
	if (s.running || usb_mode_active() || charger_input_present()) {
		k_work_reschedule(&g_idle_sleep, IDLE_SLEEP_TIMEOUT);
		return;
	}
	LOG_INF("menu idle — entering deep sleep");
	(void)enter_deep_sleep();
}

static void hotkey_persist_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)settings_set_int("vol", emu_buzzer_volume_get());
	(void)settings_set_int("bright", gfx_backlight_get());
}
static K_WORK_DELAYABLE_DEFINE(g_hotkey_persist, hotkey_persist_work_fn);

/* SELECT chord state. SELECT's press is DEFERRED: swallowed on press,
 * and its meaning decided by what follows:
 *  - a direction  → hotkey chord; the handler never sees SELECT at all
 *  - release      → it was a tap; inject a clean synthetic tap
 *  - START/A/B    → combo (e.g. SELECT+START quit); flush the pending
 *                   SELECT press first, then let the other key through
 */
static bool g_select_held;     /* physical state */
static bool g_select_pending;  /* press not yet delivered to handler */
static bool g_select_was_chord;

/* SELECT held >1.5 s in the MENU toggles USB mass-storage mode. Armed on
 * the SELECT press, disarmed on release and whenever a chord claims
 * SELECT (otherwise a long volume-adjust session would fire it). When
 * it fires, the held SELECT is consumed like a chord so the menu never
 * sees a tap on release. In-game it does nothing — SELECT belongs to
 * the game there.
 */
static void usb_longpress_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	struct emu_stats s;

	emu_get_stats(&s);
	if (s.running) {
		return; /* menu only */
	}

	g_select_was_chord = true;
	g_select_pending = false;

	if (usb_mode_active()) {
		LOG_INF("SELECT held — exiting USB mode");
		usb_mode_exit();
	} else {
		LOG_INF("SELECT held — entering USB mode");
		usb_mode_enter();
	}
}
static K_WORK_DELAYABLE_DEFINE(g_usb_longpress, usb_longpress_work_fn);

static void select_flush(void)
{
	if (g_select_pending) {
		input_inject(INPUT_BTN_SELECT, true);
		g_select_pending = false;
		g_select_was_chord = false;
		(void)k_work_cancel_delayable(&g_usb_longpress);
	}
}

static bool hotkey_level_adjust(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_UP:
		emu_buzzer_volume(emu_buzzer_volume_get() + 1);
		hud_show_level("VOL", emu_buzzer_volume_get(), 11);
		emu_buzzer_blip(880, 40); /* audible at the NEW volume */
		break;
	case INPUT_KEY_DOWN:
		emu_buzzer_volume(emu_buzzer_volume_get() - 1);
		hud_show_level("VOL", emu_buzzer_volume_get(), 11);
		emu_buzzer_blip(880, 40);
		break;
	case INPUT_KEY_RIGHT:
		(void)gfx_backlight_set(gfx_backlight_get() + 1);
		hud_show_level("LCD", gfx_backlight_get(), GFX_BRIGHT_MAX);
		emu_buzzer_blip(1320, 20);
		break;
	case INPUT_KEY_LEFT:
		(void)gfx_backlight_set(gfx_backlight_get() - 1);
		hud_show_level("LCD", gfx_backlight_get(), GFX_BRIGHT_MAX);
		emu_buzzer_blip(1320, 20);
		break;
	default:
		return false;
	}
	k_work_reschedule(&g_hotkey_persist, K_MSEC(1500));
	return true;
}

/* Panic chord: A+B+START+SELECT all held 2 s → clean reboot. The input
 * path runs independently of the shell, so this recovers a wedged
 * console without the power button (GBA-style soft reset; games with
 * their own A+B+START+SELECT reset fire on press — only a deliberate
 * hold reboots the device).
 */
static void panic_reboot_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("panic chord held — rebooting");
	storage_unmount();
	platform_reboot();
}
static K_WORK_DELAYABLE_DEFINE(g_panic_reboot, panic_reboot_work_fn);
static uint8_t g_panic_mask;

static void panic_chord_track(uint16_t code, bool pressed)
{
	uint8_t bit;

	switch (code) {
	case INPUT_BTN_A:
		bit = BIT(0);
		break;
	case INPUT_BTN_B:
		bit = BIT(1);
		break;
	case INPUT_BTN_START:
		bit = BIT(2);
		break;
	case INPUT_BTN_SELECT:
		bit = BIT(3);
		break;
	default:
		return;
	}
	g_panic_mask = pressed ? (g_panic_mask | bit) : (g_panic_mask & ~bit);
	if (g_panic_mask == 0x0f) {
		k_work_reschedule(&g_panic_reboot, K_SECONDS(2));
	} else {
		(void)k_work_cancel_delayable(&g_panic_reboot);
	}
}

static bool system_hotkeys_filter(struct input_event *evt)
{
	if (evt->type != INPUT_EV_KEY) {
		return false;
	}

	/* Any physical button counts as activity for idle auto-sleep. */
	k_work_reschedule(&g_idle_sleep, IDLE_SLEEP_TIMEOUT);

	panic_chord_track(evt->code, evt->value);

	/* SNES modifier buttons consume both edges, even if SELECT is released
	 * before the other key. The existing quit and volume chords stay active. */
	static uint16_t snes_chords;
	unsigned snes_id = 16;
	switch (evt->code) {
	case INPUT_BTN_A: snes_id = RETRO_DEVICE_ID_JOYPAD_A; break;
	case INPUT_BTN_B: snes_id = RETRO_DEVICE_ID_JOYPAD_X; break;
	case INPUT_KEY_LEFT: snes_id = RETRO_DEVICE_ID_JOYPAD_L; break;
	case INPUT_KEY_RIGHT: snes_id = RETRO_DEVICE_ID_JOYPAD_R; break;
	default: break;
	}
	if (snes_id < 16) {
		if (!evt->value && (snes_chords & BIT(snes_id))) {
			snes_chords &= ~BIT(snes_id);
			emu_retro_button(snes_id, false);
			return true;
		}
		if (evt->value && g_select_held && emu_current_system() == EMU_SYSTEM_SNES) {
			g_select_was_chord = true;
			g_select_pending = false;
			(void)k_work_cancel_delayable(&g_usb_longpress);
			snes_chords |= BIT(snes_id);
			emu_retro_button(snes_id, true);
			return true;
		}
	}

	switch (evt->code) {
	case INPUT_BTN_START:
		if (evt->value) {
			k_work_reschedule(&g_sleep_longpress, LONGPRESS_TIMEOUT);
			select_flush(); /* keep SELECT+START quit combo alive */
		} else {
			(void)k_work_cancel_delayable(&g_sleep_longpress);
		}
		return false;

	case INPUT_BTN_SELECT:
		if (evt->value) {
			g_select_held = true;
			g_select_pending = true;
			g_select_was_chord = false;
			k_work_reschedule(&g_usb_longpress, LONGPRESS_TIMEOUT);
			return true; /* defer */
		}
		g_select_held = false;
		(void)k_work_cancel_delayable(&g_usb_longpress);
		if (g_select_pending && !g_select_was_chord) {
			/* Plain tap: deliver it now, cleanly. */
			g_select_pending = false;
			input_inject(INPUT_BTN_SELECT, true);
			input_inject(INPUT_BTN_SELECT, false);
			return true;
		}
		if (g_select_was_chord) {
			/* Chord consumed the press — swallow the release. */
			g_select_pending = false;
			g_select_was_chord = false;
			return true;
		}
		return false; /* press was flushed earlier — forward release */

	case INPUT_KEY_UP:
	case INPUT_KEY_DOWN:
	case INPUT_KEY_LEFT:
	case INPUT_KEY_RIGHT:
		if (g_select_held && evt->value) {
			g_select_was_chord = true;
			g_select_pending = false;
			(void)k_work_cancel_delayable(&g_usb_longpress);
			(void)hotkey_level_adjust(evt->code);
			return true;
		}
		return false;

	case INPUT_BTN_A:
	case INPUT_BTN_B:
		/* SELECT+B = save state, SELECT+A = restore state
		 * (in-game only; the game never sees the chord).
		 */
		if (g_select_held && evt->value) {
			struct emu_stats st;

			emu_get_stats(&st);
			if (st.running) {
				g_select_was_chord = true;
				g_select_pending = false;
				(void)k_work_cancel_delayable(&g_usb_longpress);
				bool save = (evt->code == INPUT_BTN_B);
				int rc = save ? emu_state_save(NULL) : emu_state_restore(NULL);

				hud_show_text(rc     ? "STATE ERROR"
					      : save ? "SAVING..."
						     : "LOADING...");
				return true;
			}
		}
		if (evt->value) {
			select_flush();
		}
		return false;

	default:
		if (evt->value) {
			select_flush();
		}
		return false;
	}
}



int main(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	platform_init();
	input_set_filter(system_hotkeys_filter);
	k_work_schedule(&g_idle_sleep, IDLE_SLEEP_TIMEOUT);

	uint32_t reset_cause = 0;
	hwinfo_get_reset_cause(&reset_cause);
	LOG_INF("reset cause: 0x%02x%s%s%s%s%s", reset_cause,
		(reset_cause & RESET_POR) ? " POR" : "",
		(reset_cause & RESET_PIN) ? " PIN" : "",
		(reset_cause & RESET_BROWNOUT) ? " BROWNOUT" : "",
		(reset_cause & RESET_WATCHDOG) ? " WDT" : "",
		(reset_cause & RESET_DEBUG) ? " DBG" : "");

	(void)disk_access_init(STORAGE_DISK_NAME);
	/* The first mount after a tool/OTA reset fails intermittently
	 * (rc gets logged; root cause still open) — retry with backoff
	 * instead of silently booting without storage.
	 */
	for (int attempt = 1; attempt <= 4; attempt++) {
		int mrc = storage_mount();

		if (mrc == 0) {
			break;
		}
		LOG_WRN("boot mount attempt %d failed: %d", attempt, mrc);
		k_msleep(300 * attempt);
	}
	emu_buzzer_volume(settings_get_int("vol", emu_buzzer_volume_get()));

	if (usbd_setup()) {
		LOG_ERR("USB setup failed");
		return 0;
	}

	/* Light the panel at the persisted brightness, roll the boot
	 * animation once (any button skips), then hand over to the menu.
	 */
	(void)gfx_init();
	(void)gfx_backlight_set(settings_get_int("bright", GFX_BRIGHT_DEFAULT));
	boot_anim_play();
	menu_start();

	while (!dtr) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}

	return 0;
}
