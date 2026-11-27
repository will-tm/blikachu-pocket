/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded, headless hardware diagnostics. This is not the gameplay renderer. */
#include "content.h"
#include "session.h"
#include "vfs.h"
#include "environment.h"
#include "memory.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>
extern "C" {
#include "../emu.h"
#include "../storage.h"
}

static struct {
	unsigned video;
	unsigned samples;
	unsigned frame;
	unsigned limit;
	unsigned width;
	unsigned height;
	uint32_t crc;
	bool invalid;
} probe;

static void video(const void *pixels, unsigned width, unsigned height, size_t pitch)
{
	if (!pixels) {
		return;
	}
	if (!width || !height || width > 512 || height > 478 || pitch < width * 2U) {
		probe.invalid = true;
		return;
	}
	probe.video++;
	probe.width = width;
	probe.height = height;
	if (probe.frame == 0 || probe.frame + 1 == probe.limit) {
		uint32_t crc = 0;

		for (unsigned row = 0; row < height; ++row) {
			crc = crc32_ieee_update(crc, static_cast<const uint8_t *>(pixels) + row * pitch,
					       width * 2U);
		}
		probe.crc = crc;
	}
}
static void audio_sample(int16_t, int16_t)
{
	probe.samples++;
}
static size_t audio_batch(const int16_t *, size_t frames)
{
	probe.samples += frames;
	return frames;
}
static void input_poll(void) {}
static int16_t input_state(unsigned, unsigned, unsigned, unsigned) { return 0; }

static bool allocator_check()
{
	size_t capacity, used, peak;
	lr_memory_stats(&capacity, &used, &peak);
	uint8_t *p = static_cast<uint8_t *>(lr_calloc(1, 256));

	if (!p) {
		return false;
	}
	for (unsigned i = 0; i < 256; ++i) {
		if (p[i]) {
			lr_free(p);
			return false;
		}
		p[i] = static_cast<uint8_t>(i);
	}
	uint8_t *q = static_cast<uint8_t *>(lr_realloc(p, 512));

	if (!q) {
		lr_free(p);
		return false;
	}
	for (unsigned i = 0; i < 256; ++i) {
		if (q[i] != static_cast<uint8_t>(i)) {
			lr_free(q);
			return false;
		}
	}
	lr_free(q);
	void *aligned = lr_aligned_alloc(64, 256);
	bool alignment_ok = aligned && !(reinterpret_cast<uintptr_t>(aligned) & 63);

	lr_free(aligned);
	void *oversized = lr_malloc(capacity + 1);
	void *overflow = lr_calloc(SIZE_MAX, 2);
	bool bounded = !oversized && !overflow;

	lr_free(oversized);
	lr_free(overflow);
	bool caught = false;
	try {
		void *allocation = ::operator new(capacity + 1);
		::operator delete(allocation);
	} catch (const std::bad_alloc &) {
		caught = true;
	}
	return alignment_ok && bounded && caught;
}

static bool vfs_check(const char *path, const uint8_t *data, size_t size)
{
	const struct retro_vfs_interface *vfs = lr_vfs_interface();
	retro_vfs_file_handle *file = vfs->open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
	if (!file) {
		return false;
	}
	uint8_t bytes[16];
	bool ok = size >= sizeof(bytes) && vfs->size(file) == static_cast<int64_t>(size) &&
		vfs->read(file, bytes, sizeof(bytes)) == sizeof(bytes) &&
		!memcmp(bytes, data, sizeof(bytes)) &&
		vfs->seek(file, -static_cast<int64_t>(sizeof(bytes)), RETRO_VFS_SEEK_POSITION_END) ==
			static_cast<int64_t>(size - sizeof(bytes)) &&
		vfs->read(file, bytes, sizeof(bytes)) == sizeof(bytes) &&
		!memcmp(bytes, data + size - sizeof(bytes), sizeof(bytes));
	return vfs->close(file) == 0 && ok;
}

static int run_probe(const struct shell *shell, size_t argc, char **argv)
{
	unsigned frames = 30;

	if (argc == 3) {
		char *end;
		unsigned long value = std::strtoul(argv[2], &end, 10);

		if (*end || value < 1 || value > 120) {
			return -EINVAL;
		}
		frames = value;
	}
	struct emu_stats old;
	emu_get_stats(&old);
	if (old.running) {
		shell_error(shell, "stop the current game before probing a core");
		return -EBUSY;
	}
	const char *error = nullptr;
	size_t size;
	struct fs_file_t pin;
	storage_guard_lock();
	const uint8_t *data = storage_pin_rom(argv[1], &size, &error, &pin);

	if (!data) {
		storage_guard_unlock();
		shell_error(shell, "%s", error ? error : "content unavailable");
		return -EIO;
	}
	struct emu_content_info content;
	int rc = emu_content_identify(argv[1], data, size, &content);

	if (rc != 0) {
		fs_close(&pin);
		storage_guard_unlock();
		shell_error(shell, "content identification: %d", rc);
		return rc;
	}
	const struct lr_core *core = lr_core_for_system(content.system);

	if (!core) {
		fs_close(&pin);
		storage_guard_unlock();
		return -ENOTSUP;
	}
	probe = {};
	probe.limit = frames;
	struct lr_session session = {};
	const struct lr_callbacks callbacks = {video, audio_sample, audio_batch, input_poll, input_state};
	shell_print(shell, "headless probe: %s, %zu cartridge bytes", core->name, content.size);
	k_msleep(20);
	rc = lr_session_open(&session, &content, argv[1], &callbacks);
	if (rc != 0) {
		fs_close(&pin);
		storage_guard_unlock();
		shell_error(shell, "session open: %d", rc);
		return rc;
	}
	storage_guard_unlock();
	if (!allocator_check() || !vfs_check(argv[1], data, size)) {
		rc = -EFAULT;
	} else {
		shell_print(shell, "allocator and VFS read/seek checks passed");
	}
	shell_print(shell, "core ready: %u milli-fps, %u Hz, state=%zu bytes",
		    (unsigned)(session.av.timing.fps * 1000), (unsigned)session.av.timing.sample_rate,
		    core->serialize_size());
	uint32_t elapsed_us = 0;
	for (; rc == 0 && probe.frame < frames; ++probe.frame) {
		uint32_t start = k_cycle_get_32();
		rc = lr_session_run(&session);
		elapsed_us += k_cyc_to_us_floor32(k_cycle_get_32() - start);
		if (probe.invalid) {
			rc = -EIO;
		}
		k_msleep(1);
	}
	shell_print(shell, "core runtime=%u us (video CRC on first and last frame)", elapsed_us);
	size_t capacity, used, peak;
	lr_memory_stats(&capacity, &used, &peak);
	shell_print(shell, "rc=%d frames=%u video=%u size=%ux%u audio=%u crc32=%08x",
		    rc, probe.frame, probe.video, probe.width, probe.height, probe.samples, probe.crc);
	shell_print(shell, "heap used=%zu peak=%zu capacity=%zu", used, peak, capacity);
	if (rc == 0) {
		size_t state_bytes;
		void *state = lr_session_state_acquire(&session, &state_bytes);
		int state_rc = state ? lr_session_state_transfer(&session, state, state_bytes, false) : -ENOMEM;
		if (state_rc == 0) {
			state_rc = lr_session_reset(&session);
			if (state_rc == 0) {
				state_rc = lr_session_state_transfer(&session, state, state_bytes, true);
			}
			if (state_rc == 0) {
				state_rc = lr_session_run(&session);
				if (!state_rc && probe.invalid) {
					state_rc = -EIO;
				}
			}
		}
		lr_session_state_release(&session, state);
		shell_print(shell, "state roundtrip rc=%d bytes=%zu", state_rc, state_bytes);
		rc = state_rc;
	}
	storage_guard_lock();
	int close_rc = lr_session_close(&session);
	fs_close(&pin);
	storage_guard_unlock();
	if (rc == 0) {
		rc = close_rc;
	}
	return rc;
}

static Z_KERNEL_STACK_DEFINE_IN(probe_stack, 24 * 1024, __attribute__((section("WIFI_RAM"))));
static struct k_thread probe_thread;

struct probe_request {
	const struct shell *shell;
	size_t argc;
	char **argv;
	int result;
};

static void probe_worker(void *arg, void *, void *)
{
	auto *request = static_cast<probe_request *>(arg);
	request->result = run_probe(request->shell, request->argc, request->argv);
}

static int command_probe(const struct shell *shell, size_t argc, char **argv)
{
	struct probe_request request = {shell, argc, argv, 0};
	k_thread_create(&probe_thread, probe_stack, K_KERNEL_STACK_SIZEOF(probe_stack),
			probe_worker, &request, nullptr, nullptr, 10, 0, K_FOREVER);
	k_thread_name_set(&probe_thread, "lrprobe");
	k_thread_start(&probe_thread);
	k_thread_join(&probe_thread, K_FOREVER);
	size_t unused;
	if (k_thread_stack_space_get(&probe_thread, &unused) == 0) {
		shell_print(shell, "stack used=%zu capacity=%zu", K_KERNEL_STACK_SIZEOF(probe_stack) - unused,
			    K_KERNEL_STACK_SIZEOF(probe_stack));
	}
	return request.result;
}

SHELL_CMD_ARG_REGISTER(lrprobe, NULL, "<path> [1-120 frames] — headless core/memory diagnostic",
		      command_probe, 2, 1);
