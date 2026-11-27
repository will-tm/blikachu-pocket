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
#include <zephyr/net/net_if.h>

#include "battery.h"
#include "charger.h"
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
#define LINE_HEIGHT   26
#define HEADER_HEIGHT 40
#define FOOTER_HEIGHT 20
#define VISIBLE_LINES ((LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT) / LINE_HEIGHT)
#define NAME_X        24
#define NAME_W        (LCD_WIDTH - NAME_X - 8)
#define SCROLL_MS     50
#define SCROLL_STEP   2
#define SCROLL_HOLD   800

struct rom_entry {
	char name[ROM_NAME_MAX];
	size_t size;
};

static BLIKACHU_BIGBUF struct rom_entry g_roms[MAX_ROMS];
static int g_rom_count;
static int g_selected;
static int g_top;
static char g_scroll_name[ROM_NAME_MAX];
static int g_scroll_selected;
static int g_scroll_y;
static unsigned g_scroll_overflow;
static unsigned g_scroll_offset;
static int64_t g_scroll_epoch;
static int64_t g_status_time;
static int g_status_pct;
static bool g_status_wifi;
static bool g_status_power;

static struct k_thread g_thread;
static K_THREAD_STACK_DEFINE(g_stack, 4096);
static struct k_sem g_kick;

static bool g_emu_active;
static bool g_started;

#define BG_COLOR     RGB565(14, 19, 27)
#define ROW_COLOR    RGB565(28, 36, 47)
#define BAR_COLOR    RGB565(37, 47, 61)
#define FG_COLOR     COLOR_WHITE
#define HEADER_COLOR COLOR_PIKACHU
#define HILITE_BG    COLOR_PIKACHU
#define HILITE_FG    COLOR_BLACK
#define DIM_COLOR    RGB565(160, 175, 190)

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
	g_top = MIN(g_top, g_selected);
}

/* ---- Drawing ---- */

static void draw_status(bool force)
{
	int pct = battery_cached_percent();
	bool power = charger_input_present();
	bool wifi = net_if_is_up(net_if_get_first_wifi());

	g_status_time = k_uptime_get();
	if (!force && pct == g_status_pct && power == g_status_power && wifi == g_status_wifi) {
		return;
	}
	g_status_pct = pct;
	g_status_power = power;
	g_status_wifi = wifi;
	gfx_fill_rect(148, 5, 92, 30, BAR_COLOR);

	/* Wi-Fi: nested arcs and a dot; gray when the link is down. */
	uint16_t ink = wifi ? COLOR_WHITE : COLOR_GRAY;
	gfx_fill_rect(151, 9, 14, 2, ink);
	gfx_fill_rect(149, 11, 2, 2, ink);
	gfx_fill_rect(165, 11, 2, 2, ink);
	gfx_fill_rect(153, 13, 10, 2, ink);
	gfx_fill_rect(151, 15, 2, 2, ink);
	gfx_fill_rect(163, 15, 2, 2, ink);
	gfx_fill_rect(156, 17, 4, 2, ink);
	gfx_fill_rect(157, 21, 2, 2, ink);

	/* Battery outline, terminal, and proportional fill. */
	uint16_t fill = power ? COLOR_GREEN : pct <= 15 ? COLOR_RED : HEADER_COLOR;
	gfx_fill_rect(175, 9, 24, 14, DIM_COLOR);
	gfx_fill_rect(199, 13, 2, 6, DIM_COLOR);
	gfx_fill_rect(177, 11, 20, 10, BAR_COLOR);
	if (pct >= 0) {
		gfx_fill_rect(178, 12, 18 * pct / 100, 8, fill);
	}
	char value[5];
	if (pct < 0) {
		strcpy(value, "--");
	} else {
		snprintf(value, sizeof(value), "%u%%", (unsigned)(uint8_t)pct);
	}
	gfx_draw_text(204, 12, value, power ? COLOR_GREEN : FG_COLOR, BAR_COLOR);
}

static void draw_header(void)
{
	gfx_fill_rect(0, 0, LCD_WIDTH, HEADER_HEIGHT, BAR_COLOR);
	gfx_draw_text_box(8, 5, 132, "ROM LIBRARY", 0, HEADER_COLOR, BAR_COLOR);

	char counts[24];
	snprintf(counts, sizeof(counts), "%d game%s", g_rom_count, g_rom_count == 1 ? "" : "s");
	gfx_draw_text(8, 27, counts, DIM_COLOR, BAR_COLOR);
	draw_status(true);
}

static void draw_footer(void)
{
	int y = LCD_HEIGHT - FOOTER_HEIGHT;
	gfx_fill_rect(0, y, LCD_WIDTH, FOOTER_HEIGHT, BAR_COLOR);
	gfx_draw_text(8, y + 6, "A Play", HEADER_COLOR, BAR_COLOR);
	gfx_draw_text(72, y + 6, "SEL Refresh", DIM_COLOR, BAR_COLOR);
	char count[8];
	snprintf(count, sizeof(count), "%u/%u", (unsigned)(uint8_t)(g_rom_count ? g_selected + 1 : 0),
		 (unsigned)(uint8_t)g_rom_count);
	gfx_draw_text(LCD_WIDTH - 8 - strlen(count) * 8, y + 6, count, FG_COLOR, BAR_COLOR);
}

static void draw_list(void)
{
	int y = HEADER_HEIGHT;
	g_scroll_overflow = 0;
	g_scroll_offset = 0;
	gfx_fill_rect(0, y, LCD_WIDTH, LCD_HEIGHT - y - FOOTER_HEIGHT, BG_COLOR);
	draw_footer();

	if (g_rom_count == 0) {
		gfx_draw_text_box(8, y + 24, 224, "No ROM files", 0, FG_COLOR, BG_COLOR);
		gfx_draw_text(8, y + 52, "Use Wi-Fi ROM manager", DIM_COLOR, BG_COLOR);
		return;
	}

	int last = g_top + VISIBLE_LINES;
	if (last > g_rom_count) {
		last = g_rom_count;
	}
	for (int i = g_top; i < last; i++) {
		int row_y = y + (i - g_top) * LINE_HEIGHT;
		bool sel = (i == g_selected);
		uint16_t fg = sel ? HILITE_FG : FG_COLOR;
		uint16_t bg = sel ? HILITE_BG : ROW_COLOR;
		gfx_fill_rect(4, row_y + 1, LCD_WIDTH - 8, LINE_HEIGHT - 2, bg);
		const char *name = g_roms[i].name;
		char trim[NAME_W / GFX_LARGE_FONT_W + 1];
		if (sel) {
			gfx_draw_text_box(8, row_y + 5, 12, ">", 0, fg, bg);
			memcpy(g_scroll_name, g_roms[i].name, sizeof(g_scroll_name));
			g_scroll_name[sizeof(g_scroll_name) - 1] = '\0';
			g_scroll_selected = i;
			g_scroll_y = row_y + 5;
			g_scroll_overflow = MAX(0, (int)strlen(g_scroll_name) * GFX_LARGE_FONT_W - NAME_W);
			name = g_scroll_name;
		} else if (strlen(name) >= sizeof(trim)) {
			memcpy(trim, name, sizeof(trim) - 4);
			memcpy(trim + sizeof(trim) - 4, "...", 4);
			name = trim;
		}
		gfx_draw_text_box(NAME_X, row_y + 5, NAME_W, name, 0, fg, bg);
	}
	g_scroll_epoch = k_uptime_get();
}

static void tick_menu(void)
{
	int64_t now = k_uptime_get();
	if (now - g_status_time >= 1000) {
		draw_status(false);
	}
	if (!g_scroll_overflow || g_selected != g_scroll_selected ||
	    hud_overlaps(g_scroll_y + LCD_VIEW_Y, GFX_LARGE_FONT_H)) {
		return;
	}
	unsigned travel = DIV_ROUND_UP(g_scroll_overflow, SCROLL_STEP) * SCROLL_MS;
	unsigned phase = (now - g_scroll_epoch) % (2 * (SCROLL_HOLD + travel));
	unsigned offset;
	if (phase < SCROLL_HOLD) {
		offset = 0;
	} else if (phase < SCROLL_HOLD + travel) {
		offset = MIN(g_scroll_overflow, (phase - SCROLL_HOLD) / SCROLL_MS * SCROLL_STEP);
	} else if (phase < 2 * SCROLL_HOLD + travel) {
		offset = g_scroll_overflow;
	} else {
		offset = g_scroll_overflow - MIN(g_scroll_overflow,
			(phase - 2 * SCROLL_HOLD - travel) / SCROLL_MS * SCROLL_STEP);
	}
	if (offset != g_scroll_offset) {
		g_scroll_offset = offset;
		gfx_draw_text_box(NAME_X, g_scroll_y, NAME_W, g_scroll_name, offset,
				  HILITE_FG, HILITE_BG);
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
		struct emu_stats stats;
		emu_get_stats(&stats);
		int rc = k_sem_take(&g_kick, K_MSEC(!stats.running && g_scroll_overflow ? SCROLL_MS : 1000));
		emu_get_stats(&stats);
		if (rc) {
			if (!stats.running) {
				tick_menu();
			}
			continue;
		}

		atomic_val_t pending = atomic_set(&g_event_pending, 0);
		if (stats.running) {
			continue;
		}
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
