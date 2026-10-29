#ifndef FH2_LINE_IN_CAPTURE_H
#define FH2_LINE_IN_CAPTURE_H

#include "extra_options.h"

/* Line-in Radio's audio sources, both 44.1 kHz 16-bit stereo into the bridge's pipe:
   - Source = Songs: the files in the mod's Songs\ folder (line_in_songs.c);
   - Source = Device: the sound device named in FH2_LineInRadio.ini, captured with WASAPI (an output
     device through loopback, an input device directly). */

extern char line_in_device[256]; // [Audio] Device: part of the device's name, UTF-8; empty = off
extern int line_in_use_songs;    // [Audio] Source: 1 = Songs (default), 0 = Device
extern int line_in_shuffle;      // [Audio] Shuffle: 1 = random order (default), 0 = by file name

// 1: case-insensitive (ASCII) substring match; an empty `wanted` matches nothing.
int line_in_name_matches(const char *name, const char *wanted);
// 1: linux_radio.py runs beside the game (Linux, Proton): it makes LINE_IN_LINUX_DEVICE and reads MPRIS.
int line_in_linux_helper(void);
#define LINE_IN_LINUX_DEVICE "Forza Horizon 2 Radio"

// Starts the source picked in the menu (source.txt), else the config's. Owns `pipe_write`; folder: the mod's.
int line_in_capture_start(HANDLE pipe_write, const wchar_t *folder);
void line_in_capture_stop(void);
// The in-game menu (line_in_menu.c): switch to a device (NULL: the Songs folder), set Shuffle, list devices.
void line_in_select(const char *device);
void line_in_set_shuffle(int on);
unsigned line_in_list_devices(wchar_t names[][128], unsigned capacity);
void line_in_state_parse(char *text); // source.txt's lines
void line_in_menu_install(unsigned char *game_image);
size_t line_in_menu_xaml(const char *text, size_t length, char *output, size_t capacity);

int line_in_songs_start(HANDLE pipe_write, const wchar_t *folder);
// How many songs the Songs folder in `folder` holds.
unsigned line_in_songs_count(const wchar_t *folder);
void line_in_songs_stop(void);
// A track button (TRACK_*) for the song player. 0: Songs is not playing, the caller handles it.
int line_in_songs_command(int command);
// One file into write(): 1 played (or skipped), 0 unreadable, -1 write() failed.
int line_in_songs_decode(const wchar_t *path, int (*write)(const void *data, DWORD size));

// nowplaying.txt in the mod's folder ("title\r\nartist\r\n", UTF-8), which the bridge shows on the HUD.
extern wchar_t line_in_now_playing_path[PATH_CAPACITY];
void line_in_now_playing_write(const char *text);

// Source = Device: the player on the device's render side (line_in_player.c).
extern wchar_t line_in_render_id[512];
void line_in_player_start(void);
void line_in_player_stop(void);
extern wchar_t line_in_command_path[PATH_CAPACITY]; // command.txt in the mod's folder
// A track button for that player's media session (under Proton: written to command.txt for
// linux_radio.py). 0: no player with a media session found.
int line_in_player_command(int command);
int line_in_app_matches(const wchar_t *app_id, const wchar_t *program);
unsigned line_in_session_program(const wchar_t *identifier, wchar_t out[64]);

#endif
