#include "track_controls.h"
#include "logging.h"
#include "support.h"

#define WIDE_(text) L##text
#define WIDE(text) WIDE_(text)
#ifdef LINE_IN_RADIO // Line-in Radio's INI: these buttons plus [Audio] Device
#include "line_in_capture.h"
#define RADIO_INI "FH2_LineInRadio.ini"
#define RADIO_LOG "LINE-IN: "
#else
#define RADIO_INI "FH2_SpotifyRadio.ini"
#define RADIO_LOG "SPOTIFY: "
#endif

static unsigned double_tap_ms = 200; // [Controls] DoubleTapTime

TrackBinding track_bindings[TRACK_COMMANDS][2];

static const struct {
    const char *name;
    unsigned short bit;
} button_names[] = {
    {"Up", 0x0001},   {"Down", 0x0002}, {"Left", 0x0004}, {"Right", 0x0008}, {"Menu", 0x0010},
    {"View", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
    {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
};

static const char *const ini_keys[TRACK_COMMANDS][2] = {
    {NULL, NULL}, {"NextTrack", "NextTrackKey"}, {"PreviousTrack", "PreviousTrackKey"},
    {"PlayPause", "PlayPauseKey"},
};

static const char default_ini[] =
#ifdef LINE_IN_RADIO
    "// FH2 Line-in Radio by Bullet4fun: https://github.com/Bullet4fun/FH2_ModsCollection\r\n"
    "// Works with Azimuth 1.0.3 and the Azimuth Mod Loader; other Azimuth versions are untested.\r\n"
    "//\r\n"
    "// Read at game startup. A key left out keeps its default; an empty value turns a button off.\r\n"
    "// What the radio plays (the Songs folder beside this file, or a sound device such as a virtual\r\n"
    "// cable) and Shuffle are picked in the game: Settings > Audio, under Radio Volume.\r\n"
    "// The track buttons work only while Line-in Radio is playing in the car: with Songs they control\r\n"
    "// the mod's own player, with a device they go to the player playing into it.\r\n"
    "\r\n"
#else
    "// FH2 Spotify Radio by Bullet4fun: https://github.com/Bullet4fun/FH2_ModsCollection\r\n"
    "// Works with Azimuth 1.0.3 and the Azimuth Mod Loader; other Azimuth versions are untested.\r\n"
    "//\r\n"
    "// Read at game startup. A key left out keeps its default; an empty value turns a button off.\r\n"
    "// The track buttons work only while Spotify Radio is playing in the car.\r\n"
#endif
    "// Gamepad: A B X Y LB RB LS RS View Menu Up Down Left Right\r\n"
    "//   LS       tap the left stick\r\n"
    "//   LS LS    tap it twice\r\n"
    "//   LS+RB    press them together (the game sees both buttons too)\r\n"
    "// Keyboard: F1-F12, A-Z, 0-9\r\n"
    "// Extra Options uses the D-pad, RS, X, Y, View and F1-F4 by default (its [Bindings]): pick other buttons.\r\n"
    "\r\n"
    "[Controls]\r\n"
    "NextTrack = LS             // Next track, gamepad. (Default = LS)\r\n"
    "PreviousTrack = LS LS      // Previous track, gamepad. (Default = LS LS)\r\n"
    "PlayPause =                // Play/pause, gamepad. (Default = empty, off)\r\n"
    "NextTrackKey = F6          // Next track, keyboard. (Default = F6)\r\n"
    "PreviousTrackKey = F5      // Previous track, keyboard. (Default = F5)\r\n"
    "PlayPauseKey = F7          // Play/pause, keyboard. (Default = F7)\r\n"
    "DoubleTapTime = 200        // How fast a double tap must be, in milliseconds; a single tap waits this long. (Default = 200) (100/600)\r\n";

static int equal_n(const char *a, size_t length, const char *b) {
    size_t i = 0;
    for (; i < length && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) {
            return 0;
        }
    }
    return i == length && !b[i];
}

static unsigned char parse_key(const char *text, size_t length) {
    char c = text[0] >= 'a' && text[0] <= 'z' ? (char)(text[0] - 32) : text[0];
    if (length == 1 && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
        return (unsigned char)c; // VK_A..VK_Z and VK_0..VK_9 are the characters
    }
    if (length >= 2 && length <= 3 && c == 'F') {
        unsigned n = 0;
        for (size_t i = 1; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return 0;
            }
            n = n * 10 + (unsigned)(text[i] - '0');
        }
        return n >= 1 && n <= 12 ? (unsigned char)(0x6f + n) : 0; // VK_F1 = 0x70
    }
    return 0;
}

int track_binding_parse(const char *text, TrackBinding *binding) {
    *binding = (TrackBinding){0};
    unsigned short groups[3] = {0};
    unsigned group_count = 0;
    const char *p = text;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) {
        return 1; // off
    }
    while (*p) {
        if (group_count == 2) {
            return 0; // at most a double tap
        }
        unsigned short group = 0;
        for (;;) {
            const char *token = p;
            while (*p && *p != '+' && *p != ' ' && *p != '\t') ++p;
            size_t length = (size_t)(p - token);
            if (!length) {
                return 0;
            }
            unsigned short bit = 0;
            for (unsigned i = 0; i < ARRAY_COUNT(button_names); ++i) {
                if (equal_n(token, length, button_names[i].name)) {
                    bit = button_names[i].bit;
                }
            }
            if (!bit) {
                unsigned char key = parse_key(token, length);
                // a key stands alone: no combination, no double tap
                if (!key || group || group_count || *p) {
                    return 0;
                }
                binding->key = key;
                return 1;
            }
            group |= bit;
            if (*p != '+') {
                break;
            }
            ++p;
        }
        groups[group_count++] = group;
        while (*p == ' ' || *p == '\t') ++p;
    }
    if (group_count == 2 && groups[0] != groups[1]) {
        return 0; // a double tap repeats the same buttons
    }
    binding->buttons = groups[0];
    binding->taps = (unsigned char)group_count;
    return 1;
}

typedef struct {
    unsigned short mask;
    unsigned char down, presses, max_taps;
    unsigned long long last;
} PadState;

static PadState pads[TRACK_COMMANDS];
static unsigned pad_count;
static unsigned char keys_were_down[TRACK_COMMANDS];

static void setup_pads(void) {
    pad_count = 0;
    for (unsigned c = 1; c < TRACK_COMMANDS; ++c) {
        const TrackBinding *b = &track_bindings[c][0];
        if (!b->buttons) {
            continue;
        }
        unsigned i = 0;
        while (i < pad_count && pads[i].mask != b->buttons) ++i;
        if (i == pad_count) {
            pads[pad_count++] = (PadState){b->buttons, 0, 0, 0, 0};
        }
        if (b->taps > pads[i].max_taps) {
            pads[i].max_taps = b->taps;
        }
    }
}

static int command_for(unsigned short mask, unsigned taps) {
    for (unsigned c = 1; c < TRACK_COMMANDS; ++c) {
        if (track_bindings[c][0].buttons == mask && track_bindings[c][0].taps == taps) {
            return c;
        }
    }
    return TRACK_NONE;
}

int track_controls_step(unsigned short buttons, const unsigned char *keys_down, unsigned long long now) {
    int command = TRACK_NONE;
    for (unsigned c = 1; c < TRACK_COMMANDS; ++c) {
        unsigned char down = keys_down[c];
        if (down && !keys_were_down[c]) {
            command = c;
        }
        keys_were_down[c] = down;
    }
    for (unsigned i = 0; i < pad_count; ++i) {
        PadState *pad = &pads[i];
        int held = (buttons & pad->mask) == pad->mask;
        // LS held as part of LS+RB is not a tap of LS
        for (unsigned j = 0; j < pad_count; ++j) {
            if (j != i && (pads[j].mask & pad->mask) == pad->mask && (buttons & pads[j].mask) == pads[j].mask) {
                pad->presses = 0;
                pad->down = 1;
                held = -1;
            }
        }
        if (held < 0) {
            continue;
        }
        if (held && !pad->down) {
            pad->presses++;
            pad->last = now;
            if (pad->presses >= pad->max_taps) {
                command = command_for(pad->mask, pad->presses);
                pad->presses = 0;
            }
        } else if (!held && pad->presses && now - pad->last > double_tap_ms) {
            command = command_for(pad->mask, pad->presses);
            pad->presses = 0;
        }
        pad->down = (unsigned char)held;
    }
    return command;
}

static char ini_text[4096];

static void parse_ini(void) {
    int in_controls = 0;
#ifdef LINE_IN_RADIO
    int in_audio = 0, source_given = 0;
#endif
    char *line = ini_text;
    while (*line) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        char *cut = line; // "Key = value   // note": the note and the padding go
        while (*cut && *cut != ';' && !(cut[0] == '/' && cut[1] == '/')) ++cut;
        while (cut > line && (cut[-1] == ' ' || cut[-1] == '\t')) --cut;
        *cut = 0; // only this line's bytes: the buffer is parsed once
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '[') {
            in_controls = equal_n(line, text_length(line), "[Controls]");
#ifdef LINE_IN_RADIO
            in_audio = equal_n(line, text_length(line), "[Audio]");
        } else if (in_audio && *line) {
            char *eq = line;
            while (*eq && *eq != '=') ++eq;
            char *key_end = eq;
            while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            const char *value = eq + (*eq != 0);
            while (*value == ' ' || *value == '\t') ++value;
            size_t key = (size_t)(key_end - line), length = text_length(value);
            if (*eq && equal_n(line, key, "Device")) {
                size_t n = 0;
                for (; value[n] && n < sizeof(line_in_device) - 1; ++n) line_in_device[n] = value[n];
                line_in_device[n] = 0;
            } else if (*eq && equal_n(line, key, "Source")) {
                source_given = 1;
                line_in_use_songs = !equal_n(value, length, "Device");
                if (line_in_use_songs && !equal_n(value, length, "Songs")) {
                    log_append(RADIO_LOG RADIO_INI ": Source must be Songs or Device; Songs is used.\r\n");
                }
            } else if (*eq && equal_n(line, key, "Shuffle")) {
                line_in_shuffle = !equal_n(value, length, "0");
            }
#endif
        } else if (in_controls && *line && *line != ';') {
            char *eq = line;
            while (*eq && *eq != '=') ++eq;
            char *key_end = eq;
            while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            if (*eq && equal_n(line, (size_t)(key_end - line), "DoubleTapTime")) {
                unsigned ms = 0;
                const char *d = eq + 1;
                while (*d == ' ' || *d == '\t') ++d;
                for (; *d >= '0' && *d <= '9'; ++d) ms = ms * 10 + (unsigned)(*d - '0');
                if (ms >= 100 && ms <= 600) {
                    double_tap_ms = ms;
                } else {
                    log_append(RADIO_LOG RADIO_INI ": DoubleTapTime must be 100-600; 200 is used.\r\n");
                }
            } else if (*eq) {
                for (unsigned c = 1; c < TRACK_COMMANDS; ++c) {
                    for (unsigned k = 0; k < 2; ++k) {
                        if (!equal_n(line, (size_t)(key_end - line), ini_keys[c][k])) {
                            continue;
                        }
                        TrackBinding binding;
                        if (!track_binding_parse(eq + 1, &binding) || (k == 0 && binding.key) ||
                            (k == 1 && binding.buttons)) {
                            log_append(RADIO_LOG RADIO_INI ": ");
                            log_append(ini_keys[c][k]);
                            log_append(" not understood; it stays off.\r\n");
                            binding = (TrackBinding){0};
                        }
                        track_bindings[c][k] = binding;
                        if (binding.buttons & (0x000f | 0x0080)) {
                            log_append(RADIO_LOG);
                            log_append(ini_keys[c][k]);
                            log_append(" uses the D-pad or RS, which Extra Options also uses.\r\n");
                        }
                    }
                }
            }
        }
        *end = saved;
        line = *end ? end + 1 : end;
    }
#ifdef LINE_IN_RADIO
    if (!source_given && line_in_device[0]) {
        line_in_use_songs = 0; // a config from 0.1.0 (no Source yet): its Device is what it plays
    }
#endif
}

void track_controls_load(const wchar_t *folder) {
    static wchar_t path[PATH_CAPACITY];
    for (unsigned c = 0; c < TRACK_COMMANDS; ++c) {
        track_bindings[c][0] = track_bindings[c][1] = (TrackBinding){0};
        keys_were_down[c] = 0;
    }
    double_tap_ms = 200;
    if (!path_join(path, folder, WIDE(RADIO_INI))) {
        return;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, default_ini, sizeof(default_ini) - 1, &written, NULL);
            CloseHandle(file);
        }
        memory_copy(ini_text, default_ini, sizeof(default_ini));
    } else {
        DWORD got = 0;
        ReadFile(file, ini_text, sizeof(ini_text) - 1, &got, NULL);
        CloseHandle(file);
        ini_text[got] = 0;
    }
    parse_ini();
    setup_pads();
}
