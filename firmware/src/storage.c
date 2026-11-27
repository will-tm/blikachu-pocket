/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include "storage.h"
#include "emu.h"
#include "menu.h"
#include "libretro/workspace.h"

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

#define STORAGE_ID DT_PARTITION_ID(DT_NODELABEL(storage_partition))
#define ROM_NODE DT_NODELABEL(rom_partition)
#define ROM_ID DT_PARTITION_ID(ROM_NODE)
#define ROM_BYTES DT_REG_SIZE(ROM_NODE)
#define ROM_XIP DT_REG_ADDR(ROM_NODE)
#define ERASE_SIZE 4096U
#define ROM_PAGES (ROM_BYTES / ERASE_SIZE)
#define ROM_MAGIC 0x314d4f52U /* ROM1 */
#define ROM_TEMP STORAGE_MOUNT_POINT "/.rom-upload.tmp"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(g_lfs);
static struct fs_mount_t g_mount = {
	.type = FS_LITTLEFS, .fs_data = &g_lfs,
	.storage_dev = (void *)(uintptr_t)STORAGE_ID,
	.mnt_point = STORAGE_MOUNT_POINT, .flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static bool g_mounted;
static K_MUTEX_DEFINE(storage_guard);
static uint8_t used_pages[DIV_ROUND_UP(ROM_PAGES, 8)];

struct rom_record {
	uint32_t magic, offset, size, data_crc, record_crc;
};

static bool rom_path_valid(const char *path)
{
	const char prefix[] = STORAGE_MOUNT_POINT "/";
	if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
		return false;
	}
	const char *name = path + sizeof(prefix) - 1;
	return *name && *name != '.' && !strchr(name, '/') && !strchr(name, '\\') &&
	       strlen(name) < ROM_NAME_MAX && emu_content_extension_supported(name);
}

extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

void storage_guard_lock(void) { k_mutex_lock(&storage_guard, K_FOREVER); }
void storage_guard_unlock(void) { k_mutex_unlock(&storage_guard); }

int storage_mount(void)
{
	storage_guard_lock();
	int rc = g_mounted ? 0 : fs_mount(&g_mount);

	if (rc == 0) {
		g_mounted = true;
	}
	storage_guard_unlock();
	return rc;
}

int storage_unmount(void)
{
	storage_guard_lock();
	int rc = lr_workspace_core_owned() ? -EBUSY : (g_mounted ? fs_unmount(&g_mount) : 0);

	if (rc == 0) {
		g_mounted = false;
	}
	storage_guard_unlock();
	return rc;
}

int storage_mkfs(void)
{
	storage_guard_lock();
	if (lr_workspace_core_owned() || g_mounted) {
		storage_guard_unlock();
		return -EBUSY;
	}
	resetinfo_wdt_pause();
	int rc = fs_mkfs(FS_LITTLEFS, STORAGE_ID, &g_lfs, 0);

	resetinfo_wdt_resume();
	storage_guard_unlock();
	return rc;
}

static bool record_valid(const struct rom_record *r)
{
	return r->magic == ROM_MAGIC && r->offset % ERASE_SIZE == 0 &&
	       r->offset < ROM_BYTES && r->size >= 4 && r->size <= ROM_BYTES - r->offset &&
	       r->record_crc == crc32_ieee((const uint8_t *)r, offsetof(struct rom_record, record_crc));
}

static int read_record(struct fs_file_t *f, struct rom_record *r)
{
	uint8_t extra;

	return fs_read(f, r, sizeof(*r)) == sizeof(*r) &&
	       fs_read(f, &extra, 1) == 0 && record_valid(r) ? 0 : -EBADMSG;
}

static int load_record(const char *path, struct rom_record *r)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_READ);

	if (!rc) {
		rc = read_record(&f, r);
		int close_rc = fs_close(&f);
		if (!rc) {
			rc = close_rc;
		}
	}
	return rc;
}

int storage_rom_info(const char *path, size_t *size)
{
	struct rom_record r;
	int rc = load_record(path, &r);

	if (!rc) {
		*size = r.size;
	}
	return rc;
}

static bool page_used(uint32_t p)
{
	return used_pages[p / 8] & BIT(p % 8);
}

/* Reconstruct allocation from the committed catalogue; no mutable raw-flash
 * allocation table is needed. An invalid or overlapping record fails closed.
 */
static int scan_catalogue(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	memset(used_pages, 0, sizeof(used_pages));
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, STORAGE_MOUNT_POINT);

	if (rc) {
		return rc;
	}
	while (!(rc = fs_readdir(&dir, &ent)) && ent.name[0]) {
		if (ent.type != FS_DIR_ENTRY_FILE || !emu_content_extension_supported(ent.name)) {
			continue;
		}
		char path[sizeof(STORAGE_MOUNT_POINT) + sizeof(ent.name) + 1];
		struct rom_record r;

		snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", ent.name);
		rc = load_record(path, &r);
		if (rc) {
			break;
		}
		uint32_t first = r.offset / ERASE_SIZE;
		uint32_t count = DIV_ROUND_UP(r.size, ERASE_SIZE);
		for (uint32_t p = first; p < first + count; p++) {
			if (page_used(p)) {
				rc = -EBADMSG;
				break;
			}
			used_pages[p / 8] |= BIT(p % 8);
		}
		if (rc) {
			break;
		}
	}
	int close_rc = fs_closedir(&dir);
	return rc ? rc : close_rc;
}

size_t storage_rom_free(void)
{
	storage_guard_lock();
	size_t free = 0;

	if (!scan_catalogue()) {
		for (uint32_t p = 0; p < ROM_PAGES; p++) {
			free += page_used(p) ? 0 : ERASE_SIZE;
		}
	}
	storage_guard_unlock();
	return free;
}

static int allocate_extent(size_t size, uint32_t *offset)
{
	int rc = scan_catalogue();
	if (rc) {
		return rc;
	}
	uint32_t count = DIV_ROUND_UP(size, ERASE_SIZE);
	uint32_t run = 0;

	for (uint32_t p = 0; p < ROM_PAGES; p++) {
		run = page_used(p) ? 0 : run + 1;
		if (run == count) {
			*offset = (p + 1 - count) * ERASE_SIZE;
			return 0;
		}
	}
	return -ENOSPC;
}

static int publish(const char *path, const struct rom_record *r)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, ROM_TEMP, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (!rc) {
		rc = fs_write(&f, r, sizeof(*r)) == sizeof(*r) ? 0 : -EIO;
		if (!rc) {
			rc = fs_sync(&f);
		}
		int close_rc = fs_close(&f);
		if (!rc) {
			rc = close_rc;
		}
	}
	/* LittleFS rename atomically replaces the old catalogue entry. */
	return rc ? rc : fs_rename(ROM_TEMP, path);
}

int storage_rom_install(const char *path, const uint8_t *data, size_t size)
{
	if (size < 4 || size > ROM_BYTES || !rom_path_valid(path)) {
		return -EINVAL;
	}
	storage_guard_lock();
	if (lr_workspace_core_owned()) {
		storage_guard_unlock();
		return -EBUSY;
	}
	struct rom_record r = {.magic = ROM_MAGIC, .size = size};
	int rc = allocate_extent(size, &r.offset);
	const struct flash_area *fa = NULL;

	if (!rc) {
		rc = flash_area_open(ROM_ID, &fa);
	}
	if (rc) {
		storage_guard_unlock();
		return rc;
	}
	resetinfo_wdt_pause();
	for (size_t pos = 0; !rc && pos < size; pos += ERASE_SIZE) {
		size_t n = MIN(size - pos, ERASE_SIZE);
		size_t aligned = ROUND_DOWN(n, 4);

		rc = flash_area_erase(fa, r.offset + pos, ERASE_SIZE);
		if (!rc && aligned) {
			rc = flash_area_write(fa, r.offset + pos, data + pos, aligned);
		}
		if (!rc && aligned != n) {
			uint32_t tail = UINT32_MAX;
			memcpy(&tail, data + pos + aligned, n - aligned);
			rc = flash_area_write(fa, r.offset + pos + aligned, &tail, sizeof(tail));
		}
		k_sleep(K_MSEC(1));
	}
	flash_area_close(fa);
	if (!rc) {
		r.data_crc = crc32_ieee(data, size);
		if (crc32_ieee((const uint8_t *)(uintptr_t)(ROM_XIP + r.offset), size) != r.data_crc) {
			rc = -EIO;
		}
	}
	if (!rc) {
		r.record_crc = crc32_ieee((const uint8_t *)&r, offsetof(struct rom_record, record_crc));
		rc = publish(path, &r);
	}
	resetinfo_wdt_resume();
	storage_guard_unlock();
	LOG_INF("ROM install %zu bytes: %d", size, rc);
	return rc;
}

const uint8_t *resolve_file_xip(const char *path, size_t *out_size, const char **err_out)
{
	struct rom_record r;
	int rc = load_record(path, &r);

	if (err_out) {
		*err_out = rc ? "invalid ROM catalogue entry" : NULL;
	}
	if (rc) {
		return NULL;
	}
	if (out_size) {
		*out_size = r.size;
	}
	return (const uint8_t *)(uintptr_t)(ROM_XIP + r.offset);
}

const uint8_t *storage_pin_rom(const char *path, size_t *size, const char **error,
			      struct fs_file_t *pin)
{
	struct rom_record r;
	fs_file_t_init(pin);
	int rc = fs_open(pin, path, FS_O_READ);

	if (!rc) {
		rc = read_record(pin, &r);
		if (rc) {
			fs_close(pin);
		}
	}
	if (rc) {
		*error = "invalid ROM catalogue entry";
		return NULL;
	}
	*size = r.size;
	*error = NULL;
	return (const uint8_t *)(uintptr_t)(ROM_XIP + r.offset);
}
