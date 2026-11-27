#ifndef BLIKACHU_STORAGE_H
#define BLIKACHU_STORAGE_H

#include <stddef.h>
#include <stdint.h>

struct fs_file_t;

#define STORAGE_MOUNT_POINT "/NAND:"

int storage_mount(void);
int storage_unmount(void);
int storage_mkfs(void);
void storage_guard_lock(void);
void storage_guard_unlock(void);

/* ROM catalogue entries reference immutable, contiguous flash extents. */
int storage_rom_info(const char *path, size_t *size);
size_t storage_rom_free(void);
int storage_rom_install(const char *path, const uint8_t *data, size_t size);
const uint8_t *resolve_file_xip(const char *path, size_t *out_size, const char **err_out);
const uint8_t *storage_pin_rom(const char *path, size_t *size, const char **error,
			      struct fs_file_t *pin);

#endif
