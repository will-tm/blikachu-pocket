/*
 * Boot menu: scans /NAND: for supported ROMs, draws a paged list on the
 * ST7789V, and dispatches user input. UP/DOWN navigate, START launches
 * the highlighted ROM, SELECT triggers a re-scan. While a ROM is
 * running, all keys go to the emulator (which detects SELECT+START to
 * quit back here).
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include "menu.h"
#include "emu.h"
#include "hud.h"
#include "input.h"
#include "gfx.h"
#include "storage.h"
#include "psram_buf.h"

LOG_MODULE_REGISTER(menu, LOG_LEVEL_INF);

#define ROMS_DIR  STORAGE_MOUNT_POINT
#define SAVES_DIR STORAGE_MOUNT_POINT "/.saves"

#define MAX_ROMS      64
/* ROM_NAME_MAX is exposed in menu.h so the shell can reuse it. */
#define LINE_HEIGHT   16
#define VISIBLE_LINES 11 /* 216 - header(32) = 184; 184/16=11 */
#define HEADER_HEIGHT 32

struct rom_entry {
	char name[ROM_NAME_MAX];
	size_t size;
};

static BLIKACHU_BIGBUF struct rom_entry g_roms[MAX_ROMS];
static int g_rom_count;
static int g_selected;
static int g_top;

static struct k_thread g_thread;
static K_THREAD_STACK_DEFINE(g_stack, 4096);
static struct k_sem g_kick;

static bool g_emu_active;
static bool g_started;

#define BG_COLOR     COLOR_BLACK
#define FG_COLOR     COLOR_WHITE
#define HEADER_COLOR COLOR_PIKACHU
#define HILITE_BG    COLOR_PIKACHU
#define HILITE_FG    COLOR_BLACK
#define DIM_COLOR    COLOR_GRAY

extern void buzzer_blip(uint32_t freq, uint32_t ms); /* implemented below */

/* ---- ROM list ---- */

static bool has_rom_ext(const char *name)
{
	return emu_content_extension_supported(name);
}

static void scan_roms(void)
{
	g_rom_count = 0;

	if (storage_mount() != 0) {
		return;
	}
	struct fs_dirent ent;
	if (fs_stat(SAVES_DIR, &ent) != 0) {
		(void)fs_mkdir(SAVES_DIR);
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, ROMS_DIR);
	if (rc) {
		LOG_WRN("fs_opendir %s: %d", ROMS_DIR, rc);
		return;
	}

	for (;;) {
		struct fs_dirent ent;
		rc = fs_readdir(&dir, &ent);
		if (rc || ent.name[0] == '\0') {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (ent.name[0] == '.') {
			continue;
		}
		if (!has_rom_ext(ent.name)) {
			continue;
		}
		if (g_rom_count >= MAX_ROMS) {
			break;
		}
		char path[sizeof(ROMS_DIR) + sizeof(ent.name) + 1];
		size_t rom_size;

		snprintf(path, sizeof(path), ROMS_DIR "/%s", ent.name);
		if (storage_rom_info(path, &rom_size)) {
			continue;
		}
		(void)snprintf(g_roms[g_rom_count].name, ROM_NAME_MAX, "%.*s",
			       (int)(ROM_NAME_MAX - 1), ent.name);
		g_roms[g_rom_count].size = rom_size;
		g_rom_count++;
	}
	fs_closedir(&dir);

	if (g_selected >= g_rom_count) {
		g_selected = g_rom_count > 0 ? g_rom_count - 1 : 0;
	}
}

/* ---- Drawing ---- */

static void draw_header(void)
{
	gfx_fill_rect(0, 0, LCD_WIDTH, HEADER_HEIGHT, COLOR_DGRAY);
	gfx_draw_text(8, 4, "BLikachu Pocket", HEADER_COLOR, COLOR_DGRAY);

	char counts[24];
	snprintf(counts, sizeof(counts), "%d ROM%s", g_rom_count, g_rom_count == 1 ? "" : "s");
	gfx_draw_text(8, 18, counts, FG_COLOR, COLOR_DGRAY);

	gfx_draw_text(LCD_WIDTH - 8 * 11, 4, "UP/DN nav", DIM_COLOR, COLOR_DGRAY);
	gfx_draw_text(LCD_WIDTH - 8 * 7, 18, "A play", DIM_COLOR, COLOR_DGRAY);
}

static void draw_list(void)
{
	int y = HEADER_HEIGHT;
	gfx_fill_rect(0, y, LCD_WIDTH, LCD_HEIGHT - y, BG_COLOR);

	if (g_rom_count == 0) {
		gfx_draw_text(8, y + 16, "no ROM files", DIM_COLOR, BG_COLOR);
		gfx_draw_text(8, y + 32, "Use Wi-Fi ROM manager", DIM_COLOR, BG_COLOR);
		return;
	}

	int max_chars = (LCD_WIDTH - 16) / 8; /* 28 */
	int last = g_top + VISIBLE_LINES;
	if (last > g_rom_count) {
		last = g_rom_count;
	}
	for (int i = g_top; i < last; i++) {
		int row_y = y + (i - g_top) * LINE_HEIGHT;
		bool sel = (i == g_selected);
		uint16_t fg = sel ? HILITE_FG : FG_COLOR;
		uint16_t bg = sel ? HILITE_BG : BG_COLOR;
		gfx_fill_rect(0, row_y, LCD_WIDTH, LINE_HEIGHT, bg);
		gfx_draw_text(8, row_y + 4, sel ? ">" : " ", fg, bg);
		char trim[ROM_NAME_MAX];
		strncpy(trim, g_roms[i].name, sizeof(trim));
		trim[sizeof(trim) - 1] = '\0';
		if ((int)strlen(trim) > max_chars - 2) {
			trim[max_chars - 2] = '\0';
		}
		gfx_draw_text(24, row_y + 4, trim, fg, bg);
	}
}

static void redraw_all(void)
{
	gfx_clear(BG_COLOR);
	draw_header();
	draw_list();
}

/* ---- Navigation ---- */

static void ensure_selected_visible(void)
{
	if (g_selected < g_top) {
		g_top = g_selected;
	} else if (g_selected >= g_top + VISIBLE_LINES) {
		g_top = g_selected - VISIBLE_LINES + 1;
	}
}

/* ---- Input handler (only active when menu is the screen owner) ---- */

static atomic_t g_event_pending;

static void menu_input_cb(struct input_event *evt)
{
	if (evt->type != INPUT_EV_KEY || !evt->value) {
		return; /* key down only */
	}

	switch (evt->code) {
	case INPUT_KEY_UP:
		if (g_selected > 0) {
			g_selected--;
			buzzer_blip(880, 20);
		}
		break;
	case INPUT_KEY_DOWN:
		if (g_selected + 1 < g_rom_count) {
			g_selected++;
			buzzer_blip(880, 20);
		}
		break;
	case INPUT_BTN_A:
		buzzer_blip(1320, 40);
		atomic_set(&g_event_pending, 1);
		break;
	case INPUT_BTN_SELECT:
		buzzer_blip(660, 20);
		atomic_set(&g_event_pending, 2);
		break;
	default:
		return;
	}

	k_sem_give(&g_kick);
}

/* ---- Buzzer ---- */

/* Delegates to emu.c, which owns the buzzer and applies the persisted
 * volume + mute settings (the old local PWM path blasted 50% duty
 * regardless of `emu vol`).
 */
void buzzer_blip(uint32_t freq, uint32_t ms)
{
	emu_buzzer_blip(freq, ms);
}

/* ---- Launch ---- */

int menu_launch(const char *path)
{
	g_emu_active = true;
	int rc = emu_run(path, 0);
	if (rc == 0) {
		emu_wait();
	} else {
		hud_show_text("LAUNCH FAILED");
		LOG_ERR("launch %s: %d", path, rc);
	}
	g_emu_active = false;

	/* Hand input back to us and redraw. */
	struct emu_stats stats;
	emu_get_stats(&stats);
	if (!stats.running) {
		input_set_handler(menu_input_cb);
		redraw_all();
	}
	return rc;
}

int menu_get_rom(int idx, char *out, size_t cap)
{
	scan_roms();
	if (g_rom_count == 0) {
		return -ENOENT;
	}
	if (idx < 0 || idx >= g_rom_count) {
		return -ERANGE;
	}
	strncpy(out, g_roms[idx].name, cap - 1);
	out[cap - 1] = '\0';
	return 0;
}

void menu_list(menu_print_fn cb, void *ctx)
{
	if (!cb) {
		return;
	}
	scan_roms();
	if (g_rom_count == 0) {
		cb(ctx, "(no ROMs)");
		return;
	}
	for (int i = 0; i < g_rom_count; i++) {
		char line[80];
		snprintf(line, sizeof(line), "%2d  %-40.40s  %u KB", i, g_roms[i].name,
			 (unsigned)(g_roms[i].size / 1024));
		cb(ctx, line);
	}
}

void menu_refresh(void)
{
	if (!g_started) {
		return;
	}
	atomic_set(&g_event_pending, 3);
	k_sem_give(&g_kick);
}

/* ---- Thread ---- */

static void menu_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)gfx_init();

	scan_roms();
	redraw_all();
	input_set_handler(menu_input_cb);
	LOG_INF("menu ready: %d ROMs", g_rom_count);

	for (;;) {
		k_sem_take(&g_kick, K_FOREVER);

		atomic_val_t pending = atomic_set(&g_event_pending, 0);
		switch (pending) {
		case 1: {
			/* A — launch selection */
			if (g_rom_count == 0) {
				break;
			}
			char path[ROM_NAME_MAX + 16];
			snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s",
				 g_roms[g_selected].name);

			emu_buzzer_blip(1175, 50);
			input_set_handler(NULL); /* emu_run will install its own */
			(void)menu_launch(path);
			break;
		}
		case 2: /* SELECT — refresh */
		case 3: { /* programmatic refresh */
			struct emu_stats stats;
			emu_get_stats(&stats);
			if (stats.running) {
				break;
			}
			scan_roms();
			redraw_all();
			input_set_handler(menu_input_cb);
			break;
		}
		default:
			ensure_selected_visible();
			draw_list();
			break;
		}
	}
}

void menu_start(void)
{
	if (g_started) {
		return;
	}
	k_sem_init(&g_kick, 0, 1);
	g_started = true;
	k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack), menu_thread_fn, NULL,
			NULL, NULL, K_PRIO_PREEMPT(12), 0, K_NO_WAIT);
	k_thread_name_set(&g_thread, "menu");
}

static int cmd_menu_refresh(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	menu_refresh();
	shell_print(sh, "menu refresh requested");
	return 0;
}
SHELL_CMD_REGISTER(menu, NULL, "Refresh the game menu", cmd_menu_refresh);
