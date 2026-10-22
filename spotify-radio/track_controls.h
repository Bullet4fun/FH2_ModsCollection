#ifndef FH2_TRACK_CONTROLS_H
#define FH2_TRACK_CONTROLS_H

#include "extra_options.h"

/* Track buttons for Spotify Radio, set in FH2_SpotifyRadio.ini. */

enum { TRACK_NONE, TRACK_NEXT, TRACK_PREVIOUS, TRACK_PLAY_PAUSE, TRACK_COMMANDS };

typedef struct {
    unsigned short buttons; // gamepad buttons pressed together (XInput bits); 0 = none
    unsigned char taps;     // 1, or 2 for a double tap
    unsigned char key;      // keyboard virtual key; 0 = none
} TrackBinding;

// "LS", "LS LS" (double tap), "LS+RB" (together), "F6"; empty = off. 0: not understood.
int track_binding_parse(const char *text, TrackBinding *binding);

// Reads <folder>FH2_SpotifyRadio.ini, writing the default one first if there is none.
void track_controls_load(const wchar_t *folder);

// One poll: the buttons and keys held now. Returns the TRACK_* command to send, or TRACK_NONE.
int track_controls_step(unsigned short buttons, const unsigned char *keys_down, unsigned long long now);

extern TrackBinding track_bindings[TRACK_COMMANDS][2]; // [command][0: gamepad, 1: keyboard]

#endif
