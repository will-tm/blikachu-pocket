/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_SESSION_H
#define BLIKACHU_LIBRETRO_SESSION_H

#include "core.h"

struct lr_callbacks {
	retro_video_refresh_t video;
	retro_audio_sample_t audio;
	retro_audio_sample_batch_t audio_batch;
	retro_input_poll_t input_poll;
	retro_input_state_t input_state;
};

/* One session owns the core workspace. All operations run on its worker. */
struct lr_session {
	const struct lr_core *core;
	struct retro_system_av_info av;
	enum emu_system system;
	bool initialized;
	bool loaded;
};

#ifdef __cplusplus
extern "C" {
#endif
int lr_session_open(struct lr_session *session, const struct emu_content_info *content,
		    const char *path, const struct lr_callbacks *callbacks);
int lr_session_run(struct lr_session *session);
int lr_session_reset(struct lr_session *session);
int lr_session_close(struct lr_session *session);
/* Scratch is valid only at a frame boundary and until the next run. */
void *lr_session_state_acquire(struct lr_session *session, size_t *size);
void lr_session_state_release(struct lr_session *session, void *buffer);
int lr_session_state_transfer(struct lr_session *session, void *buffer, size_t size, bool restore);
#ifdef __cplusplus
}
#endif
#endif
