/* SPDX-License-Identifier: Apache-2.0 */
#include "state.h"
#include "../storage.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#define STATE_MAGIC 0x3153524cU /* LRS1 */
#define SAVES STORAGE_MOUNT_POINT "/.saves"

struct state_header {
	uint32_t magic, version, system, rom_crc, kind, size, data_crc;
};

extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

int lr_state_path(char *path, size_t capacity, const struct lr_session *session,
		  uint32_t rom_crc, const char *suffix)
{
	int n = snprintf(path, capacity, SAVES "/%s-%08x.%s", session->core->name, rom_crc, suffix);
	return n < 0 || (size_t)n >= capacity ? -ENAMETOOLONG : 0;
}

static int transfer(const struct lr_session *session, const char *path, uint32_t rom_crc,
		    unsigned kind, void *data, size_t size, bool restore)
{
	struct state_header header = {STATE_MAGIC, 1, session->system, rom_crc, kind, size, 0};
	struct fs_file_t file;
	fs_file_t_init(&file);
	int rc = fs_open(&file, path, restore ? FS_O_READ : FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}
	if (restore) {
		struct state_header actual;
		if (fs_read(&file, &actual, sizeof(actual)) != sizeof(actual) ||
		    memcmp(&header, &actual, offsetof(struct state_header, data_crc)) != 0) {
			rc = -EINVAL;
		} else {
			uint8_t block[512];
			uint32_t crc = 0;
			for (size_t remaining = size; remaining; ) {
				size_t n = remaining < sizeof(block) ? remaining : sizeof(block);
				if (fs_read(&file, block, n) != (ssize_t)n) {
					rc = -EBADMSG;
					break;
				}
				crc = crc32_ieee_update(crc, block, n);
				remaining -= n;
			}
			if (!rc && (crc != actual.data_crc || fs_read(&file, block, 1) != 0)) {
				rc = -EBADMSG;
			}
			if (!rc && (fs_seek(&file, sizeof(header), FS_SEEK_SET) ||
				    fs_read(&file, data, size) != (ssize_t)size)) {
				rc = -EIO;
			}
		}
	} else {
		header.data_crc = crc32_ieee(data, size);
		if (fs_write(&file, &header, sizeof(header)) != sizeof(header) ||
		    fs_write(&file, data, size) != (ssize_t)size) {
			rc = -EIO;
		}
	}
	int close_rc = fs_close(&file);
	return rc ? rc : close_rc;
}

int lr_state_file(struct lr_session *session, const char *path, uint32_t rom_crc, bool restore)
{
	/* An explicit state path cannot overwrite a legacy .state or ROM. */
	const char *dot = strrchr(path, '.');
	if (!dot || strcmp(dot, ".lrstate") != 0) {
		return -EINVAL;
	}
	size_t size;
	void *buffer = lr_session_state_acquire(session, &size);
	if (!buffer) {
		return -ENOMEM;
	}
	int rc = 0;
	if (!restore) {
		rc = lr_session_state_transfer(session, buffer, size, false);
	}
	if (!rc) {
		resetinfo_wdt_pause();
		rc = transfer(session, path, rom_crc, 0, buffer, size, restore);
		resetinfo_wdt_resume();
	}
	if (!rc && restore) {
		rc = lr_session_state_transfer(session, buffer, size, true);
	}
	lr_session_state_release(session, buffer);
	return rc;
}

int lr_battery_file(struct lr_session *session, uint32_t rom_crc, bool restore)
{
	const unsigned types[] = {RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC};
	const char *suffixes[] = {"lrsram", "lrrtc"};
	int result = 0;
	for (unsigned i = 0; i < 2; ++i) {
		size_t size = session->core->get_memory_size(types[i]);
		void *data = session->core->get_memory_data(types[i]);
		if (!size || !data) {
			continue;
		}
		char path[96];
		int rc = lr_state_path(path, sizeof(path), session, rom_crc, suffixes[i]);
		if (!rc) {
			resetinfo_wdt_pause();
			rc = transfer(session, path, rom_crc, i + 1, data, size, restore);
			resetinfo_wdt_resume();
		}
		if (rc && !(restore && rc == -ENOENT)) {
			result = rc;
		}
	}
	return result;
}
