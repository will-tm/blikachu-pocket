/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_LIBRETRO_ENVIRONMENT_H
#define BLIKACHU_LIBRETRO_ENVIRONMENT_H
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
int lr_environment_begin(const struct retro_game_info *game, bool need_fullpath);
bool lr_environment(unsigned command, void *data);
bool lr_environment_shutdown(void);
void lr_environment_video_enable(bool enabled);
int lr_environment_error(void);
enum retro_pixel_format lr_environment_pixel_format(void);
const struct retro_system_av_info *lr_environment_av_info(void);
#ifdef __cplusplus
}
#endif
#endif
