#ifndef FH2_SPOTIFY_BRIDGE_H
#define FH2_SPOTIFY_BRIDGE_H

#include "extra_options.h"

// Line-in Radio (line-in-radio/) is this mod built with LINE_IN_RADIO: a sound device or the Songs
// folder feeds the pipe instead of librespot; the station, logo, HUD and buttons are the same.
#ifdef LINE_IN_RADIO
#include "line_in_capture.h"
#define RADIO_TITLE "Line-in Radio"
#define AUDIO_SOURCE "the sound device"
#define RADIO_FILE "FH2_LineInRadio"   // the log's name
#define RADIO_TAG "LineInRadio"        // its lines' tag
#define RADIO_VERSION LINE_IN_VERSION
#define LOGO_FILE "HUD_RadioIcon_LineIn.swatchbin"
#define LOGO_FOLDER "lineinradio"      // mounted over the game's media folder for the logo
#define LOGO_NAME "Line-in Radio"
#else
#define RADIO_TITLE "Spotify Radio"
#define AUDIO_SOURCE "librespot"
#define RADIO_FILE "FH2_SpotifyRadio"
#define RADIO_TAG "SpotifyRadio"
#define RADIO_VERSION SPOTIFY_VERSION
#define LOGO_FILE "HUD_RadioIcon_Spotify.swatchbin"
#define LOGO_NAME "Spotify"
#define LOGO_FOLDER "spotifyradio"
#endif
#define RADIO_WIDE_(text) L##text
#define RADIO_WIDE(text) RADIO_WIDE_(text)

void spotify_bridge_start(const wchar_t *folder);
void spotify_bridge_stop(void);
void spotify_bridge_apply_audio_hook(unsigned char *game_image);
void spotify_bridge_mount_logo_folder(unsigned char *game_image);
void spotify_bridge_install_controls(unsigned char *game_image);

#endif
