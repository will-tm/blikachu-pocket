/*
 * Transient level overlay (volume / brightness). The image is rendered
 * into a static RGB565 buffer; presentation depends on who owns the
 * screen:
 *  - menu: drawn once directly, wiped by menu_refresh() on expiry.
 *  - emulator: the render thread composites the buffer over each
 *    display block (hud_overlay), so the HUD rides on top of the game
 *    until it expires and simply stops being composited.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "emu.h"
#include "gfx.h"
#include "hud.h"
#include "menu.h"
#include "psram_buf.h"

#define HUD_W 180
#define HUD_H 24
#define HUD_X ((LCD_WIDTH - HUD_W) / 2)
/* Sits near the bottom of the visible viewport. */
#define HUD_VIEW_Y  (LCD_HEIGHT - HUD_H - 8)
#define HUD_PANEL_Y (HUD_VIEW_Y + LCD_VIEW_Y)

#define HUD_BG     COLOR_DGRAY
#define HUD_BORDER COLOR_PIKACHU
#define HUD_TEXT   COLOR_WHITE
#define HUD_FILL   COLOR_PIKACHU

#define HUD_SHOW_MS 1500

static BLIKACHU_BIGBUF uint16_t g_hud_buf[HUD_H][HUD_W];
static atomic_t g_hud_active;

static void hud_expire_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	atomic_set(&g_hud_active, 0);

	struct emu_stats s;

	emu_get_stats(&s);
	if (!s.running) {
		/* Menu owns the screen — repaint to wipe the overlay. */
		menu_refresh();
	}
}
static K_WORK_DELAYABLE_DEFINE(g_hud_expire, hud_expire_work_fn);

static void hud_putc(int x, int y, char c)
{
	gfx_draw_char_buf(&g_hud_buf[0][0], HUD_W, HUD_H, x, y, c, HUD_TEXT);
}

static void hud_paint_box(const char *text, int text_y)
{
	/* Background + 1px border. */
	for (int y = 0; y < HUD_H; y++) {
		for (int x = 0; x < HUD_W; x++) {
			bool edge = (y == 0 || y == HUD_H - 1 || x == 0 || x == HUD_W - 1);

			g_hud_buf[y][x] = edge ? HUD_BORDER : HUD_BG;
		}
	}

	int tx = 6;

	for (const char *p = text; *p && tx + 8 < HUD_W; p++, tx += 8) {
		hud_putc(tx, text_y, *p);
	}
}

static void hud_present(void)
{
	atomic_set(&g_hud_active, 1);
	k_work_reschedule(&g_hud_expire, K_MSEC(HUD_SHOW_MS));

	struct emu_stats s;

	emu_get_stats(&s);
	if (!s.running) {
		gfx_draw_image(HUD_X, HUD_VIEW_Y, HUD_W, HUD_H, &g_hud_buf[0][0]);
	}
}

void hud_show_text(const char *text)
{
	hud_paint_box(text, (HUD_H - 8) / 2);
	hud_present();
}

void hud_show_level(const char *label, int val, int max)
{
	char text[24];

	snprintf(text, sizeof(text), "%s %d/%d", label, val, max);

	hud_paint_box(text, 3);

	/* Progress bar under the text: 4..HUD_W-4, rows 14..19. */
	int bar_x0 = 6, bar_x1 = HUD_W - 6;
	int fill = bar_x0;

	if (max > 0) {
		fill = bar_x0 + (bar_x1 - bar_x0) * val / max;
	}
	for (int y = 14; y < 20; y++) {
		for (int x = bar_x0; x < bar_x1; x++) {
			g_hud_buf[y][x] = (x < fill) ? HUD_FILL : COLOR_GRAY;
		}
	}

	atomic_set(&g_hud_active, 1);
	k_work_reschedule(&g_hud_expire, K_MSEC(HUD_SHOW_MS));

	struct emu_stats s;

	emu_get_stats(&s);
	if (!s.running) {
		gfx_draw_image(HUD_X, HUD_VIEW_Y, HUD_W, HUD_H, &g_hud_buf[0][0]);
	}
}

bool hud_overlaps(int panel_y, int rows)
{
	return atomic_get(&g_hud_active) && panel_y < HUD_PANEL_Y + HUD_H &&
	       panel_y + rows > HUD_PANEL_Y;
}

bool hud_overlay(int panel_y, int rows, uint16_t *buf, int stride)
{
	if (!atomic_get(&g_hud_active)) {
		return false;
	}

	int y0 = MAX(panel_y, HUD_PANEL_Y);
	int y1 = MIN(panel_y + rows, HUD_PANEL_Y + HUD_H);

	if (y0 >= y1) {
		return false;
	}
	for (int y = y0; y < y1; y++) {
		memcpy(buf + (y - panel_y) * stride + HUD_X, g_hud_buf[y - HUD_PANEL_Y],
		       HUD_W * sizeof(uint16_t));
	}
	return true;
}
