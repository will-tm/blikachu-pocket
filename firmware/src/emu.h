#ifndef BLIKACHU_EMU_H
#define BLIKACHU_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libretro/content.h"

struct emu_stats {
	uint32_t frame;
	uint32_t presented;
	uint32_t unchanged; /* frames already on the panel */
	uint32_t strips_written;
	uint32_t state_done;
	int state_result;
	uint32_t late_frames;
	uint32_t irq_off_frames;
	uint32_t render_skips; /* frames superseded before display */
	uint32_t core_skips;   /* frames intentionally not rendered by the core */
	uint32_t te_misses;    /* pushes not synced to v-blank (TE timeout) */
	uint32_t push_us;      /* last full-frame push duration */
	uint32_t push_us_max;
	uint32_t scale_us;     /* scale+HUD share of push_us */
	uint32_t te_period_us; /* measured panel refresh period */
	uint32_t emu_us;       /* last core frame including callbacks */
	uint32_t emu_us_max;
	uint64_t run_ms;
	uint32_t mfps; /* milli-fps: divide by 1000 for fps */
	int error;
	bool running;
	bool paused;
};

/* Start the emulator on a dedicated worker thread. Returns 0 once the
 * core is loaded, negative errno on init failure. The worker stops
 * when the frame budget (0 = unlimited) is hit, on quit hotkey, or on
 * `emu_quit()`.
 *
 * Gameplay is asynchronous. The caller may use emu_wait() to join it.
 */
int emu_run(const char *path, uint32_t frames);

/* Block until the emulator thread has fully exited and cleaned up. */
void emu_wait(void);

/* Toggle pause; idempotent. */
void emu_pause(void);
void emu_resume(void);

/* Ask the emulator to stop after the current frame. emu_run() returns
 * shortly after.
 */
void emu_quit(void);

/* Snapshot stats for the currently-running (or last-run) instance. */
void emu_get_stats(struct emu_stats *out);
void emu_get_resources(size_t *stack_used, size_t *stack_capacity, size_t *heap_peak, size_t *heap_capacity);

/* Inject a synthetic physical button press/release. The legacy shell
 * mask uses A=0x01 B=0x02 SEL=0x04 START=0x08
 * RIGHT=0x10 LEFT=0x20 UP=0x40 DOWN=0x80.
 */
void emu_button(uint8_t mask, bool pressed);
/* Inject an ABI joypad button ID directly, including SNES X/Y/L/R. */
void emu_retro_button(unsigned id, bool pressed);
enum emu_system emu_current_system(void);

/* Reset the loaded core at a frame boundary. */
void emu_reset(void);


/* Queue serialization/deserialization on the loaded core at a frame
 * boundary. NULL path uses the core/ROM-specific .lrstate filename.
 * Explicit paths must end in .lrstate. No core loaded returns -ENODEV.
 */
int emu_state_save(const char *path);
int emu_state_restore(const char *path);

/* Default core/ROM-specific state filename, NULL before a game loads.
 */
const char *emu_default_save_path(void);

/* Debug: force the buzzer silent regardless of APU state. */
void emu_buzzer_mute(bool mute);

/* Short UI feedback tone at the current volume setting. No-op when
 * muted. Works in-game too: briefly overrides the game's tone, which
 * resumes on its own.
 */
void emu_buzzer_blip(uint32_t freq_hz, uint32_t ms);

/* Buzzer volume 0..11, exponential: each step halves the PWM duty
 * (~-6 dB). 0 silent, 11 = 50% duty = hardware maximum. Higher volumes
 * increase coil current on the marginal power rail.
 */
void emu_buzzer_volume(int vol);
int emu_buzzer_volume_get(void);

/* Diagnostic: compare overlapped, frame-serial and DMA-isolated scheduling. */
enum emu_display_mode { EMU_DISPLAY_CONCURRENT, EMU_DISPLAY_SERIAL, EMU_DISPLAY_DMA, EMU_DISPLAY_HEADLESS };
void emu_display_mode_set(enum emu_display_mode mode);

/* Debug: print PCM sample rate, sample count and PWM status. */
void emu_apu_dump(void (*out)(void *ctx, const char *line), void *ctx);

/* Launch a ROM by name or absolute path (webui + shell shared core):
 * resolve to XIP (defragging if needed), cold-boot with the standard
 * save-state path. Quits a running game first.
 */
int emu_shell_launch(const char *spec);

/* Remote play audio: 10 kHz mono unsigned-8 tap of the PCM output.
 * Enable while a websocket client is connected; read drains the ring.
 */
void emu_audio_tap_enable(bool on);
size_t emu_audio_tap_read(uint8_t *dst, size_t max);

/* Remote play: copy the newest 240x216 fitted big-endian RGB565 frame.
 * -EAGAIN when no frame is available.
 */
int emu_frame_snapshot(uint16_t *rgb565);

#endif
