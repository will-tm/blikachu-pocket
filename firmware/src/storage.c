#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <ff.h>

#include "storage.h"
#include "libretro/workspace.h"

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

/* Base address the whole flash is XIP-mapped at. On the BL618 the SF
 * controller runs with override-bank1 + no-header (see the board DTS),
 * so the mapping is flat from flash byte 0.
 * resolve_file_xip() self-verifies with a 4-byte read-back compare, so
 * a wrong base fails loudly instead of returning garbage pointers.
 */
#define STORAGE_XIP_BASE        0xa0000000UL
#define STORAGE_PARTITION_NODE DT_NODELABEL(storage_partition)
#define STORAGE_PARTITION_ID   DT_FIXED_PARTITION_ID(STORAGE_PARTITION_NODE)

static FATFS g_fatfs_obj;
static struct fs_mount_t g_fatfs_mnt = {
	.type = FS_FATFS,
	.fs_data = &g_fatfs_obj,
	.mnt_point = STORAGE_MOUNT_POINT,
};
static bool g_mounted;
static K_MUTEX_DEFINE(storage_guard);

void storage_guard_lock(void) { k_mutex_lock(&storage_guard, K_FOREVER); }
void storage_guard_unlock(void) { k_mutex_unlock(&storage_guard); }

int storage_mount(void)
{
	if (g_mounted) {
		return 0;
	}
	int rc = fs_mount(&g_fatfs_mnt);
	if (rc == 0 || rc == -EBUSY) {
		g_mounted = true;
		return 0;
	}
	return rc;
}

int storage_unmount(void)
{
	storage_guard_lock();
	if (lr_workspace_core_owned()) {
		storage_guard_unlock();
		return -EBUSY;
	}
	if (!g_mounted) {
		storage_guard_unlock();
		return 0;
	}
	int rc = fs_unmount(&g_fatfs_mnt);
	if (rc == 0) {
		g_mounted = false;
	}
	storage_guard_unlock();
	return rc;
}

int storage_mkfs(void)
{
	storage_guard_lock();
	if (lr_workspace_core_owned()) {
		storage_guard_unlock();
		return -EBUSY;
	}
	/* FM_FAT without FM_SFD: FatFs writes an MBR + partition table
	 * that macOS recognizes, then formats the partition as FAT12/16.
	 */
	MKFS_PARM opt = {.fmt = FM_FAT};
	int rc = fs_mkfs(FS_FATFS, (uintptr_t)STORAGE_DISK_NAME ":", &opt, 0);
	storage_guard_unlock();
	return rc;
}

/* Exported so callers can pointer-compare the error and react (defrag). */
const char *const STORAGE_ERR_FRAGMENTED = "file is fragmented";

const uint8_t *resolve_file_xip(const char *path, size_t *out_size, const char **err_out)
{
	const struct flash_area *fa;
	struct fs_file_t zfp;
	uint8_t probe_fs[4], probe_xip[4];
	int rc;

	if (err_out) {
		*err_out = NULL;
	}

	fs_file_t_init(&zfp);
	rc = fs_open(&zfp, path, FS_O_READ);
	if (rc) {
		if (err_out) {
			*err_out = "fs_open failed";
		}
		return NULL;
	}

	FIL *fp = zfp.filep;
	FATFS *fs = fp->obj.fs;
	DWORD sclust = fp->obj.sclust;
	FSIZE_t size = fp->obj.objsize;

	const DWORD bytes_per_clust = (DWORD)fs->csize * 512;
	const DWORD n_clust = (size + bytes_per_clust - 1) / bytes_per_clust;
	for (DWORD i = 1; i < n_clust; i++) {
		FSIZE_t off = (FSIZE_t)i * bytes_per_clust + 1;
		FRESULT fr = f_lseek(fp, off);
		if (fr != FR_OK) {
			fs_close(&zfp);
			if (err_out) {
				*err_out = "f_lseek failed";
			}
			return NULL;
		}
		if (fp->clust != sclust + i) {
			fs_close(&zfp);
			if (err_out) {
				*err_out = STORAGE_ERR_FRAGMENTED;
			}
			return NULL;
		}
	}
	(void)f_lseek(fp, 0);

	LBA_t start_lba = fs->database + (LBA_t)(sclust - 2) * fs->csize;

	rc = flash_area_open(STORAGE_PARTITION_ID, &fa);
	if (rc) {
		fs_close(&zfp);
		if (err_out) {
			*err_out = "flash_area_open failed";
		}
		return NULL;
	}

	uintptr_t flash_off = fa->fa_off + (uintptr_t)start_lba * 512;
	const uint8_t *xip = (const uint8_t *)(STORAGE_XIP_BASE + flash_off);

	rc = fs_read(&zfp, probe_fs, sizeof(probe_fs));
	if (rc != (int)sizeof(probe_fs)) {
		fs_close(&zfp);
		if (err_out) {
			*err_out = "probe read failed";
		}
		return NULL;
	}
	memcpy(probe_xip, xip, sizeof(probe_xip));
	fs_close(&zfp);

	if (memcmp(probe_fs, probe_xip, sizeof(probe_fs)) != 0) {
		if (err_out) {
			*err_out = "XIP / FATFS mismatch";
		}
		return NULL;
	}

	if (out_size) {
		*out_size = (size_t)size;
	}
	return xip;
}

const uint8_t *storage_pin_rom(const char *path, size_t *size, const char **error,
			       struct fs_file_t *pin)
{
	fs_file_t_init(pin);
	if (fs_open(pin, path, FS_O_READ) != 0) {
		*error = "cannot pin ROM";
		return NULL;
	}
	const uint8_t *rom = resolve_file_xip(path, size, error);
	if (!rom) {
		fs_close(pin);
	}
	return rom;
}

/* Rewrite a file into one physically-contiguous cluster run so
 * resolve_file_xip() can map it — host USB-MSC copies onto a fullish
 * FAT often land fragmented. f_expand() preallocates the contiguous
 * run up front (fails cleanly if no free run is big enough — needs
 * scratch space equal to the file size), then the content is
 * stream-copied and swapped in via rename. ~2-4 s per MiB.
 */
#define DEFRAG_TMP STORAGE_MOUNT_POINT "/.defrag.tmp"

/* resetinfo.c: defrag rewrites whole files — long flash op, must not
 * race the 2 s BL61x watchdog (menu/shell/webui all launch through
 * here on fragmented ROMs).
 */
extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

static int storage_defrag_file_locked(const char *path);

int storage_defrag_file(const char *path)
{
	storage_guard_lock();
	if (lr_workspace_core_owned()) {
		storage_guard_unlock();
		return -EBUSY;
	}
	int drc;

	resetinfo_wdt_pause();
	drc = storage_defrag_file_locked(path);
	resetinfo_wdt_resume();
	storage_guard_unlock();
	return drc;
}

static int storage_defrag_file_locked(const char *path)
{
	static K_MUTEX_DEFINE(mtx);
	static uint8_t buf[4096];
	struct fs_file_t src, dst;
	struct fs_dirent ent;
	int rc;

	rc = fs_stat(path, &ent);
	if (rc) {
		return rc;
	}
	const size_t size = ent.size;

	k_mutex_lock(&mtx, K_FOREVER);
	/* fs_stat first — fs_unlink logs an ERR when the file is absent. */
	if (fs_stat(DEFRAG_TMP, &ent) == 0) {
		(void)fs_unlink(DEFRAG_TMP);
	}

	fs_file_t_init(&src);
	fs_file_t_init(&dst);
	rc = fs_open(&src, path, FS_O_READ);
	if (rc) {
		goto out;
	}
	rc = fs_open(&dst, DEFRAG_TMP, FS_O_CREATE | FS_O_WRITE);
	if (rc) {
		fs_close(&src);
		goto out;
	}

	FRESULT fr = f_expand(dst.filep, size, 1 /* allocate now */);

	if (fr != FR_OK) {
		LOG_ERR("defrag %s: no contiguous %u B free run (FR %d)", path,
			(unsigned)size, fr);
		rc = -ENOSPC;
		goto close;
	}

	for (size_t left = size; left > 0;) {
		ssize_t n = fs_read(&src, buf, MIN(left, sizeof(buf)));

		if (n <= 0 || fs_write(&dst, buf, n) != n) {
			rc = -EIO;
			goto close;
		}
		left -= n;
	}

close:
	fs_close(&src);
	fs_close(&dst);
	if (rc == 0) {
		rc = fs_unlink(path);
		if (rc == 0) {
			rc = fs_rename(DEFRAG_TMP, path);
		}
	}
	if (rc != 0) {
		(void)fs_unlink(DEFRAG_TMP);
	}
out:
	k_mutex_unlock(&mtx);
	LOG_INF("defrag %s: %d", path, rc);
	return rc;
}
