/* SPDX-License-Identifier: Apache-2.0 */
#include "session.h"
#include "environment.h"
#include "lifetime.h"
#include "memory.h"

#include <cerrno>
#include <cmath>
#include <new>

int lr_session_close(struct lr_session *session)
{
	if (!session->core) {
		return 0;
	}
	int rc = 0;
	try {
		if (session->loaded) {
			session->core->unload_game();
		}
	} catch (...) {
		rc = -EIO;
	}
	try {
		if (session->initialized) {
			session->core->deinit();
		}
	} catch (...) {
		rc = -EIO;
	}
	lr_lifetime_end();
	*session = {};
	return rc;
}

int lr_session_open(struct lr_session *session, const struct emu_content_info *content,
		    const char *path, const struct lr_callbacks *callbacks)
{
	if (session->core) {
		return -EBUSY;
	}
	const struct lr_core *core = lr_core_for_system(content->system);
	if (!core) {
		return -ENOTSUP;
	}
	int rc = lr_lifetime_begin(content->system);
	if (rc != 0) {
		return rc;
	}
	session->core = core;
	session->system = content->system;
	try {
		struct retro_system_info info = {};
		struct retro_game_info game = {path, content->data, content->size, nullptr};

		core->get_system_info(&info);
		rc = lr_environment_begin(&game, info.need_fullpath);
		if (rc == 0 && core->api_version() != RETRO_API_VERSION) {
			rc = -ENOTSUP;
		}
		if (rc == 0) {
			core->set_environment(lr_environment);
			core->set_video_refresh(callbacks->video);
			core->set_audio_sample(callbacks->audio);
			core->set_audio_sample_batch(callbacks->audio_batch);
			core->set_input_poll(callbacks->input_poll);
			core->set_input_state(callbacks->input_state);
			rc = lr_environment_error();
		}
		if (rc == 0) {
			core->init();
			session->initialized = true;
			rc = lr_environment_error();
		}
		if (rc == 0) {
			session->loaded = core->load_game(&game);
			rc = lr_environment_error();
			if (!rc && !session->loaded) {
				rc = -ENOEXEC;
			}
		}
		if (rc == 0) {
			core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
			core->get_system_av_info(&session->av);
			if (!std::isfinite(session->av.timing.fps) || session->av.timing.fps <= 0 ||
			    !std::isfinite(session->av.timing.sample_rate) || session->av.timing.sample_rate <= 0) {
				rc = -EINVAL;
			}
		}
	} catch (const std::bad_alloc &) {
		rc = -ENOMEM;
	} catch (...) {
		rc = -EIO;
	}
	if (rc != 0) {
		lr_session_close(session);
	}
	return rc;
}

int lr_session_run(struct lr_session *session)
{
	if (!session->loaded) {
		return -ENODEV;
	}
	try {
		session->core->run();
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
	int rc = lr_environment_error();
	return rc ? rc : (lr_environment_shutdown() ? -ECANCELED : 0);
}

int lr_session_reset(struct lr_session *session)
{
	if (!session->loaded) {
		return -ENODEV;
	}
	try {
		session->core->reset();
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
	return lr_environment_error();
}

extern "C" void *lr_snes9x2005_state_scratch(size_t size);

void *lr_session_state_acquire(struct lr_session *session, size_t *size)
{
	*size = 0;
	if (!session->loaded) {
		return nullptr;
	}
	try {
		*size = session->core->serialize_size();
	} catch (...) {
		return nullptr;
	}
	if (!*size) {
		return nullptr;
	}
	return session->system == EMU_SYSTEM_SNES ? lr_snes9x2005_state_scratch(*size) : lr_malloc(*size);
}

void lr_session_state_release(struct lr_session *session, void *buffer)
{
	if (session->system != EMU_SYSTEM_SNES) {
		lr_free(buffer);
	}
}

int lr_session_state_transfer(struct lr_session *session, void *buffer, size_t size, bool restore)
{
	try {
		bool ok = restore ? session->core->unserialize(buffer, size) : session->core->serialize(buffer, size);
		return ok ? 0 : -EIO;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}
