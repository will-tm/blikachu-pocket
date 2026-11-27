/*
 * Copyright (c) 2026 Camille BAUD
 * SPDX-License-Identifier: Apache-2.0
 * Environment callbacks adapted from the supplied Zephyr libretro frontend.
 */
#include "environment.h"
#include "memory.h"
#include "vfs.h"
#include "../storage.h"
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lr_environment, LOG_LEVEL_INF);

static const struct {
	const char *key;
	const char *value;
} overrides[] = {
	{ "fceumm_sndquality", "Low" },
	{ "fceumm_sndrate_hint", "32KHz" },
	{ "fceumm_overscan", "disabled" },
	{ "fceumm_overscan_v_top", "0" },
	{ "fceumm_overscan_v_bottom", "0" },
	{ "fceumm_ntsc_filter", "disabled" },
	{ "fceumm_hdpacks", "disabled" },
};

static struct {
	struct retro_game_info_ext content;
	struct retro_system_av_info av;
	struct retro_variable *options;
	size_t option_count;
	int error;
	const void *data;
	size_t size;
	enum retro_pixel_format pixel_format;
	bool shutdown;
	bool video_enabled;
} environment;

static void core_log(enum retro_log_level level, const char *format, ...)
{
	char line[192];
	va_list args;

	va_start(args, format);
	vsnprintk(line, sizeof(line), format, args);
	va_end(args);
	size_t n = strlen(line);

	while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
		line[--n] = 0;
	}
	if (!n) {
		return;
	}
	if (level >= RETRO_LOG_ERROR) {
		LOG_ERR("%s", line);
	} else if (level >= RETRO_LOG_WARN) {
		LOG_WRN("%s", line);
	} else if (level >= RETRO_LOG_INFO) {
		LOG_INF("%s", line);
	} else {
		LOG_DBG("%s", line);
	}
}

int lr_environment_begin(const struct retro_game_info *game, bool need_fullpath)
{
	memset(&environment, 0, sizeof(environment));
	environment.pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
	environment.video_enabled = true;
	environment.data = game->data;
	environment.size = game->size;
	char *path = lr_strdup(game->path ? game->path : "");

	if (!path) {
		return -ENOMEM;
	}
	const char *base = strrchr(path, '/');
	const char *name = base ? base + 1 : path;
	const char *dot = strrchr(name, '.');

	environment.content.full_path = path;
	environment.content.dir = lr_strndup(path, base ? (size_t)(base - path) : 0);
	environment.content.name = lr_strndup(name, dot ? (size_t)(dot - name) : strlen(name));
	environment.content.ext = dot ? dot + 1 : "";
	environment.content.data = need_fullpath ? NULL : game->data;
	environment.content.size = need_fullpath ? 0 : game->size;
	environment.content.persistent_data = true;
	return environment.content.dir && environment.content.name ? 0 : -ENOMEM;
}

static const char *option_value(const char *key)
{
	for (size_t i = 0; i < ARRAY_SIZE(overrides); ++i) {
		if (!strcmp(key, overrides[i].key)) {
			return overrides[i].value;
		}
	}
	for (size_t i = 0; i < environment.option_count; ++i) {
		if (!strcmp(key, environment.options[i].key)) {
			return environment.options[i].value;
		}
	}
	return NULL;
}

static bool reserve_options(size_t count)
{
	for (size_t i = 0; i < environment.option_count; ++i) {
		lr_free((void *)environment.options[i].key);
		lr_free((void *)environment.options[i].value);
	}
	lr_free(environment.options);
	environment.options = NULL;
	environment.option_count = 0;
	if (!count) {
		return true;
	}
	environment.options = lr_calloc(count, sizeof(*environment.options));
	if (!environment.options) {
		environment.error = -ENOMEM;
		return false;
	}
	return true;
}

/* Retain owned keys and selected defaults; no options/category menu is exposed. */
static bool save_option(const char *key, const char *value, size_t length)
{
	if (!value) {
		environment.error = -EINVAL;
		return false;
	}
	char *key_copy = lr_strdup(key);
	char *value_copy = lr_strndup(value, length);

	if (!key_copy || !value_copy) {
		lr_free(key_copy);
		lr_free(value_copy);
		environment.error = -ENOMEM;
		return false;
	}
	environment.options[environment.option_count++] =
		(struct retro_variable){ .key = key_copy, .value = value_copy };
	return true;
}

static bool options_v2(const struct retro_core_option_v2_definition *options)
{
	size_t count = 0;

	while (options && options[count].key) {
		++count;
	}
	if (!reserve_options(count)) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		const char *value = options[i].default_value ? options[i].default_value :
			options[i].values[0].value;

		if (!save_option(options[i].key, value, SIZE_MAX)) {
			return false;
		}
	}
	return true;
}

static bool options_v1(const struct retro_core_option_definition *options)
{
	size_t count = 0;

	while (options && options[count].key) {
		++count;
	}
	if (!reserve_options(count)) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		const char *value = options[i].default_value ? options[i].default_value :
			options[i].values[0].value;

		if (!save_option(options[i].key, value, SIZE_MAX)) {
			return false;
		}
	}
	return true;
}

static bool legacy_options(const struct retro_variable *options)
{
	size_t count = 0;

	while (options && options[count].key) {
		++count;
	}
	if (!reserve_options(count)) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		const char *start = options[i].value ? strstr(options[i].value, "; ") : NULL;

		if (!start) {
			environment.error = -EINVAL;
			return false;
		}
		start += 2;
		if (!save_option(options[i].key, start, strcspn(start, "|"))) {
			return false;
		}
	}
	return true;
}

static bool extension_matches(const char *list, const char *extension)
{
	size_t n = strlen(extension);

	while (*list) {
		size_t length = strcspn(list, "|");

		if (length == n && !strncasecmp(list, extension, n)) {
			return true;
		}
		list += length;
		if (*list) {
			++list;
		}
	}
	return false;
}

bool lr_environment(unsigned command, void *data)
{
	switch (command) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format format = *(enum retro_pixel_format *)data;

		if (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_0RGB1555) {
			return false;
		}
		environment.pixel_format = format;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = STORAGE_MOUNT_POINT "/.system";
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = STORAGE_MOUNT_POINT "/.saves";
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		(void)options_v2(data ? ((struct retro_core_options_v2 *)data)->definitions : NULL);
		return false; /* Defaults are registered, but categories are not exposed. */
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
		const struct retro_core_options_v2_intl *options = data;

		(void)options_v2(options && options->us ? options->us->definitions : NULL);
		return false;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
		return options_v1(data);
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
		return options_v1(data ? ((struct retro_core_options_intl *)data)->us : NULL);
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return legacy_options(data);
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *variable = data;

		variable->value = option_value(variable->key);
		return variable->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: {
		const struct retro_system_content_info_override *override = data;

		for (; override && override->extensions; ++override) {
			if (extension_matches(override->extensions, environment.content.ext)) {
				environment.content.data = override->need_fullpath ? NULL : environment.data;
				environment.content.size = override->need_fullpath ? 0 : environment.size;
			}
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
		*(const struct retro_game_info_ext **)data = &environment.content;
		return true;
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
		struct retro_vfs_interface_info *info = data;

		if (info->required_interface_version > 3) {
			return false;
		}
		info->required_interface_version = 3;
		info->iface = (struct retro_vfs_interface *)lr_vfs_interface();
		return true;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		environment.av = *(struct retro_system_av_info *)data;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		environment.av.geometry = *(struct retro_game_geometry *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*(int *)data = environment.video_enabled ? 3 : 2;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
		*(unsigned *)data = 1;
		return true;
	case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		*(unsigned *)data = 1;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		LOG_INF("%s", ((struct retro_message *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
		LOG_INF("%s", ((struct retro_message_ext *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		environment.shutdown = true;
		return true;
	default:
		return false;
	}
}

int lr_environment_error(void)
{
	return environment.error;
}

bool lr_environment_shutdown(void)
{
	return environment.shutdown;
}

void lr_environment_video_enable(bool enabled)
{
	environment.video_enabled = enabled;
}
enum retro_pixel_format lr_environment_pixel_format(void)
{
	return environment.pixel_format;
}
const struct retro_system_av_info *lr_environment_av_info(void)
{
	return &environment.av;
}
