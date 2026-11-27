/* SPDX-License-Identifier: Apache-2.0 */
#include "lifetime.h"
#include "memory.h"
#include "vfs.h"

#include <cerrno>
#include <cstdlib>
#include <new>

struct destructor_entry {
	void (*function)(void *);
	void *object;
};
static destructor_entry destructors[32];
static size_t destructor_count;
static bool destructor_overflow;
using constructor = void (*)();
extern "C" constructor __lr_gambatte_init_start[], __lr_gambatte_init_end[];
extern "C" int __real___cxa_atexit(void (*function)(void *), void *object, void *dso);

extern "C" int __wrap___cxa_atexit(void (*function)(void *), void *object, void *dso)
{
	if (!lr_memory_active() || !lr_memory_contains(object)) {
		return __real___cxa_atexit(function, object, dso);
	}
	if (destructor_count == sizeof(destructors) / sizeof(destructors[0])) {
		destructor_overflow = true;
		return -1;
	}
	destructors[destructor_count++] = {function, object};
	return 0;
}

void *operator new(size_t size)
{
	if (!size) {
		size = 1;
	}
	void *pointer = lr_memory_active() ? lr_malloc(size) : std::malloc(size);

	if (!pointer) {
		throw std::bad_alloc();
	}
	return pointer;
}

void *operator new[](size_t size)
{
	return ::operator new(size);
}

void operator delete(void *pointer) noexcept
{
	if (lr_memory_contains(pointer)) {
		lr_free(pointer);
	} else {
		std::free(pointer);
	}
}

void operator delete[](void *pointer) noexcept
{
	::operator delete(pointer);
}

void operator delete(void *pointer, size_t) noexcept
{
	::operator delete(pointer);
}

void operator delete[](void *pointer, size_t) noexcept
{
	::operator delete(pointer);
}

int lr_lifetime_begin(enum emu_system system)
{
	int rc = lr_memory_begin(system);

	if (rc != 0) {
		return rc;
	}
	destructor_count = 0;
	destructor_overflow = false;
	try {
		if (system == EMU_SYSTEM_GB || system == EMU_SYSTEM_GBC) {
			for (constructor *init = __lr_gambatte_init_start;
			     init < __lr_gambatte_init_end; ++init) {
				(*init)();
			}
		}
		if (!destructor_overflow) {
			return 0;
		}
		rc = -ENOMEM;
	} catch (const std::bad_alloc &) {
		rc = -ENOMEM;
	} catch (...) {
		rc = -EIO;
	}
	lr_lifetime_end();
	return rc;
}

void lr_lifetime_end(void)
{
	if (!lr_memory_active()) {
		return;
	}
	while (destructor_count) {
		destructor_entry entry = destructors[--destructor_count];

		entry.function(entry.object);
	}
	lr_vfs_close_all();
	lr_memory_end();
}
