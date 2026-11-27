/* Storage diagnostics for the handheld.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#include "storage.h"
#include "libretro/workspace.h"
#include "menu.h"

LOG_MODULE_REGISTER(storage_shell, LOG_LEVEL_INF);

/* ---- Filesystem / device shell commands ---- */

static int cmd_mount(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_mount();
	if (rc) {
		shell_error(sh, "mount failed: %d", rc);
		return rc;
	}
	shell_print(sh, "mounted %s", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(mount, NULL, "Mount LittleFS", cmd_mount);

static int cmd_umount(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_unmount();
	if (rc) {
		shell_error(sh, "unmount failed: %d", rc);
		return rc;
	}
	shell_print(sh, "unmounted %s", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(umount, NULL, "Unmount LittleFS", cmd_umount);

static int cmd_mkfs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_mkfs();
	if (rc) {
		shell_error(sh, "mkfs failed: %d", rc);
		return rc;
	}
	shell_print(sh, "formatted %s as LittleFS", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(mkfs, NULL, "Format LittleFS and clear the ROM catalogue", cmd_mkfs);

/* ---- Storage status ---- */
static int cmd_disk(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	struct fs_statvfs sv;
	int rc = fs_statvfs(STORAGE_MOUNT_POINT, &sv);
	if (rc) {
		shell_error(sh, "LittleFS status: %d", rc);
		return rc;
	}
	shell_print(sh, "LittleFS: %llu bytes free; ROM extents: %zu bytes free",
		    (unsigned long long)sv.f_bfree * sv.f_frsize, storage_rom_free());
	return 0;
}
SHELL_CMD_REGISTER(disk, NULL, "Report LittleFS and ROM storage capacity", cmd_disk);

/* Import a regular staging file into the contiguous ROM store. */
static int cmd_romimport(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	struct fs_dirent ent;
	int rc = fs_stat(argv[1], &ent);
	if (rc || ent.size < 4 || ent.size > LR_STAGING_BYTES) {
		return rc ? rc : -EINVAL;
	}
	uint8_t *buf = lr_workspace_claim(LR_WORKSPACE_STAGING);
	if (!buf) {
		return -EBUSY;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	rc = fs_open(&f, argv[1], FS_O_READ);
	if (!rc) {
		rc = fs_read(&f, buf, ent.size) == (ssize_t)ent.size ? 0 : -EIO;
		int close_rc = fs_close(&f);
		if (!rc) {
			rc = close_rc;
		}
	}
	if (!rc) {
		rc = storage_rom_install(argv[2], buf, ent.size);
	}
	(void)lr_workspace_release(LR_WORKSPACE_STAGING);
	if (!rc) {
		shell_print(sh, "imported %zu bytes", ent.size);
		menu_refresh();
	} else {
		shell_error(sh, "import failed: %d", rc);
	}
	return rc;
}
SHELL_CMD_ARG_REGISTER(romimport, NULL, "<staged file> <ROM path>", cmd_romimport, 3, 0);

static int cmd_romaddr(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: romaddr <path>");
		return -EINVAL;
	}
	const char *err = NULL;
	size_t size;
	const uint8_t *p = resolve_file_xip(argv[1], &size, &err);
	if (!p) {
		shell_error(sh, "%s: %s", argv[1], err ? err : "unknown");
		return -EIO;
	}
	shell_print(sh, "%s -> %p (%zu bytes), header: %02x %02x %02x %02x", argv[1], p, size, p[0],
		    p[1], p[2], p[3]);
	return 0;
}
SHELL_CMD_REGISTER(romaddr, NULL, "Resolve /NAND: file to XIP pointer", cmd_romaddr);

static int cmd_romcrc(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const char *err = NULL;
	size_t size;
	const uint8_t *rom = resolve_file_xip(argv[1], &size, &err);

	if (!rom) {
		shell_error(sh, "%s: %s", argv[1], err ? err : "unavailable");
		return -EIO;
	}
	uint32_t crc = 0;

	for (size_t offset = 0; offset < size;) {
		size_t bytes = MIN(size - offset, (size_t)65536);

		crc = crc32_ieee_update(crc, rom + offset, bytes);
		offset += bytes;
		k_sleep(K_MSEC(1));
	}
	shell_print(sh, "crc32=%08x bytes=%zu", crc, size);
	return 0;
}
SHELL_CMD_ARG_REGISTER(romcrc, NULL, "<path> — CRC32 of flash-mapped file bytes",
		      cmd_romcrc, 2, 0);
