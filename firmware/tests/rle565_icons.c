/* SPDX-License-Identifier: Apache-2.0 */
#include "rle565.h"
#include "sleep_icon.h"
#include "usb_icon.h"
#include <assert.h>
#include <stdio.h>

static void check(const uint16_t *words, size_t count, size_t pixels, const char *path)
{
	struct rle565 decoder = { .next = words, .end = words + count };
	FILE *reference = fopen(path, "rb");
	uint16_t strip[23];

	assert(reference);
	while (pixels) {
		size_t n = pixels < 23 ? pixels : 23;

		assert(rle565_read(&decoder, strip, n));
		for (size_t i = 0; i < n; ++i) {
			int low = fgetc(reference);
			int high = fgetc(reference);

			assert(low != EOF && high != EOF);
			assert(strip[i] == (uint16_t)(low | high << 8));
		}
		pixels -= n;
	}
	assert(decoder.next == decoder.end && decoder.count == 0);
	assert(fgetc(reference) == EOF);
	fclose(reference);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	check(sleep_icon_rle, sizeof(sleep_icon_rle) / sizeof(sleep_icon_rle[0]),
	      SLEEP_ICON_W * SLEEP_ICON_H, argv[1]);
	check(usb_icon_rle, sizeof(usb_icon_rle) / sizeof(usb_icon_rle[0]),
	      USB_ICON_W * USB_ICON_H, argv[2]);
	puts("RLE565 icons: every decoded pixel matches the raw reference");
	return 0;
}
