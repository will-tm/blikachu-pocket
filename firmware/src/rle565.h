/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_RLE565_H
#define BLIKACHU_RLE565_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rle565 {
	const uint16_t *next;
	const uint16_t *end;
	uint16_t count;
	uint16_t value;
};

/* Decode a bounded strip, retaining any unfinished run for the next strip. */
static inline bool rle565_read(struct rle565 *rle, uint16_t *dst, size_t pixels)
{
	while (pixels) {
		if (!rle->count) {
			if (rle->end - rle->next < 2) {
				return false;
			}
			rle->count = *rle->next++;
			rle->value = *rle->next++;
			if (!rle->count) {
				return false;
			}
		}
		size_t n = pixels < rle->count ? pixels : rle->count;

		for (size_t i = 0; i < n; ++i) {
			*dst++ = rle->value;
		}
		rle->count -= n;
		pixels -= n;
	}
	return true;
}

#endif
