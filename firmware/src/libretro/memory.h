/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_MEMORY_H
#define BLIKACHU_LIBRETRO_MEMORY_H

#include "content.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All core callbacks/consumers must be stopped before end(). Constructors
 * run after begin(), and registered destructors run before end().
 */
int lr_memory_begin(enum emu_system system);
void lr_memory_end(void);
bool lr_memory_active(void);
bool lr_memory_contains(const void *pointer);
void lr_memory_stats(size_t *capacity, size_t *used, size_t *peak);

void *lr_malloc(size_t size);
void *lr_calloc(size_t count, size_t size);
void *lr_realloc(void *pointer, size_t size);
void *lr_reallocarray(void *pointer, size_t count, size_t size);
void lr_free(void *pointer);
char *lr_strdup(const char *text);
char *lr_strndup(const char *text, size_t limit);
void *lr_memalign(size_t alignment, size_t size);
void *lr_aligned_alloc(size_t alignment, size_t size);
int lr_posix_memalign(void **pointer, size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif
#endif
