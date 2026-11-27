/*
 * RAM-resident 64-bit division helpers.
 *
 * The BFLB flash driver's erase/write path runs from RAM with XIP
 * suspended and IRQs locked; its settle-delay math divides 64-bit
 * clock frequencies, which gcc lowers to __udivdi3/__umoddi3 calls.
 * libgcc lives in flash, so the first in-window call fetches from the
 * suspended XIP window and hard-wedges the chip (no fault, IRQs off).
 *
 * These strong definitions shadow libgcc's copies and carry a
 * __ramfunc placement. Plain 1-bit shift-subtract: slow but branch-only,
 * and the in-window callers only divide small clock numbers. Shifts are
 * by constants only — a variable 64-bit shift lowers to libgcc's
 * __ashldi3/__lshrdi3, which live in flash too.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <zephyr/linker/sections.h>
#include <zephyr/toolchain.h>

static __ramfunc uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
	uint64_t q = 0, r = 0;

	if (d != 0) {
		for (int i = 0; i < 64; i++) {
			r = (r << 1) | (n >> 63);
			n <<= 1;
			q <<= 1;
			if (r >= d) {
				r -= d;
				q |= 1;
			}
		}
	}
	if (rem) {
		*rem = r;
	}
	return q;
}

__ramfunc uint64_t __udivdi3(uint64_t n, uint64_t d)
{
	return udivmod64(n, d, NULL);
}

__ramfunc uint64_t __umoddi3(uint64_t n, uint64_t d)
{
	uint64_t r;

	udivmod64(n, d, &r);
	return r;
}

__ramfunc int64_t __divdi3(int64_t n, int64_t d)
{
	int neg = (n < 0) ^ (d < 0);
	uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : n, d < 0 ? -(uint64_t)d : d, NULL);

	return neg ? -(int64_t)q : (int64_t)q;
}

__ramfunc int64_t __moddi3(int64_t n, int64_t d)
{
	uint64_t r;

	udivmod64(n < 0 ? -(uint64_t)n : n, d < 0 ? -(uint64_t)d : d, &r);
	return n < 0 ? -(int64_t)r : (int64_t)r;
}
