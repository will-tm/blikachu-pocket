#ifndef BLIKACHU_GFX_H
#define BLIKACHU_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCD_PANEL_W 240
#define LCD_PANEL_H 240
#define LCD_VIEW_Y  24
#define LCD_WIDTH   240
#define LCD_HEIGHT  216

/* RGB565 (LCD wire-order, big-endian as written by the ST7789V driver). */
#define RGB565(r, g, b) __builtin_bswap16((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define COLOR_BLACK  RGB565(0, 0, 0)
#define COLOR_WHITE  RGB565(0xFF, 0xFF, 0xFF)
#define COLOR_GRAY   RGB565(0x60, 0x60, 0x60)
#define COLOR_DGRAY  RGB565(0x20, 0x20, 0x20)
#define COLOR_GREEN  RGB565(0x10, 0xC0, 0x40)
#define COLOR_RED    RGB565(0xE0, 0x10, 0x10)
#define COLOR_YELLOW RGB565(0xF0, 0xC0, 0x00)
#define COLOR_PIKACHU RGB565(0xFF, 0xDE, 0x00) /* Pikachu yellow */

/* Bring up the display device + backlight. Idempotent. Returns 0 on
 * success.
 */
int gfx_init(void);

/* Block until `delay_us` past the panel's v-blank (TE pulse on GPIO20).
 * Phase-based: if the last edge is less than delay_us old, only the
 * remainder is slept — a push that spills slightly past an edge doesn't
 * stall a whole period. Waits for a fresh pulse (up to timeout_ms)
 * otherwise. False on timeout / TE unavailable (caller pushes
 * unsynced).
 */
bool gfx_te_sync(uint32_t delay_us, int32_t timeout_ms);

/* Measured interval between the last two TE pulses in µs (0 until two
 * pulses have been seen). Expected ≈ the panel refresh period.
 */
uint32_t gfx_te_period_us(void);

/* Low-overhead blit path for the game renderer: set the GRAM window
 * (panel coordinates) + RAMWR once, then stream pixel data in any
 * number of bursts — the write pointer auto-advances. Not for use
 * concurrently with the gfx_* drawing calls.
 */
int gfx_blit_window(int x, int y, int w, int h);
int gfx_blit_data(const void *buf, size_t len);

/* Backlight brightness 1..GFX_BRIGHT_MAX (never 0 — use
 * gfx_backlight_off() for the sleep path). Quadratic duty curve for a
 * perceptually even ramp. Returns the clamped applied level.
 */
#define GFX_BRIGHT_MAX     10
#define GFX_BRIGHT_DEFAULT 7
int gfx_backlight_set(int level);
int gfx_backlight_get(void);
void gfx_backlight_off(void);

/* Fill the entire screen with `color`. */
void gfx_clear(uint16_t color);

/* Fill a rectangle [x..x+w-1, y..y+h-1] with `color`. Clipped to screen. */
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draw a single 8x8 ASCII character. Bounds-clipped at the right and
 * bottom edges (off-screen chars become a no-op).
 */
void gfx_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg);

/* Render a NUL-terminated string at (x,y). Stops at the right edge or
 * NUL. Each char is 8x8 px wide.
 */
void gfx_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/* Draw a clipped 12x16 text viewport, with a horizontal pixel offset. */
#define GFX_LARGE_FONT_W 12
#define GFX_LARGE_FONT_H 16
void gfx_draw_text_box(int x, int y, int w, const char *s, unsigned offset_px,
		       uint16_t fg, uint16_t bg);

/* Draw one 8x8 glyph into a caller-owned RGB565 buffer (transparent
 * background: only foreground pixels are written). `stride` is the
 * buffer width in pixels, `buf_h` its height; clipped at the edges.
 */
void gfx_draw_char_buf(uint16_t *buf, int stride, int buf_h, int x, int y, char c, uint16_t fg);

/* Blit a pre-rendered RGB565 image (big-endian, row-major). */
void gfx_draw_image(int x, int y, int w, int h, const uint16_t *pixels);

/* Count/value RLE image; coordinates must fit the physical panel. */
int gfx_draw_image_rle(int x, int y, int w, int h, const uint16_t *words, size_t count);

#endif
