/* SPDX-License-Identifier: Apache-2.0 */
#include "libretro/video.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct lr_frame frame, previous;
static uint16_t output[LR_VIEW_W * LR_VIEW_H];
static uint16_t highres[512 * 478];

int main(void)
{
	/* Two source rows with odd-byte pitch and padding. */
	uint8_t source[10] = {0};
	const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
	memcpy(source, colors, 4);
	memcpy(source + 5, colors + 2, 4);
	assert(lr_video_store(&frame, source, 2, 2, 5, RETRO_PIXEL_FORMAT_RGB565, 1.0f) == 0);
	assert(frame.view_x == 12 && frame.view_y == 0 && frame.view_w == 216);
	lr_video_strip(&frame, 0, 108, output);
	lr_video_strip(&frame, 108, 108, output + 108 * LR_VIEW_W);
	assert(output[0] == 0 && output[12] == 0x00f8 && output[120] == 0xe007);
	assert(output[108 * LR_VIEW_W + 12] == 0x1f00);
	assert(output[108 * LR_VIEW_W + 120] == 0xffff);
	assert(output[LR_VIEW_W - 1] == 0);
	previous = frame;
	assert(lr_video_strip_equal(&frame, &previous, 0, 108));
	frame.pixels[2] ^= 1;
	assert(lr_video_strip_equal(&frame, &previous, 0, 108));
	assert(!lr_video_strip_equal(&frame, &previous, 108, 108));
	frame = previous;
	frame.view_x++;
	assert(!lr_video_strip_equal(&frame, &previous, 0, 108));
	frame = previous;

	const uint16_t rgb555[] = {0x7c00, 0x03e0, 0x001f, 0x7fff};
	assert(lr_video_store(&frame, rgb555, 4, 1, 8, RETRO_PIXEL_FORMAT_0RGB1555, 4.0f) == 0);
	assert(frame.view_w == 240 && frame.view_h == 60 && frame.view_y == 78);
	lr_video_strip(&frame, 77, 2, output);
	assert(output[0] == 0 && output[LR_VIEW_W] == 0x00f8);
	assert(output[LR_VIEW_W + 60] == 0xe007 && output[LR_VIEW_W + 120] == 0x1f00);
	assert(output[LR_VIEW_W + 180] == 0xffff);

	for (unsigned y = 0; y < 478; ++y) {
		for (unsigned x = 0; x < 512; ++x) {
			highres[y * 512 + x] = (uint16_t)(x ^ y);
		}
	}
	assert(lr_video_store(&frame, highres, 512, 478, 1024, RETRO_PIXEL_FORMAT_RGB565, 4.0f / 3) == 0);
	assert(frame.width == 256 && frame.height == 240 && frame.view_h == 180 && frame.view_y == 18);
	assert(frame.pixels[100 * frame.width + 100] == (200 ^ (100 * 478 / 240)));
	/* A true equality result must always produce identical wire pixels,
	 * including scaling, letterboxing and the two supported formats. */
	previous = frame;
	static uint16_t prior_strip[LR_VIEW_W * 32];
	for (unsigned i = 0; i < 64; ++i) {
		unsigned pixel = (i * 997) % (frame.width * frame.height);
		frame.pixels[pixel] ^= 0xffff;
		for (unsigned y = 0; y < LR_VIEW_H; y += 32) {
			unsigned rows = LR_VIEW_H - y < 32 ? LR_VIEW_H - y : 32;
			if (lr_video_strip_equal(&frame, &previous, y, rows)) {
				lr_video_strip(&frame, y, rows, output);
				lr_video_strip(&previous, y, rows, prior_strip);
				assert(!memcmp(output, prior_strip, rows * LR_VIEW_W * sizeof(uint16_t)));
			}
		}
		frame.pixels[pixel] ^= 0xffff;
	}
	assert(lr_video_strip_equal(&frame, &previous, 0, LR_VIEW_H));
	assert(lr_video_store(&frame, source, 2, 2, 3, RETRO_PIXEL_FORMAT_RGB565, 1) == -EINVAL);
	assert(lr_video_store(&frame, NULL, 2, 2, 4, RETRO_PIXEL_FORMAT_RGB565, 1) == -EINVAL);
	assert(lr_video_store(&frame, source, 2, 2, 4, RETRO_PIXEL_FORMAT_XRGB8888, 1) == -EINVAL);
	puts("libretro video: aspect fit, strip boundaries, pitch, RGB565/1555 and high-res sampling passed");
	return 0;
}
