/* SPDX-License-Identifier: Apache-2.0 */
/* PCM game audio and independent UI tones on the BL618 buzzer. */
#include "../emu.h"
#include "psram_buf.h"
#include <bouffalolab/common/pwm_v2_reg.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <stdio.h>

#define OUTPUT_RATE 10000U
#define CAPACITY 2048U
#define TAP_CAPACITY 8192U
#define PWM_BASE DT_REG_ADDR(DT_PWMS_CTLR(DT_ALIAS(buzzer)))
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_ALIAS(buzzer));
static BLIKACHU_BIGBUF uint8_t pcm[CAPACITY], tap[TAP_CAPACITY];
static atomic_t pcm_read, pcm_write, tap_read, tap_write;
static atomic_t volume = ATOMIC_INIT(11), muted, active, output_enabled, paused, tap_enabled, gated;
static atomic_t tone_hz, tone_until;
static unsigned source_rate, phase, pwm_period, tone_phase;
static int pwm_rc;
static uint32_t samples, underruns;

static void duty(unsigned value)
{
	sys_write32(value << PWM_CH0_THREH_SHIFT,
		    PWM_BASE + PWM_MC0_CH0_THRE_OFFSET + buzzer.channel * 4);
}

static void audio_tick(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	uint32_t rd = atomic_get(&pcm_read);
	int sample = 128;
	bool available = rd != (uint32_t)atomic_get(&pcm_write);
	if (available) {
		sample = pcm[rd % CAPACITY];
		atomic_set(&pcm_read, rd + 1);
	} else {
		underruns++;
	}
	int vol = atomic_get(&volume);
	bool blip = (int32_t)(atomic_get(&tone_until) - k_uptime_get_32()) > 0;
	if (blip) {
		tone_phase += atomic_get(&tone_hz);
		if (tone_phase >= OUTPUT_RATE) tone_phase %= OUTPUT_RATE;
		sample = tone_phase < OUTPUT_RATE / 2 ? 224 : 32;
	}
	if (!vol || atomic_get(&muted) || atomic_get(&gated) ||
	    (!blip && (!available || atomic_get(&paused)))) {
		duty(0);
	} else {
		unsigned max_duty = pwm_period >> (12 - vol);
		duty((unsigned)sample * max_duty / 255);
	}
}
static K_TIMER_DEFINE(pcm_timer, audio_tick, NULL);

static void menu_tone(unsigned hz)
{
	if (!device_is_ready(buzzer.dev) || atomic_get(&gated)) return;
	int vol = atomic_get(&volume);
	if (!hz || !vol || atomic_get(&muted)) {
		pwm_rc = pwm_set_dt(&buzzer, 1000000, 0);
	} else {
		uint32_t period = 1000000000U / hz;
		pwm_rc = pwm_set_dt(&buzzer, period, period >> (12 - vol));
	}
}
static void blip_off(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_get(&active)) menu_tone(0);
}
static K_WORK_DELAYABLE_DEFINE(blip_work, blip_off);

static void update_output(void)
{
	bool enable = atomic_get(&active) && atomic_get(&volume) && !atomic_get(&muted) && !atomic_get(&gated);
	if (!enable) {
		if (atomic_cas(&output_enabled, 1, 0)) {
			k_timer_stop(&pcm_timer);
			duty(0);
		}
		return;
	}
	if (atomic_get(&output_enabled)) {
		return;
	}
	uint64_t clock;
	pwm_rc = pwm_get_cycles_per_sec(buzzer.dev, buzzer.channel, &clock);
	if (!pwm_rc) {
		pwm_period = (uint32_t)(clock / 50000);
		pwm_rc = pwm_set_cycles(buzzer.dev, buzzer.channel, pwm_period, 0, buzzer.flags);
	}
	if (!pwm_rc) {
		atomic_set(&pcm_read, atomic_get(&pcm_write));
		atomic_set(&output_enabled, 1);
		k_timer_start(&pcm_timer, K_USEC(100), K_USEC(100));
	}
}
void lr_audio_start(unsigned rate)
{
	source_rate = rate;
	phase = tone_phase = samples = underruns = 0;
	atomic_set(&pcm_read, 0); atomic_set(&pcm_write, 0);
	atomic_set(&paused, 0); atomic_set(&tone_until, 0);
	(void)k_work_cancel_delayable(&blip_work);
	atomic_set(&active, 1);
	update_output();
}
void lr_audio_stop(void)
{
	atomic_set(&active, 0);
	update_output();
	source_rate = 0;
	menu_tone(0);
}
void lr_audio_pause(bool pause)
{
	atomic_set(&paused, pause);
	if (pause) atomic_set(&pcm_read, atomic_get(&pcm_write));
}
void lr_audio_sample(int16_t left, int16_t right)
{
	if (!source_rate) return;
	phase += OUTPUT_RATE;
	if (phase < source_rate) return;
	phase -= source_rate;
	uint8_t sample = (uint8_t)((((int32_t)left + right) / 2 >> 8) + 128);
	samples++;
	uint32_t wr = atomic_get(&pcm_write);
	if (atomic_get(&output_enabled) && wr - (uint32_t)atomic_get(&pcm_read) < CAPACITY) {
		pcm[wr % CAPACITY] = sample;
		atomic_set(&pcm_write, wr + 1);
	}
	if (atomic_get(&tap_enabled)) {
		wr = atomic_get(&tap_write);
		if (wr - (uint32_t)atomic_get(&tap_read) < TAP_CAPACITY) {
			tap[wr % TAP_CAPACITY] = sample;
			atomic_set(&tap_write, wr + 1);
		}
	}
}
size_t lr_audio_batch(const int16_t *data, size_t frames)
{
	for (size_t i = 0; i < frames; ++i) lr_audio_sample(data[i * 2], data[i * 2 + 1]);
	return frames;
}
void emu_audio_tap_enable(bool on)
{
	if (on) atomic_set(&tap_read, atomic_get(&tap_write));
	atomic_set(&tap_enabled, on);
}
size_t emu_audio_tap_read(uint8_t *dst, size_t max)
{
	uint32_t rd = atomic_get(&tap_read);
	size_t n = 0;
	while (n < max && rd != (uint32_t)atomic_get(&tap_write)) dst[n++] = tap[rd++ % TAP_CAPACITY];
	atomic_set(&tap_read, rd);
	return n;
}
void emu_buzzer_volume(int vol)
{
	atomic_set(&volume, CLAMP(vol, 0, 11));
	update_output();
	if (!vol && !atomic_get(&active)) menu_tone(0);
}
int emu_buzzer_volume_get(void) { return atomic_get(&volume); }
void emu_buzzer_mute(bool mute)
{
	atomic_set(&muted, mute);
	update_output();
	if (mute && !atomic_get(&active)) menu_tone(0);
}
void emu_buzzer_pcm_gate(bool on) { atomic_set(&gated, on); update_output(); }
void emu_buzzer_blip(uint32_t hz, uint32_t ms)
{
	if (atomic_get(&muted) || !atomic_get(&volume)) return;
	if (atomic_get(&active)) {
		atomic_set(&tone_hz, hz);
		atomic_set(&tone_until, k_uptime_get_32() + ms);
	} else {
		menu_tone(hz);
		k_work_reschedule(&blip_work, K_MSEC(ms));
	}
}
void emu_apu_dump(void (*out)(void *, const char *), void *ctx)
{
	char line[112];
	snprintf(line, sizeof(line), "PCM %u Hz -> %u Hz, samples=%u underruns=%u PWM rc=%d",
		 source_rate, OUTPUT_RATE, samples, underruns, pwm_rc);
	out(ctx, line);
}
