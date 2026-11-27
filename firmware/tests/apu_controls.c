/* Local-buzzer control-only stepping must match the PCM control state.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "minigb_apu.h"

static void write_both(struct minigb_apu_ctx *pcm, struct minigb_apu_ctx *control,
                       uint16_t address, uint8_t value)
{
	minigb_apu_audio_write(pcm, address, value);
	minigb_apu_audio_write(control, address, value);
}

int main(void)
{
	struct minigb_apu_ctx pcm, control;
	audio_sample_t samples[AUDIO_SAMPLES_TOTAL];

	minigb_apu_audio_init(&pcm);
	control = pcm;
	write_both(&pcm, &control, 0xff26, 0x80);
	write_both(&pcm, &control, 0xff24, 0x77);
	write_both(&pcm, &control, 0xff25, 0xff);
	for (unsigned int frame = 0; frame < 1000; frame++) {
		if (frame % 17 == 0) {
			unsigned int frequency = 1400 + frame % 600;

			write_both(&pcm, &control, 0xff10, 0x12);
			write_both(&pcm, &control, 0xff11, 0x80 | (frame & 63));
			write_both(&pcm, &control, 0xff12, 0xa3);
			write_both(&pcm, &control, 0xff13, frequency);
			write_both(&pcm, &control, 0xff14, 0xc0 | (frequency >> 8));
			write_both(&pcm, &control, 0xff16, 0x40 | (frame & 63));
			write_both(&pcm, &control, 0xff17, 0x95);
			write_both(&pcm, &control, 0xff18, frequency);
			write_both(&pcm, &control, 0xff19, 0xc0 | (frequency >> 8));
			write_both(&pcm, &control, 0xff1a, 0x80);
			write_both(&pcm, &control, 0xff1b, frame);
			write_both(&pcm, &control, 0xff1c, (frame % 4) << 5);
			write_both(&pcm, &control, 0xff1d, frequency);
			write_both(&pcm, &control, 0xff1e, 0xc0 | (frequency >> 8));
			write_both(&pcm, &control, 0xff20, frame & 63);
			write_both(&pcm, &control, 0xff21, 0x73);
			write_both(&pcm, &control, 0xff22, ((frame % 16) << 4) | 2);
			write_both(&pcm, &control, 0xff23, 0xc0);
		}
		minigb_apu_audio_callback(&pcm, samples);
		minigb_apu_audio_callback(&control, NULL);
		assert(memcmp(pcm.audio_mem, control.audio_mem, sizeof(pcm.audio_mem)) == 0);
		assert(pcm.vol_l == control.vol_l && pcm.vol_r == control.vol_r);
		for (unsigned int i = 0; i < 4; i++) {
			const struct chan *a = &pcm.chans[i], *b = &control.chans[i];

			assert(a->enabled == b->enabled && a->powered == b->powered);
			assert(a->volume == b->volume && a->volume_init == b->volume_init);
			assert(a->freq == b->freq && a->freq_inc == b->freq_inc);
			assert(memcmp(&a->len, &b->len, sizeof(a->len)) == 0);
			assert(memcmp(&a->env, &b->env, sizeof(a->env)) == 0);
			assert(memcmp(&a->sweep, &b->sweep, sizeof(a->sweep)) == 0);
		}
	}
	puts("PASS: 1000 frames of PCM/control-only length, envelope, sweep and status equivalence");
	return 0;
}
