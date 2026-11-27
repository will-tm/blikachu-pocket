/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_VIDEO_H
#define BLIKACHU_LIBRETRO_VIDEO_H

#include <libretro.h>
#include <stdbool.h>

#define LR_SOURCE_W 256
#define LR_SOURCE_H 240
#define LR_VIEW_W 240
#define LR_VIEW_H 216

/* Bounded copy of the core's transient video callback. High-resolution
 * SNES fields are sampled down before publication; the original aspect
 * ratio determines the viewport, not the stored dimensions. */
struct lr_frame {
	unsigned width, height;
	unsigned view_x, view_y, view_w, view_h;
	enum retro_pixel_format format;
	uint16_t pixels[LR_SOURCE_W * LR_SOURCE_H];
};

int lr_video_store(struct lr_frame *frame, const void *pixels, unsigned width,
		   unsigned height, size_t pitch, enum retro_pixel_format format, float aspect);
bool lr_video_strip_equal(const struct lr_frame *a, const struct lr_frame *b,
			  unsigned y, unsigned rows);
void lr_video_strip(const struct lr_frame *frame, unsigned y, unsigned rows, uint16_t *dst);
#endif
