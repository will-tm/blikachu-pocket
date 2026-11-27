/* SPDX-License-Identifier: Apache-2.0 */
#include "video.h"

#include <errno.h>
#include <string.h>

int lr_video_store(struct lr_frame *frame, const void *pixels, unsigned width,
		   unsigned height, size_t pitch, enum retro_pixel_format format, float aspect)
{
	if (!pixels || !width || !height || width > 512 || height > 478 || pitch < width * 2 ||
	    pitch > SIZE_MAX / height ||
	    (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_0RGB1555)) {
		return -EINVAL;
	}
	if (!(aspect > 0.01f && aspect < 100.0f)) {
		aspect = (float)width / height;
	}
	frame->width = width > LR_SOURCE_W ? LR_SOURCE_W : width;
	frame->height = height > LR_SOURCE_H ? LR_SOURCE_H : height;
	frame->format = format;
	frame->view_w = LR_VIEW_W;
	frame->view_h = LR_VIEW_H;
	if (aspect >= (float)LR_VIEW_W / LR_VIEW_H) {
		frame->view_h = (unsigned)(LR_VIEW_W / aspect + 0.5f);
	} else {
		frame->view_w = (unsigned)(LR_VIEW_H * aspect + 0.5f);
	}
	frame->view_x = (LR_VIEW_W - frame->view_w) / 2;
	frame->view_y = (LR_VIEW_H - frame->view_h) / 2;
	for (unsigned y = 0; y < frame->height; ++y) {
		const uint8_t *src = (const uint8_t *)pixels + (size_t)(y * height / frame->height) * pitch;
		uint16_t *dst = frame->pixels + y * frame->width;

		if (frame->width == width) {
			memcpy(dst, src, width * 2);
		} else {
			for (unsigned x = 0; x < frame->width; ++x) {
				memcpy(dst + x, src + (x * width / frame->width) * 2, 2);
			}
		}
	}
	return 0;
}

/* Compare the native rows sampled by the strip. Comparing any extra
 * native pixels is conservative: it may cause a redundant LCD write. */
bool lr_video_strip_equal(const struct lr_frame *a, const struct lr_frame *b,
			  unsigned y, unsigned rows)
{
	if (a->width != b->width || a->height != b->height ||
	    a->view_x != b->view_x || a->view_y != b->view_y ||
	    a->view_w != b->view_w || a->view_h != b->view_h || a->format != b->format) {
		return false;
	}
	unsigned first = y > a->view_y ? y : a->view_y;
	unsigned last = y + rows < a->view_y + a->view_h ? y + rows : a->view_y + a->view_h;

	if (first >= last || !a->width || !a->view_h) {
		return true;
	}
	unsigned sy0 = (first - a->view_y) * a->height / a->view_h;
	unsigned sy1 = (last - 1 - a->view_y) * a->height / a->view_h;
	size_t offset = sy0 * a->width;
	size_t bytes = (sy1 - sy0 + 1) * a->width * sizeof(uint16_t);
	return memcmp(a->pixels + offset, b->pixels + offset, bytes) == 0;
}

static inline uint16_t wire_color(uint16_t pixel, enum retro_pixel_format format)
{
	if (format == RETRO_PIXEL_FORMAT_0RGB1555) {
		pixel = ((pixel & 0x7fe0) << 1) | ((pixel & 0x0200) >> 4) | (pixel & 0x1f);
	}
	return __builtin_bswap16(pixel);
}

void lr_video_strip(const struct lr_frame *frame, unsigned y, unsigned rows, uint16_t *dst)
{
	uint16_t xmap[LR_VIEW_W];
	for (unsigned x = 0; x < frame->view_w; ++x) {
		xmap[x] = x * frame->width / frame->view_w;
	}
	unsigned previous = UINT32_MAX;
	for (unsigned row = 0; row < rows; ++row, ++y, dst += LR_VIEW_W) {
		if (!frame->width || y < frame->view_y || y >= frame->view_y + frame->view_h) {
			memset(dst, 0, LR_VIEW_W * 2);
			continue;
		}
		unsigned sy = (y - frame->view_y) * frame->height / frame->view_h;
		if (sy == previous) {
			memcpy(dst, dst - LR_VIEW_W, LR_VIEW_W * 2);
			continue;
		}
		previous = sy;
		const uint16_t *src = frame->pixels + sy * frame->width;
		memset(dst, 0, frame->view_x * 2);
		for (unsigned x = 0; x < frame->view_w; ++x) {
			dst[frame->view_x + x] = wire_color(src[xmap[x]], frame->format);
		}
		unsigned end = frame->view_x + frame->view_w;
		memset(dst + end, 0, (LR_VIEW_W - end) * 2);
	}
}
