/* SPDX-License-Identifier: Apache-2.0 */
/* Single-active-core libretro gameplay frontend. */
#include "emu.h"
#include "gfx.h"
#include "hud.h"
#include "input.h"
#include "menu.h"
#include "libretro/environment.h"
#include "libretro/memory.h"
#include "libretro/session.h"
#include "libretro/state.h"
#include "libretro/video.h"
#include "psram_buf.h"
#include "storage.h"
#include <zephyr/fs/fs.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/display.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(emu, LOG_LEVEL_INF);

/* 32 rows fit in one 4095-word DMA transfer. */
#define STRIP_ROWS 32
#define BLOCK_COUNT DIV_ROUND_UP(LR_VIEW_H, STRIP_ROWS)
static BLIKACHU_BIGBUF struct lr_frame frame_slots[2];
static uint16_t strips[2][STRIP_ROWS][LR_VIEW_W]
	__aligned(32) __attribute__((section("WIFI_RAM")));
static K_MUTEX_DEFINE(frame_lock);
static K_SEM_DEFINE(render_kick, 0, 1);
static K_SEM_DEFINE(render_idle, 1, 1);
static K_SEM_DEFINE(frame_presented, 0, 1);
static K_SEM_DEFINE(scale_go, 0, 1);
static K_SEM_DEFINE(block_ready, 0, 2);
static K_SEM_DEFINE(block_free, 2, 2);
static struct k_thread render_thread, scale_thread;
static K_THREAD_STACK_DEFINE(render_stack, 4096);
static K_THREAD_STACK_DEFINE(scale_stack, 2048);
static bool render_started, pending, rendering, frame_valid;
static unsigned latest_slot, render_slot;
static unsigned panel_slot, dirty_mask, panel_hud, render_hud, full_strip_mask;
static bool panel_known;
static const struct device *display_dev;
static uint32_t scale_cycles;
static struct k_spinlock stats_lock;
static struct emu_stats stats;
static atomic_t presented, unchanged, strips_written, skips, te_misses;
static atomic_t display_mode = ATOMIC_INIT(EMU_DISPLAY_CONCURRENT);
unsigned int lr_dma_checkpoint_requested;
static atomic_t dma_benchmark;

/* At core event boundaries, give a pending LCD transfer a short opportunity
 * to service its interrupt. Never alter the core's emulated cycle count. */
void lr_dma_checkpoint(void)
{
	static uint32_t last_yield;
	if (!__atomic_load_n(&lr_dma_checkpoint_requested, __ATOMIC_RELAXED)) return;
	uint32_t now = k_cycle_get_32();
	if (k_cyc_to_us_floor32(now - last_yield) < 200) return;
	last_yield = now;
	k_sleep(K_USEC(20));
}
static atomic_t stop_requested, pause_requested, reset_requested, running;
static atomic_t menu_requested;
static atomic_t buttons, select_pulse;
static struct lr_session session;
static enum emu_system current_system;
static struct emu_content_info content;
static char rom_path[96], default_state[96], state_path[96];
static uint32_t rom_crc, frame_budget;
static int last_error, open_result;
static int64_t start_ticks, end_ticks;
static struct k_thread worker;
static K_THREAD_STACK_DEFINE(worker_stack, 12288);
static K_MUTEX_DEFINE(launch_lock);
static K_MUTEX_DEFINE(state_lock);
static K_SEM_DEFINE(open_done, 0, 1);
static K_SEM_DEFINE(frame_tick, 0, 1);
static atomic_t state_op;
static bool worker_created;

static void scale_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&scale_go, K_FOREVER);
		unsigned mask = dirty_mask;
		unsigned slot = 0;
		while (mask) {
			unsigned i = __builtin_ctz(mask);
			mask &= mask - 1;
			unsigned y = i * STRIP_ROWS;
			unsigned rows = MIN(STRIP_ROWS, LR_VIEW_H - y);
			k_sem_take(&block_free, K_FOREVER);
			uint32_t start = k_cycle_get_32();
			uint16_t *dst = &strips[slot][0][0];
			lr_video_strip(&frame_slots[render_slot], y, rows, dst);
			if (hud_overlay(y + LCD_VIEW_Y, rows, dst, LR_VIEW_W)) {
				render_hud |= BIT(i);
			}
			scale_cycles += k_cycle_get_32() - start;
			slot ^= 1;
			k_sem_give(&block_ready);
		}
	}
}

static void render_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&render_kick, K_FOREVER);
		k_mutex_lock(&frame_lock, K_FOREVER);
		if (!pending) {
			k_mutex_unlock(&frame_lock);
			continue;
		}
		pending = false;
		rendering = true;
		render_slot = latest_slot;
		/* The previously displayed slot is immutable until this lock is
		 * released. Work out damage now; the core may then reuse it. */
		dirty_mask = 0;
		render_hud = 0;
		full_strip_mask = 0;
		const struct lr_frame *new_frame = &frame_slots[render_slot];
		const struct lr_frame *old_frame = &frame_slots[panel_slot];
		/* The black letterbox rows are unchanged when the viewport is
		 * unchanged. Clip partial strips unless they carry a HUD. */
		bool same_view = panel_known && new_frame->view_y == old_frame->view_y &&
			new_frame->view_h == old_frame->view_h &&
			new_frame->view_x == old_frame->view_x && new_frame->view_w == old_frame->view_w;
		for (unsigned i = 0; i < BLOCK_COUNT; ++i) {
			unsigned y = i * STRIP_ROWS;
			unsigned rows = MIN(STRIP_ROWS, LR_VIEW_H - y);
			bool hud = (panel_hud & BIT(i)) || hud_overlaps(y + LCD_VIEW_Y, rows);
			if (!same_view || hud) full_strip_mask |= BIT(i);
			else if (y + rows <= new_frame->view_y ||
				 y >= new_frame->view_y + new_frame->view_h) continue;
			if (!panel_known || hud || !lr_video_strip_equal(new_frame, old_frame, y, rows)) {
				dirty_mask |= BIT(i);
			}
		}
		k_sem_take(&render_idle, K_NO_WAIT);
		k_mutex_unlock(&frame_lock);
		if (!dirty_mask) {
			atomic_inc(&unchanged);
			goto complete;
		}
		__atomic_store_n(&lr_dma_checkpoint_requested,
				 atomic_get(&display_mode) == EMU_DISPLAY_CONCURRENT, __ATOMIC_RELAXED);
		scale_cycles = 0;
		k_sem_give(&scale_go);
		k_sem_take(&block_ready, K_FOREVER);
		if (!gfx_te_sync(3000, 25)) {
			atomic_inc(&te_misses);
		}
		/* Let the core use the TE wait, but keep its flash/PSRAM traffic
		 * off the bus while the SPI DMA drains the uncached WRAM strips. */
		bool isolate = atomic_get(&display_mode) == EMU_DISPLAY_DMA;
		if (isolate) k_thread_suspend(&worker);
		uint32_t start = k_cycle_get_32();
		int result = 0;
		unsigned mask = dirty_mask;
		unsigned slot = 0;
		bool first_block = true;
		while (mask) {
			unsigned i = __builtin_ctz(mask);
			mask &= mask - 1;
			if (!first_block) {
				k_sem_take(&block_ready, K_FOREVER);
			}
			first_block = false;
			unsigned y = i * STRIP_ROWS;
			unsigned first = y, last = MIN(y + STRIP_ROWS, LR_VIEW_H);
			if (!(full_strip_mask & BIT(i))) {
				first = MAX(first, new_frame->view_y);
				last = MIN(last, new_frame->view_y + new_frame->view_h);
			}
			struct display_buffer_descriptor desc = {
				.width = LR_VIEW_W, .height = last - first, .pitch = LR_VIEW_W,
				.buf_size = (last - first) * LR_VIEW_W * sizeof(uint16_t),
			};
			int rc = display_write(display_dev, 0, LCD_VIEW_Y + first,
					       &desc, strips[slot] + (first - y));
			if (rc) {
				result = rc;
			} else {
				atomic_inc(&strips_written);
			}
			slot ^= 1;
			k_sem_give(&block_free);
		}
		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
		if (isolate) k_thread_resume(&worker);
		__atomic_store_n(&lr_dma_checkpoint_requested, 0, __ATOMIC_RELAXED);
		k_spinlock_key_t key = k_spin_lock(&stats_lock);
		stats.push_us = us;
		stats.push_us_max = MAX(stats.push_us_max, us);
		stats.scale_us = k_cyc_to_us_floor32(scale_cycles);
		k_spin_unlock(&stats_lock, key);
		if (!result) {
			atomic_inc(&presented);
		} else {
			LOG_ERR("LCD frame write: %d", result);
		}
		panel_known = !result;
complete:
		k_mutex_lock(&frame_lock, K_FOREVER);
		panel_slot = render_slot;
		panel_hud = render_hud;
		rendering = false;
		k_sem_give(&render_idle);
		k_sem_give(&frame_presented);
		k_mutex_unlock(&frame_lock);
	}
}

static void video(const void *pixels, unsigned width, unsigned height, size_t pitch)
{
	if (!pixels || atomic_get(&display_mode) == EMU_DISPLAY_HEADLESS) {
		return; /* duplicate: panel already contains the last frame */
	}
	k_mutex_lock(&frame_lock, K_FOREVER);
	unsigned slot = rendering ? (render_slot ^ 1) : (pending ? latest_slot : (latest_slot ^ 1));
	const struct retro_game_geometry *geometry = &lr_environment_av_info()->geometry;
	float aspect = geometry->base_width ? geometry->aspect_ratio : session.av.geometry.aspect_ratio;
	if (!lr_video_store(&frame_slots[slot], pixels, width, height, pitch,
			    lr_environment_pixel_format(), aspect)) {
		if (pending) {
			atomic_inc(&skips);
		}
		latest_slot = slot;
		frame_valid = pending = true;
		if (atomic_get(&display_mode) == EMU_DISPLAY_SERIAL) k_sem_reset(&frame_presented);
		k_sem_give(&render_kick);
	} else {
		last_error = -EINVAL;
		atomic_set(&stop_requested, 1);
	}
	k_mutex_unlock(&frame_lock);
	if (atomic_get(&display_mode) == EMU_DISPLAY_SERIAL && !atomic_get(&stop_requested)) {
		k_sem_take(&frame_presented, K_FOREVER);
	}
}

void emu_display_mode_set(enum emu_display_mode mode) { atomic_set(&display_mode, mode); }

int emu_frame_snapshot(uint16_t *dst)
{
	k_mutex_lock(&frame_lock, K_FOREVER);
	int rc = -EAGAIN;
	if (atomic_get(&running) && frame_valid) {
		lr_video_strip(&frame_slots[latest_slot], 0, LR_VIEW_H, dst);
		rc = 0;
	}
	k_mutex_unlock(&frame_lock);
	return rc;
}

/* ---- Input ---- */
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port || device != RETRO_DEVICE_JOYPAD || index) {
		return 0;
	}
	uint16_t value = (uint16_t)atomic_get(&buttons);
	if (atomic_get(&select_pulse)) {
		value |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
	}
	return id == RETRO_DEVICE_ID_JOYPAD_MASK ? (int16_t)value : (id < 16 && (value & BIT(id)) != 0);
}
static void input_poll(void) {}

enum emu_system emu_current_system(void) { return atomic_get(&running) ? current_system : EMU_SYSTEM_UNKNOWN; }

void emu_retro_button(unsigned id, bool pressed)
{
	if (id >= 16) {
		return;
	}
	if (pressed) {
		atomic_or(&buttons, BIT(id));
		if (id == RETRO_DEVICE_ID_JOYPAD_SELECT) {
			atomic_set(&select_pulse, 1);
		}
	} else {
		atomic_and(&buttons, ~BIT(id));
	}
	if ((atomic_get(&buttons) & (BIT(RETRO_DEVICE_ID_JOYPAD_SELECT) | BIT(RETRO_DEVICE_ID_JOYPAD_START))) ==
	    (BIT(RETRO_DEVICE_ID_JOYPAD_SELECT) | BIT(RETRO_DEVICE_ID_JOYPAD_START))) {
		atomic_set(&menu_requested, 1);
		emu_quit();
	}
}

void emu_button(uint8_t mask, bool pressed)
{
	const unsigned ids[] = {RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN};
	for (unsigned i = 0; i < ARRAY_SIZE(ids); ++i) {
		if (mask & BIT(i)) {
			unsigned id = ids[i];
			if (current_system == EMU_SYSTEM_SNES && i < 2) {
				id = i == 0 ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_Y;
			}
			emu_retro_button(id, pressed);
		}
	}
}
static void input_cb(struct input_event *evt)
{
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	const uint16_t codes[] = {INPUT_BTN_A, INPUT_BTN_B, INPUT_BTN_SELECT, INPUT_BTN_START,
		INPUT_KEY_RIGHT, INPUT_KEY_LEFT, INPUT_KEY_UP, INPUT_KEY_DOWN};
	for (unsigned i = 0; i < ARRAY_SIZE(codes); ++i) {
		if (evt->code == codes[i]) {
			emu_button(BIT(i), evt->value != 0);
			return;
		}
	}
}

/* ---- Session ---- */
extern void lr_audio_start(unsigned sample_rate);
extern void lr_audio_stop(void);
extern void lr_audio_sample(int16_t left, int16_t right);
extern size_t lr_audio_batch(const int16_t *data, size_t frames);
extern void lr_audio_pause(bool paused);

/* Absolute deadlines with Q16 fractional ticks avoid accumulating a
 * rounded tick error at NTSC/PAL frame rates. Keep only one pending tick
 * when a core cannot finish in time. */
static int64_t frame_deadline;
static uint64_t frame_period_fp;
static uint32_t frame_fraction;

static void tick(struct k_timer *timer)
{
	uint64_t step = frame_period_fp + frame_fraction;
	frame_deadline += step >> 16;
	frame_fraction = step & 0xffff;
	k_timer_start(timer, K_TIMEOUT_ABS_TICKS(frame_deadline), K_NO_WAIT);
	k_sem_give(&frame_tick);
}
static K_TIMER_DEFINE(frame_timer, tick, NULL);

static void worker_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	const struct lr_callbacks callbacks = {video, lr_audio_sample, lr_audio_batch, input_poll, input_state};
	struct fs_file_t pin;
	const char *error = NULL;
	size_t rom_size;
	storage_guard_lock();
	const uint8_t *rom = storage_pin_rom(rom_path, &rom_size, &error, &pin);
	if (!rom && error == STORAGE_ERR_FRAGMENTED) {
		hud_show_text("DEFRAGMENTING...");
		if (!storage_defrag_file(rom_path)) rom = storage_pin_rom(rom_path, &rom_size, &error, &pin);
	}
	int rc = rom ? emu_content_identify(rom_path, rom, rom_size, &content) : -EIO;
	if (!rc) {
		current_system = content.system;
		rc = lr_session_open(&session, &content, rom_path, &callbacks);
	}
	if (rc && rom) fs_close(&pin);
	storage_guard_unlock();
	open_result = rc;
	if (rc) {
		last_error = rc;
		atomic_set(&running, 0);
		k_sem_give(&open_done);
		return;
	}
	rom_crc = crc32_ieee(content.data, content.size);
	lr_state_path(default_state, sizeof(default_state), &session, rom_crc, "lrstate");
	int battery_rc = lr_battery_file(&session, rom_crc, true);
	if (battery_rc) {
		LOG_WRN("battery load: %d", battery_rc);
	}
	lr_audio_start((unsigned)session.av.timing.sample_rate);
	input_set_handler(input_cb);
	frame_period_fp = (uint64_t)(65536.0 * CONFIG_SYS_CLOCK_TICKS_PER_SEC /
				     session.av.timing.fps + 0.5);
	frame_fraction = 32768; /* round deadlines to the closest tick */
	start_ticks = k_uptime_ticks();
	frame_deadline = start_ticks + 1;
	k_timer_start(&frame_timer, K_TIMEOUT_ABS_TICKS(frame_deadline), K_NO_WAIT);
	k_sem_give(&open_done);
	bool skip_video = false;
	while (!atomic_get(&stop_requested)) {
		k_sem_take(&frame_tick, K_FOREVER);
		if (atomic_get(&stop_requested)) {
			break;
		}
		if (atomic_cas(&reset_requested, 1, 0)) {
			rc = lr_session_reset(&session);
			skip_video = false;
			if (rc) break;
		}
		k_mutex_lock(&state_lock, K_FOREVER);
		int op = atomic_set(&state_op, 0);
		if (op) {
			int state_rc = lr_state_file(&session, state_path, rom_crc, op == 2);
			k_spinlock_key_t key = k_spin_lock(&stats_lock);
			stats.state_result = state_rc;
			stats.state_done++;
			k_spin_unlock(&stats_lock, key);
			LOG_INF("state %s: %d", op == 1 ? "save" : "restore", state_rc);
			hud_show_text(state_rc ? "STATE ERROR" : (op == 1 ? "STATE SAVED" : "STATE LOADED"));
		}
		k_mutex_unlock(&state_lock);
		if (atomic_get(&pause_requested)) {
			skip_video = false;
			continue;
		}
		uint32_t start = k_cycle_get_32();
		uint32_t before_irq, after_irq;
		__asm__ volatile("csrr %0, mstatus" : "=r"(before_irq));
		lr_environment_video_enable(!skip_video);
		rc = lr_session_run(&session);
		__asm__ volatile("csrr %0, mstatus" : "=r"(after_irq));
		atomic_set(&select_pulse, 0);
		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
		bool late = k_sem_count_get(&frame_tick) > 0;
		k_spinlock_key_t key = k_spin_lock(&stats_lock);
		stats.frame++;
		stats.irq_off_frames += !(before_irq & after_irq & BIT(3));
		stats.core_skips += skip_video;
		stats.late_frames += late;
		stats.emu_us = us;
		stats.emu_us_max = MAX(stats.emu_us_max, us);
		bool finished = frame_budget && stats.frame >= frame_budget;
		k_spin_unlock(&stats_lock, key);
		/* Snes9x can omit rendering without omitting CPU/APU execution.
		 * A late frame may skip the following render to regain cadence. */
		int mode = atomic_get(&display_mode);
		skip_video = current_system == EMU_SYSTEM_SNES &&
			(mode == EMU_DISPLAY_DMA || mode == EMU_DISPLAY_CONCURRENT) &&
			late && !skip_video;
		if (finished) atomic_set(&menu_requested, 1);
		if (rc || finished) break;
	}
	k_timer_stop(&frame_timer);
	lr_audio_stop();
	input_set_handler(NULL);
	k_mutex_lock(&frame_lock, K_FOREVER);
	pending = frame_valid = false;
	k_mutex_unlock(&frame_lock);
	k_sem_take(&render_idle, K_FOREVER);
	k_sem_give(&render_idle);
	if (!rc && !battery_rc && !atomic_get(&dma_benchmark)) {
		int save_rc = lr_battery_file(&session, rom_crc, false);
		if (save_rc) LOG_WRN("battery save: %d", save_rc);
	}
	storage_guard_lock();
	int close_rc = lr_session_close(&session);
	fs_close(&pin);
	storage_guard_unlock();
	if (!rc) rc = close_rc;
	if (rc) last_error = rc;
	end_ticks = k_uptime_ticks();
	atomic_set(&running, 0);
	if (atomic_get(&menu_requested)) menu_refresh();
}

int emu_run(const char *path, uint32_t frames)
{
	k_mutex_lock(&launch_lock, K_FOREVER);
	emu_quit();
	emu_wait();
	int rc = 0;
	if (strlen(path) >= sizeof(rom_path)) { rc = -ENAMETOOLONG; goto out; }
	strcpy(rom_path, path);
	current_system = EMU_SYSTEM_UNKNOWN;
	frame_budget = frames;
	last_error = 0;
	start_ticks = end_ticks = 0;
	memset(&stats, 0, sizeof(stats));
	atomic_set(&presented, 0); atomic_set(&unchanged, 0); atomic_set(&strips_written, 0);
	atomic_set(&skips, 0); atomic_set(&te_misses, 0);
	panel_known = false;
	panel_hud = 0;
	atomic_set(&stop_requested, 0); atomic_set(&pause_requested, 0); atomic_set(&reset_requested, 0);
	atomic_set(&menu_requested, 0);
	atomic_set(&buttons, 0); atomic_set(&select_pulse, 0); atomic_set(&state_op, 0);
	default_state[0] = 0;
	k_sem_reset(&frame_tick);
	if (!render_started) {
		display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
		if (!device_is_ready(display_dev)) { rc = -ENODEV; goto out; }
		k_thread_create(&render_thread, render_stack, K_THREAD_STACK_SIZEOF(render_stack),
			render_fn, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
		k_thread_name_set(&render_thread, "lcd-push");
		k_thread_create(&scale_thread, scale_stack, K_THREAD_STACK_SIZEOF(scale_stack),
			scale_fn, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
		k_thread_name_set(&scale_thread, "lcd-scale");
		render_started = true;
	}
	atomic_set(&running, 1);
	worker_created = true;
	k_sem_reset(&open_done);
	k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
		worker_fn, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
	k_thread_name_set(&worker, "emu");
	k_sem_take(&open_done, K_FOREVER);
	rc = open_result;
out:
	k_mutex_unlock(&launch_lock);
	return rc;
}

void emu_wait(void) { if (worker_created) k_thread_join(&worker, K_FOREVER); }
void emu_quit(void) { atomic_set(&stop_requested, 1); k_sem_give(&frame_tick); }
void emu_pause(void) { atomic_set(&pause_requested, 1); lr_audio_pause(true); }
void emu_resume(void) { atomic_set(&pause_requested, 0); lr_audio_pause(false); }
void emu_reset(void) { atomic_set(&reset_requested, 1); }

void emu_get_stats(struct emu_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);
	*out = stats;
	out->running = atomic_get(&running);
	out->paused = atomic_get(&pause_requested);
	out->error = last_error;
	out->presented = atomic_get(&presented);
	out->unchanged = atomic_get(&unchanged);
	out->strips_written = atomic_get(&strips_written);
	out->render_skips = atomic_get(&skips);
	out->te_misses = atomic_get(&te_misses);
	out->te_period_us = gfx_te_period_us();
	int64_t end = out->running ? k_uptime_ticks() : end_ticks;
	out->run_ms = start_ticks ? k_ticks_to_ms_floor64(end - start_ticks) : 0;
	out->mfps = out->run_ms ? (uint64_t)out->frame * 1000000 / out->run_ms : 0;
	k_spin_unlock(&stats_lock, key);
}

void emu_get_resources(size_t *stack_used, size_t *stack_capacity, size_t *heap_peak, size_t *heap_capacity)
{
	*stack_capacity = K_THREAD_STACK_SIZEOF(worker_stack);
	*stack_used = 0;
	size_t unused;
	if (worker_created && k_thread_stack_space_get(&worker, &unused) == 0) {
		*stack_used = *stack_capacity - unused;
	}
	size_t used;
	lr_memory_stats(heap_capacity, &used, heap_peak);
}

const char *emu_default_save_path(void) { return default_state[0] ? default_state : NULL; }

static int queue_state(const char *path, int op)
{
	if (!atomic_get(&running) || !default_state[0]) return -ENODEV;
	if (!path) path = default_state;
	if (strlen(path) >= sizeof(state_path)) return -ENAMETOOLONG;
	const char *dot = strrchr(path, '.');
	if (!dot || strcmp(dot, ".lrstate")) return -EINVAL;
	if (k_mutex_lock(&state_lock, K_NO_WAIT)) return -EBUSY;
	int rc = 0;
	if (atomic_get(&state_op)) rc = -EBUSY;
	else { strcpy(state_path, path); atomic_set(&state_op, op); }
	k_mutex_unlock(&state_lock);
	return rc;
}
int emu_state_save(const char *path) { return queue_state(path, 1); }
int emu_state_restore(const char *path) { return queue_state(path, 2); }

/* Measure DMA without touching ROM or save data. */
extern void spi_bflb_dma_probe_begin(unsigned burst);
extern void spi_bflb_dma_probe_end(uint32_t out[5]);

static int cmd_lcdbench(const struct shell *sh, size_t argc, char **argv)
{
	unsigned burst = argc == 2 ? strtoul(argv[1], NULL, 10) : 4;
	if (burst != 1 && burst != 4) return -EINVAL;
	k_mutex_lock(&launch_lock, K_FOREVER);
	if (atomic_get(&running)) {
		k_mutex_unlock(&launch_lock);
		return -EBUSY;
	}
	emu_wait();
	const struct device *lcd = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(lcd)) {
		k_mutex_unlock(&launch_lock);
		return -ENODEV;
	}
	memset(strips[0], 0x5a, sizeof(strips[0]));
	uint32_t total = 0, maximum = 0, minimum = UINT32_MAX;
	int rc = 0;
	spi_bflb_dma_probe_begin(burst);
	unsigned count = 0;
	for (unsigned frame = 0; frame < 8 && !rc; ++frame) {
		for (unsigned i = 0; i < BLOCK_COUNT && !rc; ++i) {
			unsigned rows = MIN(STRIP_ROWS, LR_VIEW_H - i * STRIP_ROWS);
			const struct display_buffer_descriptor desc = {
				.width = LR_VIEW_W, .height = rows, .pitch = LR_VIEW_W,
				.buf_size = rows * LR_VIEW_W * sizeof(uint16_t),
			};
			uint32_t t0 = k_cycle_get_32();
			rc = display_write(lcd, 0, LCD_VIEW_Y + i * STRIP_ROWS, &desc, strips[0]);
			uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
			total += us;
			maximum = MAX(maximum, us);
			minimum = MIN(minimum, us);
			++count;
		}
	}
	uint32_t dma_stats[5];
	spi_bflb_dma_probe_end(dma_stats);
	k_mutex_unlock(&launch_lock);
	shell_print(sh, "lcdbench burst=%u: %u strips rc=%d total=%u us min=%u mean=%u max=%u",
		burst, count, rc, total, minimum, total / MAX(count, 1U), maximum);
	shell_print(sh, "dma late=%u pending=%u max_pending=%u irq_max=%u us wake_max=%u us",
		dma_stats[0], dma_stats[1], dma_stats[2], dma_stats[3], dma_stats[4]);
	menu_refresh();
	return rc;
}
SHELL_CMD_ARG_REGISTER(lcdbench, NULL, "Idle LCD DMA timing [burst 1|4]", cmd_lcdbench, 1, 1);

static int cmd_lrdmaprobe(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (atomic_get(&running)) return -EBUSY;
	if (!strcmp(argv[1], "end")) {
		uint32_t stats[5];
		spi_bflb_dma_probe_end(stats);
		atomic_set(&dma_benchmark, 0);
		shell_print(sh, "dma late=%u pending=%u max_pending=%u irq_max=%u us wake_max=%u us",
			stats[0], stats[1], stats[2], stats[3], stats[4]);
		return 0;
	}
	unsigned burst = strtoul(argv[1], NULL, 10);
	if (burst != 1 && burst != 4) return -EINVAL;
	spi_bflb_dma_probe_begin(burst);
	atomic_set(&dma_benchmark, 1);
	shell_print(sh, "DMA profile burst=%u (no battery writes)", burst);
	return 0;
}
SHELL_CMD_ARG_REGISTER(lrdmaprobe, NULL, "DMA benchmark 1/4 before gameplay (no battery writes), end after stop", cmd_lrdmaprobe, 2, 0);
