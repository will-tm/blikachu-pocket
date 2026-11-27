/* SPDX-License-Identifier: Apache-2.0 */
/* NOR power cut simulation for publishing a replacement catalogue record. */
#include "lfs.h"
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 4096U
#define BLOCK_COUNT 64U
static uint8_t image[BLOCK_SIZE * BLOCK_COUNT], initial[sizeof(image)];
static uint8_t read_cache[256], prog_cache[256], lookahead[64], file_cache[256];
static jmp_buf power_cut;
static unsigned mutations, cut_at, fraction;

static int read_nor(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
		    void *buffer, lfs_size_t size)
{
	(void)c;
	memcpy(buffer, image + block * BLOCK_SIZE + off, size);
	return 0;
}

static int mutate(lfs_block_t block, lfs_off_t off, const uint8_t *src, lfs_size_t n)
{
	bool cut = ++mutations == cut_at;
	lfs_size_t count = cut ? n * fraction / 4 : n;
	uint8_t *dst = image + block * BLOCK_SIZE + off;

	for (lfs_size_t i = 0; i < count; i++) {
		if (src) {
			assert((dst[i] & src[i]) == src[i]);
			dst[i] &= src[i];
		} else {
			dst[i] = 0xff;
		}
	}
	if (cut) {
		longjmp(power_cut, 1);
	}
	return 0;
}

static int prog_nor(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
		    const void *buffer, lfs_size_t size)
{
	(void)c;
	return mutate(block, off, buffer, size);
}

static int erase_nor(const struct lfs_config *c, lfs_block_t block)
{
	(void)c;
	return mutate(block, 0, NULL, BLOCK_SIZE);
}

static int sync_nor(const struct lfs_config *c) { (void)c; return 0; }
static const struct lfs_config cfg = {
	.read = read_nor, .prog = prog_nor, .erase = erase_nor, .sync = sync_nor,
	.read_size = 16, .prog_size = 16, .block_size = BLOCK_SIZE,
	.block_count = BLOCK_COUNT, .block_cycles = 512, .cache_size = 256,
	.lookahead_size = 64, .read_buffer = read_cache, .prog_buffer = prog_cache,
	.lookahead_buffer = lookahead,
};
static const struct lfs_file_config file_cfg = {.buffer = file_cache};
static const uint32_t old_record[] = {0x314d4f52, 0, 8195, 0x1234, 0x5678};
static const uint32_t new_record[] = {0x314d4f52, 12288, 8195, 0x4321, 0x8765};

static void publish(lfs_t *lfs, const uint32_t *record)
{
	lfs_file_t f = {0};
	assert(lfs_file_opencfg(lfs, &f, ".rom-upload.tmp",
				LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC, &file_cfg) == 0);
	assert(lfs_file_write(lfs, &f, record, sizeof(old_record)) == sizeof(old_record));
	assert(lfs_file_sync(lfs, &f) == 0);
	assert(lfs_file_close(lfs, &f) == 0);
	assert(lfs_rename(lfs, ".rom-upload.tmp", "game.gbc") == 0);
}

static void check_catalogue(void)
{
	lfs_t fs = {0};
	lfs_file_t f = {0};
	uint32_t record[5];
	assert(lfs_mount(&fs, &cfg) == 0);
	assert(lfs_file_opencfg(&fs, &f, "game.gbc", LFS_O_RDONLY, &file_cfg) == 0);
	assert(lfs_file_read(&fs, &f, record, sizeof(record)) == sizeof(record));
	assert(!memcmp(record, old_record, sizeof(record)) ||
	       !memcmp(record, new_record, sizeof(record)));
	assert(lfs_file_close(&fs, &f) == 0);
	assert(lfs_unmount(&fs) == 0);
}

int main(void)
{
	const unsigned histories[] = {0, 1, 10, 30, 60, 100, 150, 200};
	unsigned trials = 0;
	unsigned max_operations = 0;

	for (unsigned h = 0; h < sizeof(histories) / sizeof(histories[0]); h++) {
		lfs_t fs = {0};
		cut_at = 0;
		memset(image, 0xff, sizeof(image));
		assert(lfs_format(&fs, &cfg) == 0);
		assert(lfs_mount(&fs, &cfg) == 0);
		for (unsigned i = 0; i < histories[h]; i++) {
			publish(&fs, i & 1 ? old_record : new_record);
		}
		publish(&fs, old_record);
		assert(lfs_unmount(&fs) == 0);
		memcpy(initial, image, sizeof(image));
		assert(lfs_mount(&fs, &cfg) == 0);
		mutations = 0;
		publish(&fs, new_record);
		unsigned operation_count = mutations;
		if (operation_count > max_operations) {
			max_operations = operation_count;
		}
		assert(lfs_unmount(&fs) == 0);
		for (unsigned op = 1; op <= operation_count; op++) {
			for (unsigned p = 0; p <= 4; p++) {
				memcpy(image, initial, sizeof(image));
				memset(&fs, 0, sizeof(fs));
				cut_at = 0;
				assert(lfs_mount(&fs, &cfg) == 0);
				mutations = 0;
				cut_at = op;
				fraction = p;
				if (!setjmp(power_cut)) {
					publish(&fs, new_record);
					assert(0 && "expected power cut");
				}
				cut_at = 0;
				check_catalogue();
				trials++;
			}
		}
	}
	printf("LittleFS catalogue publication: %u interrupted NOR operations passed (max %u mutations)\n",
	       trials, max_operations);
	return 0;
}
