/*
 * Tiny graphics helpers backed by the chosen `zephyr,display` device.
 * No persistent framebuffer: each draw stages into a small per-call
 * buffer and pushes via display_write(). For the menu workload (mostly
 * static text) the cost is negligible.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "gfx.h"
#include "external/font8x8_basic.h"
#include "psram_buf.h"
#include "rle565.h"

LOG_MODULE_REGISTER(gfx, LOG_LEVEL_INF);

static const struct device *display_dev;

/* ---- Backlight (GPIO28/PWM0 CH0, Q2 low-side switch) ---- */

static int g_brightness = GFX_BRIGHT_DEFAULT;

static void backlight_apply(int level)
{
	/* Quadratic: LED luminance vs perception — level 1 ≈ 1% duty,
	 * 10 = 100%.
	 */
	extern void platform_backlight_set_pct(int pct);

	platform_backlight_set_pct(100 * level * level /
				   (GFX_BRIGHT_MAX * GFX_BRIGHT_MAX));
}

int gfx_backlight_set(int level)
{
	g_brightness = CLAMP(level, 1, GFX_BRIGHT_MAX);
	backlight_apply(g_brightness);
	return g_brightness;
}

int gfx_backlight_get(void)
{
	return g_brightness;
}

void gfx_backlight_off(void)
{
	extern void platform_backlight_set_pct(int pct);

	platform_backlight_set_pct(0);
}

/* ---- Tear-effect v-blank sync (TE on GPIO20) ----
 *
 * The panel pulses TE high at every v-blank once TEON is enabled — the
 * Zephyr ST7789V driver never sends it, so we do, through the same MIPI
 * DBI path the driver uses. gfx_te_wait() then lets the game renderer
 * start its GRAM push at v-blank and race ahead of the scan-out beam
 * (push ≈ 14 ms vs ≈ 16.7 ms scan, plus the 24-row viewport head
 * start), eliminating the drifting tear band.
 */

#define ST7789V_CMD_CASET 0x2a
#define ST7789V_CMD_RASET 0x2b
#define ST7789V_CMD_RAMWR 0x2c
#define ST7789V_CMD_TEON  0x35 /* param 0x00 = pulse on v-blank only */

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

static const struct gpio_dt_spec te_gpio =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), te_gpios);
static const struct device *const te_mipi = DEVICE_DT_GET(DT_PARENT(DISPLAY_NODE));
static const struct mipi_dbi_config te_dbi_config =
	MIPI_DBI_CONFIG_DT(DISPLAY_NODE, SPI_OP_MODE_MASTER | SPI_WORD_SET(8), 0);

static struct gpio_callback te_cb;
static struct k_sem te_sem;
static bool te_ready;
static volatile uint32_t te_last_cyc;
static volatile uint32_t te_period_cyc;

static void te_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	uint32_t now = k_cycle_get_32();

	te_period_cyc = now - te_last_cyc;
	te_last_cyc = now;
	k_sem_give(&te_sem);
}

static void te_init(void)
{
	uint8_t tem = 0x00;

	if (!device_is_ready(te_gpio.port) || !device_is_ready(te_mipi) ||
	    mipi_dbi_command_write(te_mipi, &te_dbi_config, ST7789V_CMD_TEON, &tem, 1) != 0) {
		LOG_WRN("TE unavailable — display pushes stay unsynced");
		return;
	}
	k_sem_init(&te_sem, 0, 1);
	(void)gpio_pin_configure_dt(&te_gpio, GPIO_INPUT);
	gpio_init_callback(&te_cb, te_isr, BIT(te_gpio.pin));
	(void)gpio_add_callback(te_gpio.port, &te_cb);
	if (gpio_pin_interrupt_configure_dt(&te_gpio, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		LOG_WRN("TE irq config failed — display pushes stay unsynced");
		return;
	}
	te_ready = true;
}

bool gfx_te_sync(uint32_t delay_us, int32_t timeout_ms)
{
	if (!te_ready) {
		return false;
	}
	/* Phase-based: the target is `delay_us` past the most recent TE
	 * edge. If we got here before that point (e.g. the previous push
	 * spilled just past the edge), sleep only the remainder — waiting
	 * for the NEXT edge would stall a whole extra period and halve
	 * the display rate. Only when the edge is already further away
	 * than delay_us (mid-scan, first frame, after a pause) do we wait
	 * for a fresh pulse.
	 */
	uint32_t since_us = k_cyc_to_us_floor32(k_cycle_get_32() - te_last_cyc);
	uint32_t latest_start_us = delay_us;

	/* A 12 ms transfer may start up to 6 ms after TE: the viewport
	 * starts 24 scanlines below the top, before the next scan reaches
	 * it. Accept the bounded late phase instead of skipping a refresh.
	 */
	latest_start_us += 3000;
	if (since_us >= latest_start_us) {
		k_sem_reset(&te_sem);
		if (k_sem_take(&te_sem, K_MSEC(timeout_ms)) != 0) {
			return false;
		}
		since_us = 0;
	}
	if (since_us < delay_us) {
		k_usleep(delay_us - since_us);
	}
	return true;
}

uint32_t gfx_te_period_us(void)
{
	return k_cyc_to_us_floor32(te_period_cyc);
}

/* ---- Direct blit path for the game renderer ----
 *
 * display_write() re-sends CASET/RASET/RAMWR and sets up DMA for every
 * call — 4 SPI transactions per row-block, ~4.6 ms/frame of pure
 * overhead at 9 blocks. The ST7789V write pointer auto-advances across
 * the whole window, so the emulator instead sets the window ONCE per
 * frame (gfx_blit_window) and streams the blocks as bare DC-high data
 * bursts (gfx_blit_data) — RAMWR stays the active command between
 * bursts. Panel coordinates; only safe while nothing else draws (the
 * menu/HUD gfx_* calls never run concurrently with the game push).
 */

int gfx_blit_window(int x, int y, int w, int h)
{
	uint16_t d[2];
	int rc;

	d[0] = sys_cpu_to_be16(x);
	d[1] = sys_cpu_to_be16(x + w - 1);
	rc = mipi_dbi_command_write(te_mipi, &te_dbi_config, ST7789V_CMD_CASET,
				    (uint8_t *)d, 4);
	if (rc < 0) {
		return rc;
	}
	d[0] = sys_cpu_to_be16(y);
	d[1] = sys_cpu_to_be16(y + h - 1);
	rc = mipi_dbi_command_write(te_mipi, &te_dbi_config, ST7789V_CMD_RASET,
				    (uint8_t *)d, 4);
	if (rc < 0) {
		return rc;
	}
	return mipi_dbi_command_write(te_mipi, &te_dbi_config, ST7789V_CMD_RAMWR, NULL, 0);
}

int gfx_blit_data(const void *buf, size_t len)
{
	struct display_buffer_descriptor desc = {
		.buf_size = len,
		.width = LCD_WIDTH,
		.pitch = LCD_WIDTH,
		.height = len / (LCD_WIDTH * sizeof(uint16_t)),
	};

	return mipi_dbi_write_display(te_mipi, &te_dbi_config, buf, &desc,
				      PIXEL_FORMAT_RGB_565);
}

/* Reusable line buffer big enough to fill any width-aligned slice in one
 * SPI transaction. 240 px × RGB565 = 480 B; 32 lines fit in <16 KiB.
 */
#define MAX_BLIT_ROWS 32
static BLIKACHU_BIGBUF uint16_t g_blit_buf[LCD_WIDTH * MAX_BLIT_ROWS];

static void panel_write(int x, int y, const struct display_buffer_descriptor *desc,
			const void *buf)
{
	int rc = display_write(display_dev, x, y, desc, buf);

	if (rc) {
		rc = display_write(display_dev, x, y, desc, buf);
		if (rc) {
			LOG_ERR_RATELIMIT("panel write (%d,%d) failed: %d", x, y, rc);
		}
	}
}

static void panel_fill(int x, int y_panel, int w, int h, uint16_t color)
{
	struct display_buffer_descriptor desc = {
		.width = w,
		.pitch = w,
	};

	int remaining = h;
	int row = y_panel;
	while (remaining > 0) {
		int chunk = remaining > MAX_BLIT_ROWS ? MAX_BLIT_ROWS : remaining;
		int n = w * chunk;
		for (int i = 0; i < n; i++) {
			g_blit_buf[i] = color;
		}
		desc.height = chunk;
		desc.buf_size = n * sizeof(uint16_t);
		panel_write(x, row, &desc, g_blit_buf);
		row += chunk;
		remaining -= chunk;
	}
}

int gfx_init(void)
{
	if (display_dev) {
		return 0;
	}
	display_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
	if (!display_dev || !device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		display_dev = NULL;
		return -ENODEV;
	}
	(void)display_blanking_off(display_dev);
	te_init();
	panel_fill(0, 0, LCD_PANEL_W, LCD_PANEL_H, COLOR_BLACK);
	backlight_apply(g_brightness);
	return 0;
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
	if (!display_dev || w <= 0 || h <= 0) {
		return;
	}
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > LCD_WIDTH) {
		w = LCD_WIDTH - x;
	}
	if (y + h > LCD_HEIGHT) {
		h = LCD_HEIGHT - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}

	panel_fill(x, y + LCD_VIEW_Y, w, h, color);
}

void gfx_clear(uint16_t color)
{
	if (!display_dev) {
		return;
	}
	panel_fill(0, 0, LCD_PANEL_W, LCD_PANEL_H, color);
}

void gfx_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
	if (!display_dev) {
		return;
	}
	if (x < 0 || y < 0 || x + 8 > LCD_WIDTH || y + 8 > LCD_HEIGHT) {
		return;
	}
	uint8_t idx = (uint8_t)c;
	if (idx >= 128) {
		idx = '?';
	}
	const char *glyph = font8x8_basic[idx];
	uint16_t buf[8 * 8];
	for (int row = 0; row < 8; row++) {
		uint8_t bits = (uint8_t)glyph[row];
		for (int col = 0; col < 8; col++) {
			buf[row * 8 + col] = (bits & (1 << col)) ? fg : bg;
		}
	}
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(buf),
		.width = 8,
		.height = 8,
		.pitch = 8,
	};
	panel_write(x, y + LCD_VIEW_Y, &desc, buf);
}

void gfx_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
	while (*s && x + 8 <= LCD_WIDTH) {
		gfx_draw_char(x, y, *s++, fg, bg);
		x += 8;
	}
}

void gfx_draw_text_box(int x, int y, int w, const char *s, unsigned offset_px,
		       uint16_t fg, uint16_t bg)
{
	if (!display_dev || x < 0 || x >= LCD_WIDTH || y < 0 ||
	    y + GFX_LARGE_FONT_H > LCD_HEIGHT || w <= 0) {
		return;
	}
	w = MIN(w, LCD_WIDTH - x);
	size_t len = strlen(s);

	for (int row = 0; row < GFX_LARGE_FONT_H; row++) {
		for (int col = 0; col < w; col++) {
			unsigned px = offset_px + col;
			unsigned idx = px / GFX_LARGE_FONT_W;
			uint8_t ch = idx < len ? (uint8_t)s[idx] : ' ';
			uint8_t bits = font8x8_basic[ch < 128 ? ch : '?'][row / 2];
			unsigned bit = (px % GFX_LARGE_FONT_W) * 8 / GFX_LARGE_FONT_W;

			g_blit_buf[row * w + col] = bits & BIT(bit) ? fg : bg;
		}
	}
	struct display_buffer_descriptor desc = {
		.buf_size = w * GFX_LARGE_FONT_H * sizeof(uint16_t),
		.width = w,
		.pitch = w,
		.height = GFX_LARGE_FONT_H,
	};
	panel_write(x, y + LCD_VIEW_Y, &desc, g_blit_buf);
}

void gfx_draw_char_buf(uint16_t *buf, int stride, int buf_h, int x, int y, char c, uint16_t fg)
{
	uint8_t idx = (uint8_t)c;

	if (idx >= 128) {
		idx = '?';
	}
	const char *glyph = font8x8_basic[idx];

	for (int row = 0; row < 8 && y + row < buf_h; row++) {
		uint8_t bits = (uint8_t)glyph[row];

		for (int col = 0; col < 8 && x + col < stride; col++) {
			if (bits & (1 << col)) {
				buf[(y + row) * stride + (x + col)] = fg;
			}
		}
	}
}

int gfx_draw_image_rle(int x, int y, int w, int h, const uint16_t *words, size_t count)
{
	if (!display_dev || !words || w <= 0 || h <= 0 || x < 0 ||
	    w > LCD_PANEL_W || x > LCD_PANEL_W - w ||
	    h > LCD_PANEL_H || y < -LCD_VIEW_Y ||
	    y > LCD_PANEL_H - LCD_VIEW_Y - h) {
		return -EINVAL;
	}
	struct rle565 rle = { .next = words, .end = words + count };
	struct display_buffer_descriptor desc = { .width = w, .pitch = w };

	for (int row = 0; row < h;) {
		int rows = MIN(h - row, MAX_BLIT_ROWS);

		if (!rle565_read(&rle, g_blit_buf, (size_t)w * rows)) {
			return -EINVAL;
		}
		desc.height = rows;
		desc.buf_size = (size_t)w * rows * sizeof(uint16_t);
		int rc = display_write(display_dev, x, y + LCD_VIEW_Y + row, &desc, g_blit_buf);

		if (rc < 0) {
			return rc;
		}
		row += rows;
	}
	return rle.count == 0 && rle.next == rle.end ? 0 : -EINVAL;
}

void gfx_draw_image(int x, int y, int w, int h, const uint16_t *pixels)
{
	if (!display_dev || w <= 0 || h <= 0) {
		return;
	}
	/* display_write() crawls on these const/XIP images (measured
	 * ~210 ms for a 240x177 boot-anim frame — it made the boot
	 * animation ~5x longer than its frame delays). Use the windowed
	 * gapless-burst path instead (~14 ms for a full frame), bouncing
	 * through an SRAM stripe since the burst DMA wants a RAM source.
	 * GRAM write pointer auto-advances, so stripes need no row
	 * alignment — it's one byte stream.
	 */
	static BLIKACHU_BIGBUF uint16_t stripe[240 * 24];

	if (gfx_blit_window(x, y + LCD_VIEW_Y, w, h) < 0) {
		return;
	}
	size_t left = (size_t)w * h;

	while (left > 0) {
		size_t n = MIN(left, ARRAY_SIZE(stripe));

		memcpy(stripe, pixels, n * sizeof(uint16_t));
		(void)gfx_blit_data(stripe, n * sizeof(uint16_t));
		pixels += n;
		left -= n;
	}
}
