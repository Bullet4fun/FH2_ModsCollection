#ifndef FH2_SPOTIFY_BRIDGE_H
#define FH2_SPOTIFY_BRIDGE_H

#include "extra_options.h"

void spotify_bridge_start(const wchar_t *folder);
void spotify_bridge_stop(void);
void spotify_bridge_apply_audio_hook(unsigned char *game_image);
void spotify_bridge_mount_logo_folder(unsigned char *game_image);
void spotify_bridge_install_controls(unsigned char *game_image);

#endif
