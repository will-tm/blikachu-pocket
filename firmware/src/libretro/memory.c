/* SPDX-License-Identifier: Apache-2.0 */
#include "memory.h"
#include "workspace.h"

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>

#define DECLARE_IMAGE(name) \
	extern uint8_t __lr_##name##_data_start[], __lr_##name##_data_end[]; \
	extern const uint8_t __lr_##name##_data_load[]; \
	extern uint8_t __lr_##name##_bss_start[], __lr_##name##_bss_end[]
DECLARE_IMAGE(common);
DECLARE_IMAGE(gambatte);
DECLARE_IMAGE(fceumm);
DECLARE_IMAGE(snes9x2005);
extern uint8_t __lr_workspace_end[];

struct memory_image {
	uint8_t *data_start;
	uint8_t *data_end;
	const uint8_t *data_load;
	uint8_t *bss_start;
	uint8_t *bss_end;
};
#define IMAGE(name) { __lr_##name##_data_start, __lr_##name##_data_end, \
	__lr_##name##_data_load, __lr_##name##_bss_start, __lr_##name##_bss_end }
static const struct memory_image common = IMAGE(common);
static const struct memory_image gambatte = IMAGE(gambatte);
static const struct memory_image fceumm = IMAGE(fceumm);
static const struct memory_image snes = IMAGE(snes9x2005);

static struct sys_heap heap;
static struct k_spinlock heap_lock;
static atomic_bool active;
static size_t capacity;

static void image_reset(const struct memory_image *image)
{
	size_t data_size = (uintptr_t)image->data_end - (uintptr_t)image->data_start;
	size_t bss_size = (uintptr_t)image->bss_end - (uintptr_t)image->bss_start;

	memcpy(image->data_start, image->data_load, data_size);
	memset(image->bss_start, 0, bss_size);
}

int lr_memory_begin(enum emu_system system)
{
	const struct memory_image *image;

	switch (system) {
	case EMU_SYSTEM_GB:
	case EMU_SYSTEM_GBC:
		image = &gambatte;
		break;
	case EMU_SYSTEM_NES:
		image = &fceumm;
		break;
	case EMU_SYSTEM_SNES:
		image = &snes;
		break;
	default:
		return -EINVAL;
	}
	if (!lr_workspace_claim(LR_WORKSPACE_CORE)) {
		return -EBUSY;
	}
	uintptr_t start = ROUND_UP((uintptr_t)image->bss_end, 32);
	uintptr_t end = (uintptr_t)__lr_workspace_end;

	if (start >= end || end - start < 1024) {
		lr_workspace_release(LR_WORKSPACE_CORE);
		return -ENOMEM;
	}
	image_reset(&common);
	image_reset(image);
	capacity = end - start;
	sys_heap_init(&heap, (void *)start, capacity);
	active = true;
	return 0;
}

void lr_memory_end(void)
{
	if (!active) {
		return;
	}
	active = false;
	capacity = 0;
	lr_workspace_release(LR_WORKSPACE_CORE);
}

bool lr_memory_active(void)
{
	return active;
}

bool lr_memory_contains(const void *pointer)
{
	uintptr_t p = (uintptr_t)pointer;
	uintptr_t base = (uintptr_t)lr_workspace_data();

	return p >= base && p - base < LR_WORKSPACE_BYTES;
}

void lr_memory_stats(size_t *total, size_t *used, size_t *peak)
{
	struct sys_memory_stats stats = {0};
	k_spinlock_key_t key = k_spin_lock(&heap_lock);

	if (active) {
		sys_heap_runtime_stats_get(&heap, &stats);
	}
	*total = capacity;
	*used = stats.allocated_bytes;
	*peak = stats.max_allocated_bytes;
	k_spin_unlock(&heap_lock, key);
}

void *lr_memalign(size_t alignment, size_t size)
{
	if (!active || !size || !is_power_of_two(alignment)) {
		return NULL;
	}
	k_spinlock_key_t key = k_spin_lock(&heap_lock);
	void *pointer = sys_heap_aligned_alloc(&heap, MAX(alignment, sizeof(void *)), size);

	k_spin_unlock(&heap_lock, key);
	return pointer;
}

void *lr_malloc(size_t size)
{
	return lr_memalign(_Alignof(max_align_t), size);
}

void lr_free(void *pointer)
{
	if (!pointer) {
		return;
	}
	__ASSERT_NO_MSG(active && lr_memory_contains(pointer));
	k_spinlock_key_t key = k_spin_lock(&heap_lock);

	sys_heap_free(&heap, pointer);
	k_spin_unlock(&heap_lock, key);
}

void *lr_calloc(size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size) {
		return NULL;
	}
	void *pointer = lr_malloc(count * size);

	if (pointer) {
		memset(pointer, 0, count * size);
	}
	return pointer;
}

char *lr_strndup(const char *text, size_t limit)
{
	size_t length = strnlen(text, limit);

	if (length == SIZE_MAX) {
		return NULL;
	}
	char *copy = lr_malloc(length + 1);

	if (copy) {
		memcpy(copy, text, length);
		copy[length] = '\0';
	}
	return copy;
}

char *lr_strdup(const char *text)
{
	return lr_strndup(text, SIZE_MAX);
}

void *lr_realloc(void *pointer, size_t size)
{
	if (!active) {
		return NULL;
	}
	__ASSERT_NO_MSG(!pointer || lr_memory_contains(pointer));
	k_spinlock_key_t key = k_spin_lock(&heap_lock);
	void *replacement = sys_heap_aligned_realloc(&heap, pointer, _Alignof(max_align_t), size);

	k_spin_unlock(&heap_lock, key);
	return replacement;
}

void *lr_reallocarray(void *pointer, size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size) {
		return NULL;
	}
	return lr_realloc(pointer, count * size);
}

void *lr_aligned_alloc(size_t alignment, size_t size)
{
	if (!alignment || size % alignment) {
		return NULL;
	}
	return lr_memalign(alignment, size);
}

int lr_posix_memalign(void **pointer, size_t alignment, size_t size)
{
	if (!pointer || alignment < sizeof(void *) || !is_power_of_two(alignment)) {
		return EINVAL;
	}
	void *result = lr_memalign(alignment, size);

	if (!result) {
		return ENOMEM;
	}
	*pointer = result;
	return 0;
}
