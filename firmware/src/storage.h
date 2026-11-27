#ifndef BLIKACHU_STORAGE_H
#define BLIKACHU_STORAGE_H

#include <stddef.h>
#include <stdint.h>

struct fs_file_t;

/* Disk-driver name used by the flash-disk DT node, the USBD MSC LUN, and
 * Zephyr's fs / disk_access APIs. Kept project-local so we don't depend
 * on the legacy CONFIG_MASS_STORAGE_DISK_NAME Kconfig.
 */
#define STORAGE_DISK_NAME   "NAND"
#define STORAGE_MOUNT_POINT "/" STORAGE_DISK_NAME ":"

/* Mount the FATFS volume at /NAND:. Idempotent — if already mounted by
 * a previous call (or the menu's auto-mount on boot), returns 0.
 */
int storage_mount(void);
/* Serialize mapping/claim and destructive volume operations. */
void storage_guard_lock(void);
void storage_guard_unlock(void);

/* Unmount the volume so the USB host can own the LUN cleanly. */
int storage_unmount(void);

/* Format the LUN as exFAT. The volume must NOT be mounted (call
 * storage_unmount() first or skip storage_mount()). Returns 0 on success.
 */
int storage_mkfs(void);

/* Resolve a file's contiguous bytes on /NAND: to a flat XIP-mapped
 * pointer. Verifies cluster contiguity by walking the chain via
 * f_lseek; if any cluster doesn't follow the previous one, returns
 * NULL with `*err_out` set to a static description string.
 *
 * On success returns a `const uint8_t*` into 0x10xxxxxx valid for the
 * lifetime of the underlying FAT volume (i.e. until you reformat or
 * delete the file).
 */
const uint8_t *resolve_file_xip(const char *path, size_t *out_size, const char **err_out);
/* Hold a read handle until the core has unloaded; FatFs sharing locks
 * prevent writes, renames and deletion of that file during gameplay. */
const uint8_t *storage_pin_rom(const char *path, size_t *size, const char **error,
			       struct fs_file_t *pin);

/* The `*err_out` value resolve_file_xip() sets when the file's cluster
 * chain isn't contiguous — pointer-comparable, so callers can react
 * (defrag + retry) without string matching.
 */
extern const char *const STORAGE_ERR_FRAGMENTED;

/* Rewrite a file into one contiguous cluster run (fixes
 * STORAGE_ERR_FRAGMENTED). Needs free space >= the file size for the
 * scratch copy; -ENOSPC when no contiguous free run exists. Blocks for
 * the copy (~2-4 s per MiB).
 */
int storage_defrag_file(const char *path);

#endif
