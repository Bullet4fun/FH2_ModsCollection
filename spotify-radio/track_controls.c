#include "track_controls.h"
#include "logging.h"
#include "support.h"

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
    "; Spotify Radio track buttons. They work only while Spotify Radio is playing in the car.\r\n"
    ";\r\n"
    "; Gamepad: A B X Y LB RB LS RS View Menu Up Down Left Right\r\n"
    ";   LS       tap the left stick\r\n"
    ";   LS LS    tap it twice\r\n"
    ";   LS+RB    press them together (the game sees both buttons too)\r\n"
    "; Keyboard: F1-F12, A-Z, 0-9\r\n"
    "; Leave a line empty to turn it off. Extra Options uses the D-pad and RS: pick other buttons.\r\n"
    "\r\n"
    "[Controls]\r\n"
    "NextTrack=LS\r\n"
    "PreviousTrack=LS LS\r\n"
    "PlayPause=\r\n"
    "NextTrackKey=F6\r\n"
    "PreviousTrackKey=F5\r\n"
    "PlayPauseKey=F7\r\n"
    "; How fast a double tap must be, in milliseconds (100-600). A single tap waits this long.\r\n"
    "DoubleTapTime=200\r\n";

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
    char *line = ini_text;
    while (*line) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '[') {
            in_controls = equal_n(line, text_length(line), "[Controls]");
        } else if (in_controls && *line && *line != ';') {
            char *eq = line;
            while (*eq && *eq != '=') ++eq;
            if (*eq && equal_n(line, (size_t)(eq - line), "DoubleTapTime")) {
                unsigned ms = 0;
                for (const char *d = eq + 1; *d >= '0' && *d <= '9'; ++d) ms = ms * 10 + (unsigned)(*d - '0');
                if (ms >= 100 && ms <= 600) {
                    double_tap_ms = ms;
                } else {
                    log_append("SPOTIFY: FH2_SpotifyRadio.ini: DoubleTapTime must be 100-600; 200 is used.\r\n");
                }
            } else if (*eq) {
                char *key_end = eq;
                while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
                for (unsigned c = 1; c < TRACK_COMMANDS; ++c) {
                    for (unsigned k = 0; k < 2; ++k) {
                        if (!equal_n(line, (size_t)(key_end - line), ini_keys[c][k])) {
                            continue;
                        }
                        TrackBinding binding;
                        if (!track_binding_parse(eq + 1, &binding) || (k == 0 && binding.key) ||
                            (k == 1 && binding.buttons)) {
                            log_append("SPOTIFY: FH2_SpotifyRadio.ini: ");
                            log_append(ini_keys[c][k]);
                            log_append(" not understood; it stays off.\r\n");
                            binding = (TrackBinding){0};
                        }
                        track_bindings[c][k] = binding;
                        if (binding.buttons & (0x000f | 0x0080)) {
                            log_append("SPOTIFY: ");
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
}

void track_controls_load(const wchar_t *folder) {
    static wchar_t path[PATH_CAPACITY];
    for (unsigned c = 0; c < TRACK_COMMANDS; ++c) {
        track_bindings[c][0] = track_bindings[c][1] = (TrackBinding){0};
        keys_were_down[c] = 0;
    }
    double_tap_ms = 200;
    if (!path_join(path, folder, L"FH2_SpotifyRadio.ini")) {
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
