/* SPDX-License-Identifier: Apache-2.0 */
#include "libretro/environment.h"
#include "libretro/memory.h"
#include "libretro/vfs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *allocated[256];
static bool fail_allocation;
void *lr_malloc(size_t size)
{
	if (fail_allocation) {
		return NULL;
	}
	void *pointer = malloc(size);

	assert(pointer);
	for (size_t i = 0; i < 256; ++i) {
		if (!allocated[i]) {
			allocated[i] = pointer;
			return pointer;
		}
	}
	abort();
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
char *lr_strndup(const char *text, size_t length)
{
	size_t n = strnlen(text, length);
	char *copy = lr_malloc(n + 1);

	if (copy) {
		memcpy(copy, text, n);
		copy[n] = 0;
	}
	return copy;
}
char *lr_strdup(const char *text)
{
	return lr_strndup(text, SIZE_MAX);
}
void lr_free(void *pointer)
{
	if (!pointer) {
		return;
	}
	for (size_t i = 0; i < 256; ++i) {
		if (allocated[i] == pointer) {
			allocated[i] = NULL;
			free(pointer);
			return;
		}
	}
	abort();
}
const struct retro_vfs_interface *lr_vfs_interface(void)
{
	static const struct retro_vfs_interface interface = {0};
	return &interface;
}
static const char *value(const char *key)
{
	struct retro_variable variable = {key, NULL};

	assert(lr_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable));
	return variable.value;
}

int main(void)
{
	uint8_t bytes[32] = {0};
	struct retro_game_info game = {"/NAND:/demo.NES", bytes, sizeof(bytes), NULL};
	assert(!lr_environment_begin(&game, true));
	int av_flags;
	assert(lr_environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_flags) && av_flags == 3);
	lr_environment_video_enable(false);
	assert(lr_environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_flags) && av_flags == 2);
	lr_environment_video_enable(true);
	const struct retro_game_info_ext *info;
	assert(lr_environment(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info));
	assert(!strcmp(info->dir, "/NAND:") && !strcmp(info->name, "demo"));
	assert(!strcmp(info->ext, "NES") && !info->data && !info->size);
	struct retro_system_content_info_override override[] = {
		{"snes|fds", false, true}, {NULL, false, false},
	};
	assert(lr_environment(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, override));
	assert(!info->data);
	override[0].extensions = "nes|unf";
	assert(lr_environment(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, override));
	assert(info->data == bytes && info->size == sizeof(bytes) && info->persistent_data);

	char key[] = "temporary";
	char selected[] = "chosen";
	struct retro_core_option_v2_definition definitions[2] = {0};
	definitions[0].key = key;
	definitions[0].values[0].value = selected;
	struct retro_core_options_v2 options = {NULL, definitions};
	assert(!lr_environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options));
	memset(key, 'x', sizeof(key) - 1);
	memset(selected, 'x', sizeof(selected) - 1);
	assert(!strcmp(value("temporary"), "chosen"));
	assert(!lr_environment_error());

	struct retro_variable legacy[] = {
		{"first", "First; red|blue"}, {"second", "Second; yes|no"}, {NULL, NULL},
	};
	assert(lr_environment(RETRO_ENVIRONMENT_SET_VARIABLES, legacy));
	const char *first = value("first");
	assert(!strcmp(first, "red"));
	assert(!strcmp(value("second"), "yes"));
	assert(!strcmp(first, "red"));
	assert(!strcmp(value("fceumm_sndquality"), "Low"));

	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	assert(lr_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format));
	assert(lr_environment_pixel_format() == format);
	format = RETRO_PIXEL_FORMAT_XRGB8888;
	assert(!lr_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format));
	struct retro_vfs_interface_info vfs = {4, NULL};
	assert(!lr_environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs));
	vfs.required_interface_version = 3;
	assert(lr_environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs));
	assert(vfs.iface == lr_vfs_interface());

	fail_allocation = true;
	assert(!lr_environment(RETRO_ENVIRONMENT_SET_VARIABLES, legacy));
	assert(lr_environment_error() == -ENOMEM);
	assert(lr_environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL));
	assert(lr_environment_shutdown());
	for (size_t i = 0; i < 256; ++i) {
		lr_free(allocated[i]);
	}
	puts("libretro environment: defaults, ownership, extension matching and OOM passed");
	return 0;
}
