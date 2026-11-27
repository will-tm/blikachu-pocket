/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>

#include "emu.h"
#include "libretro/workspace.h"
#include "menu.h"
#include "storage.h"

#define RX_CHUNK 512U
#define RX_TIMEOUT K_SECONDS(30)
#define SAVES_DIR STORAGE_MOUNT_POINT "/.saves"
#define SAVE_TEMP SAVES_DIR "/.usb-upload.tmp"

static K_MUTEX_DEFINE(rx_lock);
static const struct shell *rx_shell;
static uint8_t *rx_buf;
static size_t rx_size, rx_received, rx_ack;
static uint32_t rx_crc;
static bool rx_rom;
static char rx_path[128];

extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

static int commit_save(void)
{
	struct fs_file_t f;
	struct fs_dirent ent;
	int rc;

	storage_guard_lock();
	resetinfo_wdt_pause();
	rc = fs_stat(SAVES_DIR, &ent);
	if (rc == -ENOENT) {
		rc = fs_mkdir(SAVES_DIR);
	}
	fs_file_t_init(&f);
	if (!rc) {
		rc = fs_open(&f, SAVE_TEMP, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	}
	if (!rc) {
		for (size_t pos = 0; !rc && pos < rx_size;) {
			size_t n = MIN(rx_size - pos, 16U * 1024U);
			if (fs_write(&f, rx_buf + pos, n) != (ssize_t)n) {
				rc = -EIO;
			} else {
				pos += n;
			}
			k_sleep(K_MSEC(1));
		}
		if (!rc) {
			rc = fs_sync(&f);
		}
		int close_rc = fs_close(&f);
		if (!rc) {
			rc = close_rc;
		}
	}
	if (!rc) {
		rc = fs_rename(SAVE_TEMP, rx_path);
	}
	resetinfo_wdt_resume();
	storage_guard_unlock();
	return rc;
}

static void finish_locked(int rc)
{
	const struct shell *sh = rx_shell;
	bool refresh = rx_rom && !rc;

	shell_set_bypass(sh, NULL, NULL);
	rx_shell = NULL;
	rx_buf = NULL;
	(void)lr_workspace_release(LR_WORKSPACE_STAGING);
	if (!rc) {
		shell_print(sh, "UPLOAD OK %zu %08x", rx_size, rx_crc);
	} else {
		shell_error(sh, "UPLOAD ERR %d", rc);
	}
	if (refresh) {
		menu_refresh();
	}
}

static void timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&rx_lock, K_FOREVER);
	if (rx_shell) {
		finish_locked(-ETIMEDOUT);
	}
	k_mutex_unlock(&rx_lock);
}
static K_WORK_DELAYABLE_DEFINE(rx_timeout, timeout_fn);

static void receive(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);
	k_mutex_lock(&rx_lock, K_FOREVER);
	if (rx_shell != sh) {
		k_mutex_unlock(&rx_lock);
		return;
	}
	if (len > rx_size - rx_received) {
		(void)k_work_cancel_delayable(&rx_timeout);
		finish_locked(-EMSGSIZE);
		k_mutex_unlock(&rx_lock);
		return;
	}
	memcpy(rx_buf + rx_received, data, len);
	rx_received += len;
	if (rx_received == rx_size) {
		(void)k_work_cancel_delayable(&rx_timeout);
		int rc = crc32_ieee(rx_buf, rx_size) == rx_crc ? 0 : -EBADMSG;

		if (!rc) {
			rc = rx_rom ? storage_rom_install(rx_path, rx_buf, rx_size) : commit_save();
		}
		finish_locked(rc);
	} else {
		if (rx_received >= rx_ack + RX_CHUNK) {
			rx_ack = rx_received;
			shell_print(sh, "RX %zu", rx_received);
		}
		(void)k_work_reschedule(&rx_timeout, RX_TIMEOUT);
	}
	k_mutex_unlock(&rx_lock);
}

static bool save_extension(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!strcmp(dot, ".lrstate") || !strcmp(dot, ".lrsram") || !strcmp(dot, ".lrrtc"));
}

static int cmd_receive(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const char *name = argv[1];
	char *end;
	unsigned long n = strtoul(argv[2], &end, 10);
	if (*end || !n || n > LR_STAGING_BYTES || !*name || *name == '.' ||
	    strchr(name, '/') || strchr(name, '\\') || strlen(name) >= ROM_NAME_MAX) {
		return -EINVAL;
	}
	unsigned long crc = strtoul(argv[3], &end, 16);
	if (*end || strlen(argv[3]) != 8) {
		return -EINVAL;
	}
	bool rom = emu_content_extension_supported(name);
	if ((!rom && !save_extension(name)) || (rom && n < 4)) {
		return -EINVAL;
	}
	int rc = storage_mount();
	if (rc) {
		return rc;
	}
	k_mutex_lock(&rx_lock, K_FOREVER);
	if (rx_shell || !(rx_buf = lr_workspace_claim(LR_WORKSPACE_STAGING))) {
		k_mutex_unlock(&rx_lock);
		return -EBUSY;
	}
	rx_shell = sh;
	rx_size = n;
	rx_received = rx_ack = 0;
	rx_crc = crc;
	rx_rom = rom;
	snprintf(rx_path, sizeof(rx_path), "%s/%s", rom ? STORAGE_MOUNT_POINT : SAVES_DIR, name);
	shell_set_bypass(sh, receive, NULL);
	(void)k_work_reschedule(&rx_timeout, RX_TIMEOUT);
	shell_print(sh, "READY %u", RX_CHUNK);
	k_mutex_unlock(&rx_lock);
	return 0;
}
SHELL_CMD_ARG_REGISTER(recv, NULL, "<ROM/save filename> <bytes> <crc32> — binary USB upload",
		      cmd_receive, 4, 0);
