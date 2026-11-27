/*
 * One-shot boot animation from assets/boot.gif (frames live in the XIP
 * code partition as const data). Plays before the menu starts; any
 * button press skips the remainder.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "bootanim.h"
#include "gfx.h"
#include "input.h"
#include "boot_anim.h" /* generated from assets/boot.gif */

LOG_MODULE_REGISTER(bootanim, LOG_LEVEL_INF);

static atomic_t g_skip;
static int64_t g_anim_start;

/* Grace period: waking from deep sleep requires pressing a button, and
 * gpio-keys reports that held button as a fresh press right at boot —
 * which used to skip the animation instantly. Ignore presses in the
 * first half second.
 */
#define SKIP_GRACE_MS 500

static void skip_cb(struct input_event *evt)
{
	if (evt->type == INPUT_EV_KEY && evt->value &&
	    (k_uptime_get() - g_anim_start) > SKIP_GRACE_MS) {
		atomic_set(&g_skip, 1);
	}
}

/* Letterbox fill matching the animation's background (first pixel). */
#define BOOT_ANIM_BG RGB565(0x22, 0x46, 0x70)

/* Cap per-frame hold time — the GIF is authored at 50-100 ms/frame
 * (plus a 1 s hold on the last); the cap plays all 12 frames in ~0.5 s
 * with draw time included.
 */
#define BOOT_ANIM_MAX_FRAME_MS 38

void boot_anim_play(void)
{
	if (gfx_init() != 0) {
		return;
	}

	atomic_set(&g_skip, 0);
	g_anim_start = k_uptime_get();
	input_set_handler(skip_cb);

	gfx_clear(BOOT_ANIM_BG);

	/* Bottom-aligned on the physical panel, horizontally centered.
	 * (gfx_draw_image applies the +LCD_VIEW_Y viewport offset —
	 * compensate to address panel coordinates.)
	 */
	int x = (LCD_PANEL_W - BOOT_ANIM_W) / 2;
	int y = (LCD_PANEL_H - BOOT_ANIM_H) - LCD_VIEW_Y;

	for (int i = 0; i < BOOT_ANIM_FRAMES && !atomic_get(&g_skip); i++) {
		uint32_t offset = boot_anim_offsets[i];
		int rc = gfx_draw_image_rle(x, y, BOOT_ANIM_W, BOOT_ANIM_H,
					  boot_anim_rle + offset,
					  boot_anim_offsets[i + 1] - offset);

		if (rc != 0) {
			LOG_ERR("boot frame %d: %d", i, rc);
			break;
		}

		/* Sleep in small slices so a skip press cuts quickly. */
		int remaining = MIN(boot_anim_delay_ms[i], BOOT_ANIM_MAX_FRAME_MS);

		while (remaining > 0 && !atomic_get(&g_skip)) {
			int slice = MIN(remaining, 25);

			k_msleep(slice);
			remaining -= slice;
		}
	}

	input_set_handler(NULL);
	LOG_INF("boot anim done in %d ms (%d frames, cap %d ms)",
		(int)(k_uptime_get() - g_anim_start), BOOT_ANIM_FRAMES, BOOT_ANIM_MAX_FRAME_MS);
}
