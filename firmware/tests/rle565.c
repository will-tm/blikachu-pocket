/* SPDX-License-Identifier: Apache-2.0 */
#include "rle565.h"
#include "boot_anim.h"

#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	const uint16_t words[] = { 3, 0x1234, 2, 0xabcd };
	struct rle565 state = { .next = words, .end = words + 4 };
	uint16_t out[7];

	assert(rle565_read(&state, out, 2));
	assert(out[0] == 0x1234 && out[1] == 0x1234 && state.count == 1);
	assert(rle565_read(&state, out, 3));
	assert(out[0] == 0x1234 && out[1] == 0xabcd && out[2] == 0xabcd);
	assert(state.count == 0 && state.next == state.end);
	assert(!rle565_read(&state, out, 1));
	const uint16_t zero[] = { 0, 0xffff };
	state = (struct rle565){ .next = zero, .end = zero + 2 };
	assert(!rle565_read(&state, out, 1));
	state = (struct rle565){ .next = words, .end = words + 1 };
	assert(!rle565_read(&state, out, 1));

	assert(argc == 2);
	FILE *expected = fopen(argv[1], "rb");
	assert(expected);
	for (unsigned frame = 0; frame < BOOT_ANIM_FRAMES; ++frame) {
		state = (struct rle565){
			.next = boot_anim_rle + boot_anim_offsets[frame],
			.end = boot_anim_rle + boot_anim_offsets[frame + 1],
		};
		for (size_t i = 0; i < BOOT_ANIM_W * BOOT_ANIM_H;) {
			size_t count = BOOT_ANIM_W * BOOT_ANIM_H - i;
			if (count > 7) {
				count = 7;
			}
			assert(rle565_read(&state, out, count));
			for (size_t j = 0; j < count; ++j) {
				int low = fgetc(expected);
				int high = fgetc(expected);
				assert(low != EOF && high != EOF);
				assert(out[j] == (uint16_t)(low | high << 8));
			}
			i += count;
		}
		assert(state.next == state.end && !state.count);
	}
	assert(fgetc(expected) == EOF);
	fclose(expected);
	puts("RLE565: malformed inputs rejected; every animation pixel matches");
	return 0;
}
