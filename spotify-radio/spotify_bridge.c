#include "spotify_bridge.h"
#include "track_controls.h"
#include "logging.h"
#include "support.h"
#include "pinyin_table.h"
#include "unidecode_table.h"

typedef int (*FmodPlaySoundFn)(void *system, void *sound, void *channelgroup, int paused, void **channel);
typedef int (*FmodSystemUpdateFn)(void *system);

#pragma pack(push, 2)
typedef struct {
    short left;
    short right;
} PcmFrame;
#pragma pack(pop)

// Live radio: the pipe worker drains librespot 24/7 into a ring; the Xbox Music station's endless
// FMOD stream reads it (xbox_stream_read_hook), so the game's pulls set the pace. Single producer /
// single consumer: the worker only advances g_pcm_written, the reader only g_pcm_read.
#define PCM_BUFFER_FRAMES (128 * 1024) // ~3s ring
#define PCM_TARGET_FRAMES (44100 / 2)  // ring margin: librespot goes quiet for up to ~0.6s (more after a pause)
// further behind (the game was not listening): jump to live
#define PCM_MAX_FRAMES (PCM_TARGET_FRAMES + 44100 * 3 / 2)
_Static_assert(PCM_MAX_FRAMES < PCM_BUFFER_FRAMES - 4096, "the ring must hold the backlog");
#define LISTEN_TIMEOUT_MS 1000         // no read for this long: the game stopped playing the station
#define PCM_FADE_IN_FRAMES 441         // 10ms fade-in after an underrun, so audio never starts mid-waveform
#define PCM_ENTER_FADE_FRAMES 17640    // 0.4s fade-in when the game starts listening again (station switch, un-pause)
static PcmFrame g_pcm_buffer[PCM_BUFFER_FRAMES];
static volatile ULONGLONG g_pcm_written = 0; // total frames ever written
static volatile ULONGLONG g_pcm_read = 0;    // total frames ever read
static volatile ULONGLONG g_last_pull = 0;   // GetTickCount64 of the last read: is the game listening?
static volatile ULONGLONG g_last_write = 0;  // GetTickCount64 of the last frames from librespot (gap reports)
static volatile ULONGLONG g_last_update = 0; // GetTickCount64 of the game's last FMOD update: is its audio thread stalled?

static HANDLE g_spotify_process = NULL;
static HANDLE g_spotify_job = NULL;
static HANDLE g_pipe_read_handle = NULL;
static HANDLE g_pipe_thread = NULL;
static HANDLE g_startup_thread = NULL;

static volatile int g_bridge_running = 0;

static unsigned char *g_game_image = NULL;
static FmodPlaySoundFn g_orig_playSound = NULL;
static FmodSystemUpdateFn g_orig_update = NULL;
static volatile LONG g_feed_busy = 0;

static void feed_lock(void) {
    while (__atomic_exchange_n(&g_feed_busy, 1, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
}

static void feed_unlock(void) { __atomic_store_n(&g_feed_busy, 0, __ATOMIC_RELEASE); }

/* Fills datalen bytes with ring audio (fades over any gap). Stream reader only, under feed_lock. */
static int pcm_read(void *data, unsigned int datalen) {
    if (!data || datalen == 0) {
        return 0;
    }
    PcmFrame *out = (PcmFrame *)data;
    unsigned int frames_needed = datalen / sizeof(PcmFrame);

    static int s_buffering = 1;
    static int s_fade_in = 0;   // frames of fade-in left
    static int s_fade_len = PCM_FADE_IN_FRAMES; // length of the current or next fade-in
    static PcmFrame s_tail;     // last frame written; through silence it decays to 0 instead of cutting (a click)
    static PcmFrame s_from;     // where a fade-in starts, so it never steps
    ULONGLONG now = GetTickCount64();
    __atomic_store_n(&g_last_pull, now, __ATOMIC_RELAXED);
    ULONGLONG written = __atomic_load_n(&g_pcm_written, __ATOMIC_ACQUIRE); // frames below it are complete
    ULONGLONG read = g_pcm_read;
    if (written - read > PCM_MAX_FRAMES) {
        // Back after the game stopped listening: catch up to live, keeping the margin banked.
        read = written - PCM_TARGET_FRAMES;
        s_buffering = 0;
        s_fade_in = s_fade_len = PCM_ENTER_FADE_FRAMES;
        s_from = s_tail;
    }
    if (!(s_buffering && written - read < PCM_TARGET_FRAMES)) {
        s_buffering = 0; // start-up or after an underrun: silence until the margin is banked again
    }
    static int s_underrun_reports = 0; // per session, so a bad run cannot flood the log
    int underran = 0;
    for (unsigned int f = 0; f < frames_needed; ++f) {
        if (!s_buffering && read < written) {
            PcmFrame p = g_pcm_buffer[read % PCM_BUFFER_FRAMES];
            ++read;
            if (s_fade_in) {
                // new audio rises over s_fade_len; the last frame sent fades out over 10ms, so no step
                int k = s_fade_len - s_fade_in--;
                int from = k < PCM_FADE_IN_FRAMES ? PCM_FADE_IN_FRAMES - k : 0;
                p.left = (short)(p.left * k / s_fade_len + s_from.left * from / PCM_FADE_IN_FRAMES);
                p.right = (short)(p.right * k / s_fade_len + s_from.right * from / PCM_FADE_IN_FRAMES);
                if (!s_fade_in) {
                    s_fade_len = PCM_FADE_IN_FRAMES; // later underruns get the short one again
                }
            }
            out[f] = s_tail = p;
        } else {
            // underrun (Spotify paused or loading a track): fade to silence (~15ms), then re-bank
            s_tail.left = (short)(s_tail.left * 127 / 128);
            s_tail.right = (short)(s_tail.right * 127 / 128);
            out[f] = s_tail;
            underran |= !s_buffering;
            s_buffering = 1;
            s_fade_in = s_fade_len; // short, unless an entry fade is still due
            s_from = s_tail;
        }
    }
    __atomic_store_n(&g_pcm_read, read, __ATOMIC_RELEASE);
    if (underran && s_underrun_reports < 20) {
        ++s_underrun_reports;
        log_append_value("SPOTIFY: audio gap; ms since " AUDIO_SOURCE " last sent audio ", (int)(now - g_last_write));
    }
    return 0;
}

/* While the game is listening, its pulls set the pace: drain librespot only to keep the ring
   PCM_TARGET_FRAMES ahead of the reader. Pacing by the PC clock instead drifted against the sound
   card's (FMOD pulled 43.3k-44.4k frames/s), so the lag grew until a jump to live every couple of
   minutes, and a librespot hiccup was never refilled. While the game's audio thread stalls, the
   stream plays its own buffer down; bank that too, or the feed after the stall underruns (seen: 0.5-0.6s
   stalls, each followed by a gap). Returns the byte count to have consumed. */
static ULONGLONG pipe_refill_due(ULONGLONG consumed, ULONGLONG written, ULONGLONG read, ULONGLONG stalled_ms) {
    ULONGLONG target = PCM_TARGET_FRAMES + stalled_ms * 44100 / 1000;
    ULONGLONG fill = written - read;
    return fill < target ? consumed + (target - fill) * sizeof(PcmFrame) : consumed;
}

static void xbox_restart_if_stopped(void);

static DWORD WINAPI spotify_pipe_worker(LPVOID param) {
    HANDLE hPipeRead = (HANDLE)param;
    static unsigned char s_pipe_temp[4096];
    const ULONGLONG bytes_per_sec = 44100 * sizeof(PcmFrame);
    size_t carry_len = 0;
    ULONGLONG clock_start = GetTickCount64();
    ULONGLONG clock_base = 0; // consumed when the game last stopped listening
    ULONGLONG consumed = 0;   // bytes taken from librespot since start
    while (g_bridge_running) {
        Sleep(5);
        xbox_restart_if_stopped();
        ULONGLONG now = GetTickCount64();
        ULONGLONG due;
        // signed: the feeder may store a tick newer than `now`. Listening while the game plays the channel.
        int listening = (LONGLONG)(now - __atomic_load_n(&g_last_pull, __ATOMIC_RELAXED)) < LISTEN_TIMEOUT_MS;
        if (listening) {
            // signed for the same reason; capped below the listen timeout
            LONGLONG stalled = (LONGLONG)(now - __atomic_load_n(&g_last_update, __ATOMIC_RELAXED));
            stalled = stalled < 0 ? 0 : stalled > LISTEN_TIMEOUT_MS ? LISTEN_TIMEOUT_MS : stalled;
            due = pipe_refill_due(consumed, g_pcm_written, __atomic_load_n(&g_pcm_read, __ATOMIC_ACQUIRE), (ULONGLONG)stalled);
            clock_start = now;
            clock_base = consumed;
        } else { // not listening: drain at 1x wall clock so librespot never stalls
            due = clock_base + (now - clock_start) * bytes_per_sec / 1000;
            if (due > consumed + bytes_per_sec / 10) {
                consumed = due - bytes_per_sec / 10; // Spotify was paused: don't drain the restart as a fast-forward
            }
        }
        while (g_bridge_running && consumed < due) {
            DWORD want = (DWORD)(sizeof(s_pipe_temp) - carry_len);
            if (due - consumed < want) {
                want = (DWORD)(due - consumed);
            }
            DWORD bytes_read = 0;
            if (!ReadFile(hPipeRead, s_pipe_temp + carry_len, want, &bytes_read, NULL) || bytes_read == 0) {
                break;
            }
            consumed += bytes_read;
            size_t total_bytes = carry_len + bytes_read;
            size_t num_frames = total_bytes / sizeof(PcmFrame);
            carry_len = total_bytes % sizeof(PcmFrame);

            PcmFrame *frames = (PcmFrame *)s_pipe_temp;
            ULONGLONG written = g_pcm_written;
            for (size_t i = 0; i < num_frames; ++i) {
                g_pcm_buffer[(written + i) % PCM_BUFFER_FRAMES] = frames[i];
            }
            __atomic_store_n(&g_pcm_written, written + num_frames, __ATOMIC_RELEASE); // after the frames themselves
            g_last_write = GetTickCount64();

            if (carry_len > 0) {
                unsigned char *src = s_pipe_temp + (total_bytes - carry_len);
                for (size_t c = 0; c < carry_len; ++c) {
                    s_pipe_temp[c] = src[c];
                }
            }
        }
    }
    return 0;
}

static void *volatile g_xbox_sound = NULL; // the Xbox Music stream (named by its read callback, which FMOD calls on creation)
static void *volatile g_xbox_channel = NULL; // the channel playing it
// Studio recreates the stream on every timeline rewind; the old one dies with audio it read ahead but
// never played. Frames read by the current stream and the last played position of its channel let
// the next stream start where the old one went silent instead of skipping that audio.
static int (*g_get_position)(void *channel, unsigned int *position, unsigned int unit) = NULL; // Channel::getPosition
static volatile ULONGLONG g_stream_frames = 0;
static volatile unsigned int g_played_frames = 0;
static void *volatile g_played_channel = NULL; // channel g_played_frames belongs to
#define FMOD_TIMEUNIT_PCM 2
static volatile int g_stream_replaced = 0; // the event destroyed its stream (callback 4)

/* System::update, on the game's FMOD thread: marks that its audio thread runs (a stall lets the pipe
   worker bank what the stream plays down meanwhile) and samples the stream's played position. */
static int fmod_update_hook(void *system) {
    int result = g_orig_update(system);
    __atomic_store_n(&g_last_update, GetTickCount64(), __ATOMIC_RELAXED);
    void *channel = g_xbox_channel;
    unsigned int position = 0;
    if (channel && g_get_position && g_get_position(channel, &position, FMOD_TIMEUNIT_PCM) == 0) {
        g_played_frames = position;
        g_played_channel = channel;
    }
    return result;
}

/* A new stream replaces the old: hand it the frames the old one read but never played, if the ring
   still holds them. Returns the frames given back. Under feed_lock. */
static unsigned int xbox_replay_unplayed(void) {
    ULONGLONG unplayed = 0;
    if (g_xbox_channel && g_played_channel == g_xbox_channel && g_played_frames <= g_stream_frames) {
        unplayed = g_stream_frames - g_played_frames;
    }
    g_xbox_channel = NULL; // the old channel died with its stream
    g_stream_frames = 0;
    ULONGLONG read = g_pcm_read;
    if (unplayed > 44100 || unplayed > read || g_pcm_written - (read - unplayed) > PCM_MAX_FRAMES) {
        return 0; // stale (a long stop): pcm_read catches up to live anyway
    }
    __atomic_store_n(&g_pcm_read, read - unplayed, __ATOMIC_RELEASE);
    return (unsigned int)unplayed;
}

// FMOD lookups for the radio bus route (exe exports; tests set fakes)
static int (*g_get_group)(void *channel, void **group) = NULL;     // Channel::getChannelGroup
static int (*g_get_parent)(void *group, void **parent) = NULL;     // ChannelGroup::getParentGroup
static int (*g_set_volume)(void *control, float volume) = NULL;    // ChannelControl::setVolume
static int (*g_get_dsp)(void *control, int index, void **dsp) = NULL;
static int (*g_get_num_dsps)(void *control, int *count) = NULL;
static int (*g_dsp_get_info)(void *dsp, char *name, unsigned int *version, int *channels, int *width, int *height) = NULL;
static int (*g_orig_set_position)(void *channel, unsigned int position, unsigned int unit) = NULL;

/* Channel::setPosition: Studio seeks a programmer sound when its event's timeline moves. Spotify
   has no position: the stream keeps reading the ring, whatever the timeline. */
static int set_position_hook(void *channel, unsigned int position, unsigned int unit) {
    if (channel && channel == g_xbox_channel) {
        return 0;
    }
    return g_orig_set_position(channel, position, unit);
}

/* The radio's sound comes from its bus (seen live: every station track's chain runs through a group
   with Stereo Transceiver + 3-EQ + Lowpass + Send, under the music bus). The transceiver feeds the
   festival speakers, the mixer snapshots (car meets, speed zones, driving) move its EQ and filters.
   The Xbox Music event is routed past it, straight into the music bus, and its top group plays only
   through its own Send (fader at 0). So its top group moves under the radio bus, its Send is bypassed
   and its fader raised: the same path as a station's track. Verified live: audible through the bus.
   Exports: ChannelGroup::getNumGroups 0x19a7050, getGroup 0x19a6c90, addGroup 0x19a6890,
   DSP::setBypass 0x19a9cf0. */
static int dsp_named(void *control, const char *wanted, void **found) {
    int count = 0;
    if (!g_get_num_dsps || !g_dsp_get_info || g_get_num_dsps(control, &count) != 0) {
        return 0;
    }
    for (int i = 0; i < count && i < 16; ++i) {
        void *dsp = NULL;
        char name[33];
        unsigned int version = 0;
        int channels = 0, width = 0, height = 0;
        name[0] = name[32] = '\0';
        if (g_get_dsp(control, i, &dsp) == 0 && dsp && g_dsp_get_info(dsp, name, &version, &channels, &width, &height) == 0 &&
            text_length(name) == text_length(wanted) && memory_equal(name, wanted, text_length(wanted))) {
            if (found) {
                *found = dsp;
            }
            return 1;
        }
    }
    return 0;
}

static void xbox_route_through_radio_bus(void *channel) {
    int (*num_groups)(void *, int *) = (int (*)(void *, int *))(g_game_image + 0x19a7050);
    int (*child_group)(void *, int, void **) = (int (*)(void *, int, void **))(g_game_image + 0x19a6c90);
    int (*add_group)(void *, void *, unsigned char, void *) = (int (*)(void *, void *, unsigned char, void *))(g_game_image + 0x19a6890);
    int (*set_bypass)(void *, unsigned char) = (int (*)(void *, unsigned char))(g_game_image + 0x19a9cf0);
    void *group = NULL;
    if (!g_get_group || !g_get_parent || g_get_group(channel, &group) != 0) {
        return;
    }
    // walk up to the event's top group: the one whose parent is the radio bus (moved before) or whose
    // parent holds the radio bus beside it (the music bus)
    for (int level = 0; group && level < 8; ++level) {
        void *parent = NULL, *send = NULL;
        if (g_get_parent(group, &parent) != 0 || !parent) {
            return;
        }
        void *bus = dsp_named(parent, "Stereo Transceiver", NULL) ? parent : NULL;
        int siblings = 0;
        for (int i = 0; !bus && num_groups(parent, &siblings) == 0 && i < siblings && i < 64; ++i) {
            void *sibling = NULL;
            if (child_group(parent, i, &sibling) == 0 && sibling && sibling != group && dsp_named(sibling, "Stereo Transceiver", NULL)) {
                bus = sibling;
            }
        }
        if (bus) {
            if (bus != parent) {
                add_group(bus, group, 1, NULL);
            }
            if (dsp_named(group, "FMOD Send", &send)) {
                set_bypass(send, 1);
            }
            g_set_volume(group, 1.0f);
            return;
        }
        group = parent;
    }
}

/* System::playSound: when Studio plays the Xbox Music stream, its event moves onto the radio bus. */
static int spotify_radio_play_sound_hook(void *system, void *sound, void *channelgroup, int paused, void **channel) {
    if (!sound || sound != g_xbox_sound) {
        return g_orig_playSound(system, sound, channelgroup, paused, channel);
    }
    void *local = NULL;
    void **out = channel ? channel : &local;
    int result = g_orig_playSound(system, sound, channelgroup, paused, out);
    if (result == 0 && *out) {
        g_xbox_channel = *out;
        xbox_route_through_radio_bus(*out);
    }
    return result;
}

// --- Spotify as the game's own "Xbox Music" station (reverse engineered, exe RVAs) ---
// The stock game has an Xbox Music station: RadioSystem::SetRadioStation (0x1021450) plays it for the
// name "Xbox Music" when RadioSystem::vtable[+0xc0] (0x1012090, the user's Xbox Music option, off
// since the service died) says it is available; the station list (0x1028540) then shows it too, with
// its own HUD icon (HUDRadioControl.xaml: HUD_RadioIcon_Xbox_Music). It plays one Studio event,
// event:/Master/Radio/XboxLiveStreaming (AudioObjectXboxStreamingMusicFmod), whose programmer sound
// is an endless user stream (0x1018fe0: createStream OPENUSER, length 0xffffffff) read by 0x1014960
// from the Xbox Music song queue. Here the gate says yes and the read callback plays Spotify: one
// channel for the whole drive, inside the game's own radio routing, no track changes at all.
#define RADIO_SYSTEM_VTABLE 0x2e76818
#define XBOX_MUSIC_AVAILABLE 0x1012090
#define XBOX_STREAM_READ 0x1014960

static int xbox_music_available_hook(void *radio_system) {
    (void)radio_system;
    return 1;
}

/* The profile's station lock (0xa65df0, "is it locked?": true when the name is not in the profile's
   unlocked list at +0x78, seen live; it takes the std::string by value and frees it). The stock game
   unlocks "Xbox Music" only from the Xbox Music intro popup, which needs the dead service: here it is
   never locked. Other names keep the game's answer. */
static int (*g_orig_station_locked)(void *profile, void *name) = NULL;
static int station_name_is(void *name, const char *text, unsigned long long length) {
    unsigned long long size = 0, capacity = 0;
    if (!name || !memory_read((unsigned char *)name + 0x10, &size, sizeof(size)) ||
        !memory_read((unsigned char *)name + 0x18, &capacity, sizeof(capacity)) || size != length) {
        return 0;
    }
    const char *chars = capacity > 15 ? *(const char **)name : (const char *)name;
    return memory_equal(chars, text, (size_t)length);
}

/* The mixer snapshot RADIO_Station (Media/Audio/AudioSnapshotInfo.xml, range -1..8) takes the current
   station's number; at 8, Xbox Music, the bank's mix fades the TreatedMusic bus (the festival speakers,
   fed by the radio bus's transceiver) to 0. Seen live: the bus at 0.71 on Ninja Tune, 0 on Xbox Music.
   The snapshot manager (AudioSnapshotManagerFmod, 0x1024570) copies each source's value (+0x14,
   parameter name std::string at +0x60) into its ParameterInstance every frame; this copy reports 8 as 6
   (Ninja Tune, a stock station) so Spotify gets the stock mix. Rewriting the source value instead raced
   the game's own writes and made the speakers blink (seen live). */
static int (*g_param_set_value)(void *instance, float value) = NULL; // ParameterInstance::setValue
static void snapshot_copy_hook(unsigned char *manager) {
    for (unsigned char *entry = *(unsigned char **)(manager + 0x48); entry != *(unsigned char **)(manager + 0x50); entry += 0x20) {
        void *instance = *(void **)(entry + 8);
        unsigned char *source = *(unsigned char **)(entry + 0x10);
        if (!instance) {
            *(unsigned int *)(source + 0x80) = 2;
            continue;
        }
        float value = *(float *)(source + 0x14);
        if (value == 8.0f && station_name_is(source + 0x60, "RADIO_Station", 13)) {
            value = 6.0f;
        }
        g_param_set_value(instance, value);
    }
}

static int station_locked_hook(void *profile, void *name) {
    int xbox = station_name_is(name, "Xbox Music", 10); // before the call: it frees name
    int locked = g_orig_station_locked(profile, name);
    return xbox ? 0 : locked;
}

/* RadioSystem's Xbox Music command (0x1018420, on its decoder at RadioSystem+0x90: 0 play, 1 stop) posts
   system event 0x22 to XboxMusicPlayer, which needs the dead service (the HUD's sign-in error). Instead
   the station's own event plays Spotify. The decoder's stream object is a stub at startup (the audio
   engine's +0x548 is 0, so AudioEngine[0x288] built AudioObjectXboxStreamingMusicCommon, no event):
   on first play the FMOD one is built the way that engine would (factory 0xfeeec0 on engine+0x7f0,
   then its Init, vtable[0], which creates event:/Master/Radio/XboxLiveStreaming) and swapped in. The
   stub is left allocated (a few hundred bytes, once). Verified live: Spotify played on Xbox Music only. */
#define XBOX_STREAM_VTABLE 0x2e76190
static void (*g_orig_xbox_command)(unsigned char *decoder, int command) = NULL;

/* The event's programmer sound lasts 600s (seen: it stopped by itself exactly 600s after a start, with
   the station still selected; the real service restarted the event for every song). Its own callback
   (0x1018fe0, set by Init through EventInstance::setCallback 0x1a9ab90) is wrapped: when it reports
   the event stopped (type 1, the last callback of that natural stop: 2, 4, 1) while Xbox Music is
   still selected, the pipe worker starts it again. The ring carries on, so the song does too, after a
   short gap. Types seen: 3 programmer sound created, 0 started, 4 destroyed, 5 on a restart. */
#define XBOX_EVENT_STOPPED 1
#define XBOX_SOUND_DESTROYED 4
static volatile int g_xbox_selected = 0;     // the game's last Xbox Music command was play
static void *volatile g_xbox_instance = NULL; // the station's event instance
static volatile int g_xbox_restart = 0;      // stopped by itself: the pipe worker starts it again
static volatile ULONGLONG g_xbox_started = 0; // tick of the last start: only a stop near 600s is the limit
#define XBOX_EVENT_LIMIT_MS 595000            // any other stop (the game's own) is left alone
static int (*g_orig_xbox_event_callback)(unsigned int type, void *event, void *parameters) = NULL;
static int xbox_event_callback(unsigned int type, void *event, void *parameters) {
    if (type == XBOX_SOUND_DESTROYED) {
        g_stream_replaced = 1; // the next stream may reuse the old one's address
    }
    if (type == XBOX_EVENT_STOPPED && g_xbox_selected && GetTickCount64() - g_xbox_started >= XBOX_EVENT_LIMIT_MS) {
        g_xbox_restart = 1;
    }
    return g_orig_xbox_event_callback(type, event, parameters);
}

/* The 600s are the timeline: the programmer instrument spans it. Once a second the pipe worker moves
   the timeline back by 240s once it is past 300s, still inside the instrument, so it never ends.
   Seen in a 30-minute drive: Studio recreates the programmer sound on each move (callbacks 4, 3), and
   the new stream reads on from the ring, so the song carries on with no audible gap. Any seek of the
   stream's channel is dropped (set_position_hook). The restart above stays as the fallback. Pipe worker only: Studio's API is thread-safe (unlike
   FMOD low level, which this never calls here). */
#define XBOX_REWIND_AFTER_MS 300000
#define XBOX_REWIND_BY_MS 240000
static void xbox_keep_timeline(void) {
    static ULONGLONG s_checked = 0;
    ULONGLONG now = GetTickCount64();
    if (!g_xbox_selected || !g_xbox_instance || !g_orig_set_position || now - s_checked < 1000) {
        return;
    }
    s_checked = now;
    int position = 0;
    if (((int (*)(void *, int *))(g_game_image + 0x1a98b10))(g_xbox_instance, &position) != 0 || // getTimelinePosition
        position < XBOX_REWIND_AFTER_MS) {
        return;
    }
    ((int (*)(void *, int))(g_game_image + 0x1a9b040))(g_xbox_instance, position - XBOX_REWIND_BY_MS); // setTimelinePosition
}

static void xbox_restart_if_stopped(void) {
    xbox_keep_timeline();
    if (!g_xbox_restart) {
        return;
    }
    g_xbox_restart = 0;
    if (g_xbox_selected && g_xbox_instance) {
        g_xbox_started = GetTickCount64();
        int res = ((int (*)(void *))(g_game_image + 0x1a9b390))(g_xbox_instance); // EventInstance::start
        log_append_value("SPOTIFY: Xbox Music event ran out (600s), started again; result ", res);
    }
}

static void *xbox_event(unsigned char *decoder) {
    void *object = NULL, *event = NULL;
    memory_read_pointer(decoder + 0x18, &object);
    if (!object || *(unsigned char **)object != g_game_image + XBOX_STREAM_VTABLE) {
        void *engine = NULL, *made[2] = {NULL, NULL};
        if (!memory_read_pointer(g_game_image + 0x365d3d8, &engine) || !engine) {
            return NULL;
        }
        ((void *(*)(void **, void *, void *, void *))(g_game_image + 0xfeeec0))(made, (unsigned char *)engine + 0x7f0, NULL, NULL);
        if (!made[0] || *(unsigned char **)made[0] != g_game_image + XBOX_STREAM_VTABLE) {
            log_append("SPOTIFY: Xbox Music stream object could not be built.\r\n");
            return NULL;
        }
        unsigned char ok = (*(unsigned char (**)(void *, void *))*(void **)made[0])(made[0], decoder);
        *(void **)(decoder + 0x18) = made[0];
        *(void **)(decoder + 0x20) = made[1];
        object = made[0];
        if (!ok) {
            log_append("SPOTIFY: Xbox Music event could not be created.\r\n");
        }
        void *instance = NULL;
        if (memory_read_pointer((unsigned char *)object + 0x30, &instance) && instance) {
            g_orig_xbox_event_callback = (int (*)(unsigned int, void *, void *))(g_game_image + 0x1018fe0);
            ((int (*)(void *, void *))(g_game_image + 0x1a9ab90))(instance, (void *)&xbox_event_callback);
        }
    }
    memory_read_pointer((unsigned char *)object + 0x30, &event);
    return event;
}

static void xbox_command_hook(unsigned char *decoder, int command) {
    if (command != 0 && command != 1) {
        g_orig_xbox_command(decoder, command);
        return;
    }
    g_xbox_selected = command == 0; // before stop: its own callback must not restart it
    g_xbox_started = GetTickCount64();
    void *event = xbox_event(decoder);
    g_xbox_instance = event;
    int res = -1;
    if (event) {
        res = command == 0 ? ((int (*)(void *))(g_game_image + 0x1a9b390))(event)          // EventInstance::start
                           : ((int (*)(void *, int))(g_game_image + 0x1a9b550))(event, 1); // stop, immediate
    }
    if (res != 0) {
        log_append_value(command == 0 ? "SPOTIFY: Xbox Music station could not start; result " : "SPOTIFY: Xbox Music station could not stop; result ", res);
    }
}

/* FMOD_SOUND_PCMREAD_CALLBACK of the endless stream: Spotify's audio, paced by the game's pulls. */
static int xbox_stream_read_hook(void *sound, void *data, unsigned int datalen) {
    feed_lock();
    if (sound != g_xbox_sound || g_stream_replaced) {
        g_stream_replaced = 0;
        xbox_replay_unplayed();
    }
    g_xbox_sound = sound; // FMOD reads a stream once on creation, before Studio plays it
    pcm_read(data, datalen);
    g_stream_frames += datalen / sizeof(PcmFrame);
    feed_unlock();
    return 0;
}

// 14-byte absolute jmp at target; copy_len (>= 14) whole, non-RIP-relative prologue bytes
// run from the returned trampoline. Returns NULL on failure. The trampoline is written, then made
// read+execute: no memory is ever writable and executable at once (what antivirus heuristics flag).
static void *install_jmp_hook(unsigned char *target, size_t copy_len, void *hook) {
    unsigned char *trampoline = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline) {
        return NULL;
    }
    static const unsigned char jmp_rip[6] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00}; // jmp qword ptr [rip + 0]
    memory_copy(trampoline, target, copy_len);
    memory_copy(trampoline + copy_len, jmp_rip, sizeof(jmp_rip));
    uintptr_t return_address = (uintptr_t)(target + copy_len);
    memory_copy(trampoline + copy_len + 6, &return_address, sizeof(return_address));
    DWORD old_protect;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

    if (!VirtualProtect(target, copy_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return NULL;
    }
    memory_copy(target, jmp_rip, sizeof(jmp_rip));
    uintptr_t hook_address = (uintptr_t)hook;
    memory_copy(target + 6, &hook_address, sizeof(hook_address));
    for (size_t i = 14; i < copy_len; ++i) {
        target[i] = 0x90;
    }
    VirtualProtect(target, copy_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, copy_len);
    return trampoline;
}

static wchar_t s_work_dir[PATH_CAPACITY];
// The HUD hook (game thread) reads the shown buffer every frame while the poll fills the other one and
// then flips: a half-written title never reaches the HUD. A buffer is refilled only a poll (~0.5s) later.
typedef struct {
    char title[256], artist[256];
} NowPlaying;
static NowPlaying g_np[2] = {{RADIO_TITLE, "Connected"}};
static volatile LONG g_np_shown = 0;

// nowplaying.txt (written by onevent.cmd): line 1 = title, further lines = artists.
// The HUD font has no glyphs beyond Latin (squares), and a font fallback override crashes the
// game's font manager, so romanize what can be done without a dictionary: Cyrillic, Hangul
// (Revised Romanization, no sound-change rules) and kana (Hepburn, shortest form: long-vowel
// mark dropped). Han characters get toneless Mandarin pinyin, one syllable each (Japanese
// kanji too: a reading-accurate Japanese conversion needs a word dictionary).
static int is_vowel(char c) {
    c = (char)(c | 0x20);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

static int is_letter(char c) {
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'z';
}

// Capitalize romanized Hangul/kana only at a word start (not after a letter or a passed-through kanji).
static int at_word_start(const char *dst, size_t n) {
    return n == 0 || (!is_letter(dst[n - 1]) && (unsigned char)dst[n - 1] < 0x80);
}

// The HUD draws one glyph per byte (a 3-byte 「 shows as three squares; the game's own track
// names are all ASCII), so everything not romanized above is reduced to ASCII here or dropped.
static const char *ascii_fallback(unsigned int cp, char *buffer) {
    static const char *const latin[192] = { // U+00C0..U+017F, diacritics stripped
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
        "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d",
        "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",
        "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i",
        "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",
        "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "Ng", "ng", "O", "o", "O", "o",
        "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",
        "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u",
        "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s"};
    if (cp >= 0xc0 && cp <= 0x17f) {
        return latin[cp - 0xc0];
    }
    if (cp >= 0xff01 && cp <= 0xff5e) { // full-width ASCII
        buffer[0] = (char)(cp - 0xfee0);
        buffer[1] = '\0';
        return buffer;
    }
    switch (cp) {
    case 0xa0: case 0x2022: case 0x30fb: case 0x3000: return " ";
    case 0x2018: case 0x2019: case 0x201a: case 0x2032: return "'";
    case 0x201c: case 0x201d: case 0x201e: case 0x2033:
    case 0x300c: case 0x300d: case 0x300e: case 0x300f: // 「」『』
    case 0x3008: case 0x3009: case 0x300a: case 0x300b: return "\""; // 〈〉《》
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: return "-";
    case 0x2026: return "...";
    case 0x3001: return ",";
    case 0x3002: return ".";
    case 0x3010: case 0x3014: case 0x3016: return "["; // 【〔〖
    case 0x3011: case 0x3015: case 0x3017: return "]";
    case 0x301c: return "~";
    }
    return NULL; // emoji, symbols, unmapped scripts: dropped
}

// Arabic/Persian, letter by letter. The script omits short vowels, so this is a consonant
// skeleton ("علي" -> "ali"); waw/yeh read as w/y after a vowel or at a word start, else u/i.
static const char *arabic_latin(unsigned int cp, const char *dst, size_t n) {
    static const char *const letters[42] = { // U+0621..U+064A; NULL = unassigned
        "", "a", "a", "u", "i", "i", "a", "b", "a", "t", "th", "j", "h", "kh", "d", "dh",
        "r", "z", "s", "sh", "s", "d", "t", "z", "a", "gh", NULL, NULL, NULL, NULL, NULL, "",
        "f", "q", "k", "l", "m", "n", "h", "w", "a", "y"};
    int after_vowel = n == 0 || !is_letter(dst[n - 1]) || is_vowel(dst[n - 1]);
    char prev = n > 0 ? (char)(dst[n - 1] | 0x20) : 0;
    // A vowel mark or long-vowel letter right after the same vowel adds nothing: عَلِيّ -> "ali"
    if (((cp == 0x64e || cp == 0x627) && prev == 'a') || ((cp == 0x650 || cp == 0x64a || cp == 0x6cc) && prev == 'i') ||
        ((cp == 0x64f || cp == 0x648) && prev == 'u')) {
        return "";
    }
    if (cp == 0x648) {
        return after_vowel ? "w" : "u"; // waw
    }
    if (cp == 0x64a || cp == 0x6cc) {
        return after_vowel ? "y" : "i"; // yeh, Farsi yeh
    }
    if (cp >= 0x621 && cp <= 0x64a) {
        return letters[cp - 0x621];
    }
    static const char *const digits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    if (cp >= 0x660 && cp <= 0x669) {
        return digits[cp - 0x660]; // Arabic-Indic digits
    }
    if (cp >= 0x6f0 && cp <= 0x6f9) {
        return digits[cp - 0x6f0]; // Persian digits
    }
    switch (cp) {
    case 0x64e: return "a"; // fatha
    case 0x64f: return "u"; // damma
    case 0x650: return "i"; // kasra
    case 0x64b: case 0x64c: case 0x64d: case 0x651: case 0x652: case 0x200c: return ""; // tanwin, shadda, sukun, ZWNJ
    case 0x671: return "a";  // alef wasla
    case 0x67e: return "p";  // Persian letters
    case 0x686: return "ch";
    case 0x698: return "zh";
    case 0x6a9: return "k";
    case 0x6af: return "g";
    case 0x6c0: return "e";
    case 0x60c: return ",";
    case 0x61b: return ";";
    case 0x61f: return "?";
    }
    return NULL;
}

// Modern Greek, accents dropped.
static const char *greek_latin(unsigned int cp, int *upper) {
    static const char *const letters[25] = { // alpha..omega (U+03B1..U+03C9, uppercase at -0x20)
        "a", "v", "g", "d", "e", "z", "i", "th", "i", "k", "l", "m", "n", "x", "o", "p",
        "r", "s", "s", "t", "y", "f", "ch", "ps", "o"};
    static const unsigned short accented[][2] = {
        {0x386, 0x391}, {0x388, 0x395}, {0x389, 0x397}, {0x38a, 0x399}, {0x38c, 0x39f}, {0x38e, 0x3a5},
        {0x38f, 0x3a9}, {0x3aa, 0x399}, {0x3ab, 0x3a5}, {0x390, 0x3b9}, {0x3ac, 0x3b1}, {0x3ad, 0x3b5},
        {0x3ae, 0x3b7}, {0x3af, 0x3b9}, {0x3b0, 0x3c5}, {0x3ca, 0x3b9}, {0x3cb, 0x3c5}, {0x3cc, 0x3bf},
        {0x3cd, 0x3c5}, {0x3ce, 0x3c9}};
    for (size_t i = 0; i < sizeof(accented) / sizeof(accented[0]); ++i) {
        if (cp == accented[i][0]) {
            cp = accented[i][1];
            break;
        }
    }
    *upper = cp >= 0x391 && cp <= 0x3a9;
    if (*upper && cp != 0x3a2) {
        cp += 0x20;
    }
    return (cp >= 0x3b1 && cp <= 0x3c9) ? letters[cp - 0x3b1] : NULL;
}

static void romanize_text(const char *src, char *dst, size_t capacity) {
    static const char *const cyrillic[32] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
                                             "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya"};
    static const char *const kana[86] = { // U+3041..U+3096
        "a", "a", "i", "i", "u", "u", "e", "e", "o", "o",
        "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
        "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
        "ta", "da", "chi", "ji", "", "tsu", "zu", "te", "de", "to", "do",
        "na", "ni", "nu", "ne", "no",
        "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
        "ma", "mi", "mu", "me", "mo",
        "ya", "ya", "yu", "yu", "yo", "yo",
        "ra", "ri", "ru", "re", "ro",
        "wa", "wa", "wi", "we", "o", "n", "vu", "ka", "ke"};
    static const char *const hangul_lead[19] = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
    static const char *const hangul_vowel[21] = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
    static const char *const hangul_tail[28] = {"", "k", "k", "k", "n", "n", "n", "t", "l", "k", "m", "l", "l", "l", "p", "l", "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t"};
    char syllable[16];
    size_t n = 0;
    int double_next = 0;
    int prev_han = 0;
    while (*src && n + 1 < capacity) {
        unsigned char b0 = (unsigned char)src[0];
        size_t len = b0 < 0x80 ? 1 : b0 >= 0xf0 ? 4 : b0 >= 0xe0 ? 3 : 2;
        unsigned int cp = len == 1 ? b0 : (b0 & (0xffu >> (len + 1)));
        for (size_t k = 1; k < len; ++k) {
            unsigned char b = (unsigned char)src[k];
            if ((b & 0xc0) != 0x80) {
                len = 1; // malformed: pass the byte through
                cp = 0;
                break;
            }
            cp = (cp << 6) | (b & 0x3fu);
        }
        const char *latin = pinyin_lookup(cp);
        int is_han = latin != NULL;
        if (n > 0 && n + 1 < capacity && is_letter(dst[n - 1]) &&
            (is_han || (prev_han && (cp >= 0x80 || is_letter((char)cp))))) {
            dst[n++] = ' '; // separate pinyin syllables from each other and from neighbouring words
        }
        prev_han = is_han;
        int upper = is_han;
        int fold = 0;
        unsigned int h = (cp >= 0x30a1 && cp <= 0x30f6) ? cp - 0x60 : cp; // katakana -> hiragana
        if (is_han) {
            // already set
        } else if (cp >= 0x410 && cp <= 0x44f) {
            upper = cp < 0x430;
            latin = cyrillic[cp - (upper ? 0x410 : 0x430)];
        } else if ((cp >= 0x400 && cp <= 0x40f) || (cp >= 0x450 && cp <= 0x45f)) {
            upper = cp < 0x450;
            switch (cp & 0x0f) {
            case 0x1: latin = "yo"; break; // Ё ё
            case 0x4: latin = "ye"; break; // Є є
            case 0x6: latin = "i"; break;  // І і
            case 0x7: latin = "yi"; break; // Ї ї
            case 0xb: latin = "c"; break;  // Ћ ћ (Serbian Latin c, not Unidecode's "tsh")
            case 0xe: latin = "u"; break;  // Ў ў
            default: latin = unidecode_lookup(cp); upper = 0; break; // Ђ Љ Њ Ћ Џ ...: case already in the table
            }
        } else if (cp == 0x490 || cp == 0x491) {
            upper = cp == 0x490;
            latin = "g"; // Ґ ґ
        } else if (cp >= 0xac00 && cp <= 0xd7a3) {
            unsigned int s = cp - 0xac00;
            size_t m = 0;
            const char *parts[3] = {hangul_lead[s / 588], hangul_vowel[(s % 588) / 28], hangul_tail[s % 28]};
            for (int p = 0; p < 3; ++p) {
                for (const char *q = parts[p]; *q; ++q) {
                    syllable[m++] = *q;
                }
            }
            syllable[m] = '\0';
            latin = syllable;
            upper = at_word_start(dst, n);
        } else if (h >= 0x3041 && h <= 0x3096) {
            if (h == 0x3063) { // small tsu: double the next consonant
                double_next = 1;
                src += len;
                continue;
            }
            latin = kana[h - 0x3041];
            int small_y = h == 0x3083 || h == 0x3085 || h == 0x3087;
            int small_vowel = h <= 0x304a && (h & 1);
            if (small_y && n >= 2 && (dst[n - 1] | 0x20) == 'i' && is_letter(dst[n - 2]) && !is_vowel(dst[n - 2])) {
                --n; // ki+ya -> kya
                if ((dst[n - 1] | 0x20) == 'h' || (dst[n - 1] | 0x20) == 'j') {
                    ++latin; // shi+ya -> sha, ji+ya -> ja
                }
            } else if (small_vowel && n >= 2 && is_vowel(dst[n - 1]) && is_letter(dst[n - 2]) && !is_vowel(dst[n - 2])) {
                --n; // fu+a -> fa, te+i -> ti
            } else {
                upper = at_word_start(dst, n);
            }
        } else if (cp == 0x30fc) {
            src += len; // long-vowel mark: shortest form drops it
            continue;
        } else if ((latin = arabic_latin(cp, dst, n)) != NULL) {
            upper = at_word_start(dst, n);
        } else if ((latin = greek_latin(cp, &upper)) != NULL) {
            // case comes from the Greek letter
        } else if (b0 >= 0x80) {
            latin = ascii_fallback(cp, syllable);
            if (!latin && (latin = unidecode_lookup(cp)) != NULL) {
                upper = at_word_start(dst, n); // every other script, letter by letter (Hebrew, Indic, Thai, ...)
                // Non-Latin letters: Unidecode's own capitals ("SH" for shin) are noise; case comes from word starts.
                fold = cp >= 0x370 && !(cp >= 0x2000 && cp < 0x3000);
            }
            if (!latin) {
                src += len;
                continue;
            }
        }
        if (!latin) {
            if (b0 < 0x80) {
                dst[n++] = (char)b0; // plain ASCII; anything unmapped is dropped so the HUD never gets a raw byte
            }
            src += len;
            double_next = 0;
            continue;
        }
        src += len;
        if (double_next && is_letter(latin[0]) && !is_vowel(latin[0]) && n + 1 < capacity) {
            dst[n++] = latin[0] == 'c' ? 't' : latin[0]; // tchi, not cchi
        }
        double_next = 0;
        for (size_t k = 0; latin[k] && n + 1 < capacity; ++k) {
            char c = latin[k];
            if (fold && c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            dst[n++] = (upper && k == 0 && c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
    }
    dst[n] = '\0';
}

// Cut to max_chars characters, ending in "..." when cut.
static void ellipsize(char *text, size_t max_chars) {
    size_t chars = 0, cut = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) {
            if (chars == max_chars - 3) {
                cut = i;
            }
            ++chars;
        }
    }
    if (chars <= max_chars) {
        return;
    }
    while (cut > 0 && text[cut - 1] == ' ') {
        --cut;
    }
    memory_copy(text + cut, "...", 4);
}

// onevent.cmd writes through `cmd /u` (UTF-16LE, no BOM): Wine's cmd has no UTF-8 code page.
// UTF-8 never holds a zero byte and the UTF-16 text always does (its CRLF), so a zero picks UTF-16.
static DWORD now_playing_to_utf8(const char *raw, DWORD size, char *text, DWORD capacity) {
    DWORD zero = 0;
    while (zero < size && raw[zero]) {
        ++zero;
    }
    if (zero == size) {
        DWORD n = size < capacity - 1 ? size : capacity - 1;
        memory_copy(text, raw, n);
        text[n] = '\0';
        return n;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)raw, (int)(size / 2), text, (int)capacity - 1, NULL, NULL);
    text[n > 0 ? n : 0] = '\0';
    return n > 0 ? (DWORD)n : 0;
}

// Polled from the startup thread only, hence static buffers (also keeps the frame under the stack-probe limit).
static void now_playing_poll(void) {
    static wchar_t s_np_path[PATH_CAPACITY];
    static char s_previous[512];
    static char raw[1024];
    static char text[512];
    static char title[256], artist[256];
    if (!s_np_path[0]) {
        path_join(s_np_path, s_work_dir, L"nowplaying.txt");
    }
    HANDLE file = CreateFileW(s_np_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD size = 0;
    int ok = ReadFile(file, raw, sizeof(raw), &size, NULL);
    CloseHandle(file);
    if (!ok || size == 0) {
        return;
    }
    size = now_playing_to_utf8(raw, size, text, sizeof(text));
    if (size == 0) {
        return;
    }
    if (memory_equal(text, s_previous, size + 1)) {
        return;
    }
    memory_copy(s_previous, text, size + 1);

    size_t t = 0, a = 0, i = 0;
    while (text[i] && text[i] != '\r' && text[i] != '\n' && t < sizeof(title) - 1) {
        title[t++] = text[i++];
    }
    while (text[i] && text[i++] != '\n') {
    }
    while (text[i] && a < sizeof(artist) - 3) {
        char c = text[i++];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (a > 0 && text[i] && text[i] != '\r') {
                artist[a++] = ',';
                artist[a++] = ' ';
            }
            continue;
        }
        artist[a++] = c;
    }
    if (t == 0) {
        return;
    }
    title[t] = '\0';
    artist[a] = '\0';
    LONG next = !__atomic_load_n(&g_np_shown, __ATOMIC_RELAXED);
    NowPlaying *np = &g_np[next];
    romanize_text(title, np->title, sizeof(np->title));
    romanize_text(artist, np->artist, sizeof(np->artist));
    size_t visible = 0;
    for (const char *c = np->title; *c; ++c) {
        visible += *c != ' ';
    }
    if (visible == 0) {
        memory_copy(np->title, RADIO_TITLE, sizeof(RADIO_TITLE)); // unsupported script: never a blank banner
    }
    ellipsize(np->title, 80); // the game's own longest title is 78 characters
    ellipsize(np->artist, 40);
    __atomic_store_n(&g_np_shown, next, __ATOMIC_RELEASE);
}

/* The radio HUD on the Xbox Music station (HudStatusRadioManager::OnSystemEvent, 0x7835b0). Event 0x10
   is "now playing": its handler copies the station, track (+0x08) and artist (+0x28, both
   std::string) into the manager, clears the error flag (+0x78) and pushes all to the HUD view model
   (0x78a070). Event 0x22 (XboxRadio) shows the dead service's sign-in error on Xbox Music. Here 0x22
   is dropped and, on Xbox Music, Spotify's track and artist replace the empty ones. */
static void (*g_orig_hud_radio_event)(unsigned char *manager, int category, void *code, void *payload) = NULL;

static void hud_radio_event_hook(unsigned char *manager, int category, void *code, void *payload) {
    if (category == 0x22) {
        return; // no Xbox Music service, no sign-in error
    }
    g_orig_hud_radio_event(manager, category, code, payload);
    const NowPlaying *np = &g_np[__atomic_load_n(&g_np_shown, __ATOMIC_ACQUIRE)];
    // 0x10 comes about every frame on Xbox Music (seen: 21k in a drive): push only a new track, or the
    // HUD restarts its banner each time and never shows it
    if (category != 0x10 || !station_name_is(manager + 0x48, "Xbox Music", 10) ||
        (station_name_is(manager + 0x08, np->title, text_length(np->title)) &&
         station_name_is(manager + 0x28, np->artist, text_length(np->artist)) && !manager[0x78])) {
        return;
    }
    void (*assign)(void *, const char *, size_t) = (void (*)(void *, const char *, size_t))(g_game_image + 0x22c300);
    assign(manager + 0x08, np->title, text_length(np->title));
    assign(manager + 0x28, np->artist, text_length(np->artist));
    manager[0x78] = 0;
    ((void (*)(void *))(g_game_image + 0x78a070))(manager);
}

/* The audio options' Xbox Music line (A_AudioOptions.xaml: Visibility bound to IsXboxMusicEnabled,
   the view model's +0x431, read by the property getter 0xc72eb0) appears with the station's gate but
   only picks Xbox Music playlists, which do not exist: the getter always sees it off. */
static void *(*g_orig_xbox_option_visible)(void *out, void *property, unsigned char *model, void *extra) = NULL;

static void *xbox_option_visible_hook(void *out, void *property, unsigned char *model, void *extra) {
    if (model) {
        model[0x431] = 0;
    }
    return g_orig_xbox_option_visible(out, property, model, extra);
}

/* The logo folder: game:\spotifyradio\ is mounted over game:\media\ on the game's first file lookup,
   the way the game mounts its own folders, below the mediaoverride and mediapc folders Extra Options
   mounts (a user's own override wins). A jmp hook on the lookup itself, so it works with or without
   Extra Options, which replaces the vtable slot that calls it. */
typedef struct {
    const char *data;
    uintptr_t reserved;
    size_t length, capacity;
} GameString;
typedef unsigned char (*ResolveFileFn)(void *, void **, void *, const void *, unsigned char, unsigned char,
                                       unsigned char, void **, unsigned char);
static ResolveFileFn g_orig_resolve = NULL;
static unsigned char *g_mount_image;
static unsigned char (*g_folder_file_exists)(void *, const GameString *) = NULL;
static void *g_folder_vtable[1 + 0x1f0 / sizeof(void *)];
static volatile LONG g_mount_state = 0;

static GameString game_string(const char *text) {
    return (GameString){text, 0, text_length(text), 31};
}

static unsigned char folder_file_exists_hook(void *volume, const GameString *path) {
    const char *text = path->capacity < 16 ? (const char *)path : path->data;
    for (size_t i = 0; i < path->length; ++i) { // archive selectors such as <*>0 are not filenames
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c == '<' || c == '>' || c == '*' || c == '?' || c == '"' || c == '|') {
            return 0;
        }
    }
    return g_folder_file_exists(volume, path);
}

static void mount_logo_folder(void *filesystem) {
    void *root;
    if (g_mount_state || !memory_read((unsigned char *)filesystem + 0x68, &root, sizeof(root)) || !root ||
        InterlockedCompareExchange(&g_mount_state, 1, 0) != 0) {
        return;
    }
    int mounted = 0;
    void *storage = ((void *(*)(size_t))(g_mount_image + 0x3229c0))(0x50);
    if (storage) {
        GameString source = game_string("game:\\" LOGO_FOLDER "\\");
        GameString destination = game_string("game:\\media\\");
        void *volume = ((void *(*)(void *, const void *, unsigned char))(g_mount_image + 0x11e05a0))(storage, &source, 1);
        *(void ***)volume = g_folder_vtable + 1;
        void **methods = *(void ***)volume;
        ((void (*)(void *))methods[0x10 / 8])(volume);
        void **filesystem_methods = *(void ***)filesystem;
        mounted = ((unsigned char (*)(void *, const void *, void **, unsigned, unsigned char, unsigned char,
                                      const void *))filesystem_methods[0x1e8 / 8])(filesystem, &destination, &volume,
                                                                                   0x7ffffffc, 0, 0, 0);
        ((void (*)(void *))methods[0x18 / 8])(volume);
        ((void (*)(void *))filesystem_methods[0x1f8 / 8])(filesystem);
    }
    InterlockedExchange(&g_mount_state, 2);
    log_append(mounted ? "SPOTIFY: logo folder mounted; Xbox Music shows the " LOGO_NAME " logo.\r\n"
                       : "SPOTIFY: logo folder mount failed; Xbox Music keeps its logo.\r\n");
}

static unsigned char resolve_file_hook(void *filesystem, void **volume, void *relative_path, const void *path,
                                       unsigned char flag1, unsigned char flag2, unsigned char flag3, void **mount,
                                       unsigned char flag4) {
    mount_logo_folder(filesystem);
    return g_orig_resolve(filesystem, volume, relative_path, path, flag1, flag2, flag3, mount, flag4);
}

void spotify_bridge_mount_logo_folder(unsigned char *game_image) {
    // mov rax,rsp / mov [rax+20h],r9 / mov [rax+10h],rdx / mov [rax+8],rcx = 15 bytes
    static const unsigned char resolve_prologue[15] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x48, 0x20, 0x48,
                                                       0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08};
    g_mount_image = game_image;
    memory_copy(g_folder_vtable, game_image + 0x2eab910, sizeof(g_folder_vtable)); // keeps the RTTI slot
    g_folder_file_exists = (unsigned char (*)(void *, const GameString *))g_folder_vtable[1 + 0xe8 / 8];
    g_folder_vtable[1 + 0xe8 / 8] = (void *)folder_file_exists_hook;
    if (memory_equal(game_image + 0x11f49b0, resolve_prologue, sizeof(resolve_prologue))) {
        g_orig_resolve = (ResolveFileFn)install_jmp_hook(game_image + 0x11f49b0, sizeof(resolve_prologue),
                                                         (void *)&resolve_file_hook);
    }
    log_append(g_orig_resolve ? "SPOTIFY: logo installed; its folder is mounted on the game's first file lookup.\r\n"
                              : "SPOTIFY: file lookup hook skipped (unexpected code); Xbox Music keeps its logo.\r\n");
}

void spotify_bridge_apply_audio_hook(unsigned char *game_image) {
    if (!game_image || g_orig_playSound) {
        return;
    }
    g_game_image = game_image;
    g_get_group = (int (*)(void *, void **))(game_image + 0x19abbd0);
    g_get_parent = (int (*)(void *, void **))(game_image + 0x19a7170);
    g_set_volume = (int (*)(void *, float))(game_image + 0x19a6470);
    g_get_position = (int (*)(void *, unsigned int *, unsigned int))(game_image + 0x19ac440);
    g_get_dsp = (int (*)(void *, int, void **))(game_image + 0x19a2860);
    g_get_num_dsps = (int (*)(void *, int *))(game_image + 0x19a3230);
    g_dsp_get_info = (int (*)(void *, char *, unsigned int *, int *, int *, int *))(game_image + 0x19a84c0);
    g_param_set_value = (int (*)(void *, float))(game_image + 0x1a9b1e0);
    // The gate is a vtable slot (the function starts RIP-relative), the read callback a jmp hook
    // that never calls back. Both or neither: checked before either is written.
    void **slot = (void **)(game_image + RADIO_SYSTEM_VTABLE + 0xc0);
    static const unsigned char read_prologue[14] = {0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x54,
                                                    0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec};
    DWORD protection = 0;
    int ok = *slot == (void *)(game_image + XBOX_MUSIC_AVAILABLE) &&
             memory_equal(game_image + XBOX_STREAM_READ, read_prologue, sizeof(read_prologue)) &&
             install_jmp_hook(game_image + XBOX_STREAM_READ, sizeof(read_prologue), (void *)&xbox_stream_read_hook) &&
             VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection);
    if (ok) {
        *slot = (void *)&xbox_music_available_hook;
        VirtualProtect(slot, sizeof(*slot), protection, &protection);
    }
    // the profile's station lock: mov [rsp+10h],rdx / push rdi / sub rsp,30h / mov qword [rsp+20h],-2
    static const unsigned char lock_prologue[19] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30,
                                                    0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};
    if (ok && memory_equal(game_image + 0xa65df0, lock_prologue, sizeof(lock_prologue))) {
        g_orig_station_locked = install_jmp_hook(game_image + 0xa65df0, sizeof(lock_prologue), (void *)&station_locked_hook);
    }
    // RadioSystem's Xbox Music command: mov [rsp+18h],rbx / push rsi / sub rsp,30h / cmp byte [rcx],0 / mov ebx,edx
    static const unsigned char command_prologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83,
                                                       0xec, 0x30, 0x80, 0x39, 0x00, 0x8b, 0xda};
    if (ok && memory_equal(game_image + 0x1018420, command_prologue, sizeof(command_prologue))) {
        g_orig_xbox_command = install_jmp_hook(game_image + 0x1018420, sizeof(command_prologue), (void *)&xbox_command_hook);
    }
    // HudStatusRadioManager::OnSystemEvent: mov [rsp+10h],rsi / push rdi / sub rsp,30h / mov rsi,r9 / mov rdi,rcx
    static const unsigned char hud_prologue[16] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                   0xec, 0x30, 0x49, 0x8b, 0xf1, 0x48, 0x8b, 0xf9};
    if (ok && memory_equal(game_image + 0x7835b0, hud_prologue, sizeof(hud_prologue))) {
        g_orig_hud_radio_event = install_jmp_hook(game_image + 0x7835b0, sizeof(hud_prologue), (void *)&hud_radio_event_hook);
    }
    // IsXboxMusicEnabled getter: mov [rsp+8],rcx / push rdi / sub rsp,40h / mov qword [rsp+30h],-2
    static const unsigned char option_prologue[19] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40,
                                                      0x48, 0xc7, 0x44, 0x24, 0x30, 0xfe, 0xff, 0xff, 0xff};
    if (ok && memory_equal(game_image + 0xc72eb0, option_prologue, sizeof(option_prologue))) {
        g_orig_xbox_option_visible = install_jmp_hook(game_image + 0xc72eb0, sizeof(option_prologue), (void *)&xbox_option_visible_hook);
    }
    // AudioSnapshotManagerFmod's copy: mov [rsp+8],rbx / push rdi / sub rsp,20h / mov rbx,[rcx+48h] / mov rdi,rcx
    static const unsigned char snapshot_prologue[17] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec,
                                                        0x20, 0x48, 0x8b, 0x59, 0x48, 0x48, 0x8b, 0xf9};
    void *snapshot_hooked = NULL;
    if (ok && memory_equal(game_image + 0x1024570, snapshot_prologue, sizeof(snapshot_prologue))) {
        snapshot_hooked = install_jmp_hook(game_image + 0x1024570, sizeof(snapshot_prologue), (void *)&snapshot_copy_hook);
    }
    // One line per hook, installed or not: a bug report shows at a glance what is in.
    log_append(snapshot_hooked ? "SPOTIFY: Xbox Music gets the stock radio mix (festival speakers on).\r\n"
                               : "SPOTIFY: snapshot hook skipped (unexpected code); no festival speakers on Xbox Music.\r\n");
    log_append(g_orig_xbox_option_visible ? "SPOTIFY: Xbox Music line hidden from the audio options.\r\n"
                                          : "SPOTIFY: audio options hook skipped (unexpected code); the Xbox Music line stays.\r\n");
    log_append(g_orig_hud_radio_event ? "SPOTIFY: radio HUD hook installed (Spotify's track on Xbox Music, no sign-in error).\r\n"
                                      : "SPOTIFY: radio HUD hook skipped (unexpected code).\r\n");
    log_append(g_orig_xbox_command ? "SPOTIFY: Xbox Music station plays Spotify's event (play/stop command hooked).\r\n"
                                   : "SPOTIFY: Xbox Music command hook skipped (unexpected code); the station stays silent.\r\n");
    log_append(g_orig_station_locked ? "SPOTIFY: Xbox Music station unlocked for the radio wheel.\r\n"
                                     : "SPOTIFY: station lock hook skipped (unexpected code).\r\n");
    log_append(ok ? "SPOTIFY: Xbox Music station enabled for Spotify (gate + stream read hooks).\r\n"
                  : "SPOTIFY: Xbox Music station hooks skipped (unexpected code); Spotify will not play.\r\n");
    // System::update: the feeder runs right after it, on the game's FMOD thread. Prologue:
    // push rdi / sub rsp,160h / mov qword [rsp+48h],-2 = 18 bytes, nothing RIP-relative.
    static const unsigned char update_prologue[18] = {0x40, 0x57, 0x48, 0x81, 0xec, 0x60, 0x01, 0x00, 0x00,
                                                      0x48, 0xc7, 0x44, 0x24, 0x48, 0xfe, 0xff, 0xff, 0xff};
    unsigned char *update_target = game_image + 0x199d510;
    if (memory_equal(update_target, update_prologue, sizeof(update_prologue))) {
        g_orig_update = (FmodSystemUpdateFn)install_jmp_hook(update_target, sizeof(update_prologue), (void *)&fmod_update_hook);
    }
    log_append(g_orig_update ? "SPOTIFY: FMOD update hook installed.\r\n"
                             : "SPOTIFY: FMOD update hook skipped (unexpected code); Spotify will not play.\r\n");

    // Channel::setPosition: push rbx/rbp/rsi/rdi/r14 / sub rsp,170h = 14 bytes
    static const unsigned char seek_prologue[14] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56,
                                                    0x48, 0x81, 0xec, 0x70, 0x01, 0x00, 0x00};
    if (memory_equal(game_image + 0x19acec0, seek_prologue, sizeof(seek_prologue))) {
        g_orig_set_position = install_jmp_hook(game_image + 0x19acec0, sizeof(seek_prologue), (void *)&set_position_hook);
    }
    log_append(g_orig_set_position ? "SPOTIFY: seek hook installed (Xbox Music plays past 600s).\r\n"
                                   : "SPOTIFY: seek hook skipped (unexpected code); Xbox Music restarts every 600s.\r\n");

    // System::playSound. Prologue: push rbx/rbp/rsi/rdi/r14/r15 / sub rsp,168h = 16 bytes
    static const unsigned char play_prologue[16] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41,
                                                    0x57, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00};
    unsigned char *play_target = game_image + 0x199b0f0;
    if (memory_equal(play_target, play_prologue, sizeof(play_prologue))) {
        g_orig_playSound = (FmodPlaySoundFn)install_jmp_hook(play_target, sizeof(play_prologue), (void *)&spotify_radio_play_sound_hook);
    }
    log_append(g_orig_playSound ? "SPOTIFY: FMOD radio hook installed.\r\n"
                                : "SPOTIFY: FMOD radio hook skipped (unexpected code); Spotify will not play.\r\n");
}

/* Track buttons (track_controls.c): polled with the now-playing check, sent to librespot's stdin
   as next/prev/playpause. Only while Xbox Music (Spotify Radio) plays and the game has the focus. */
#ifndef LINE_IN_RADIO
static HANDLE g_command_write = NULL;
#endif
static unsigned char *g_controls_image;
static SHORT(WINAPI *g_key_state)(int) = NULL;
static HWND(WINAPI *g_foreground_window)(void) = NULL;
static DWORD(WINAPI *g_window_process)(HWND, LPDWORD) = NULL;

static int game_focused(void) {
    if (!g_foreground_window || !g_window_process) {
        return 0;
    }
    DWORD process = 0;
    HWND window = g_foreground_window();
    if (window) {
        g_window_process(window, &process);
    }
    return process == GetCurrentProcessId();
}

// The buttons held on the game's active controller (the same read as Extra Options' autodrive).
static unsigned short game_buttons(void) {
    void *manager, *vtable, *controller;
    unsigned char connected;
    unsigned short buttons = 0;
    unsigned char *image = g_controls_image;
    if (!image || !memory_read_pointer(image + 0x365d7c8, &manager) || !memory_read_pointer(manager, &vtable) ||
        vtable != image + 0x2e7a2e8 || !memory_read_pointer((unsigned char *)manager + 0xab28, &controller)) {
        return 0;
    }
    uintptr_t first = (uintptr_t)manager + 0x10, selected = (uintptr_t)controller;
    if (selected < first || selected >= first + 8 * 0x1300 || (selected - first) % 0x1300 ||
        !memory_read((unsigned char *)controller + 4, &connected, 1) || connected != 1 ||
        !memory_read((unsigned char *)controller + 0xc0, &buttons, sizeof(buttons))) {
        return 0;
    }
    return buttons;
}

static void poll_track_controls(void) {
    static const char *const lines[TRACK_COMMANDS] = {NULL, "next\n", "prev\n", "playpause\n"};
    unsigned char keys[TRACK_COMMANDS] = {0};
    int active = g_xbox_selected && game_focused();
    unsigned short buttons = active ? game_buttons() : 0;
    for (unsigned c = 1; active && g_key_state && c < TRACK_COMMANDS; ++c) {
        unsigned char key = track_bindings[c][1].key;
        keys[c] = key && (g_key_state(key) & 0x8000) != 0;
    }
    int command = track_controls_step(buttons, keys, GetTickCount64());
#ifdef LINE_IN_RADIO
    // Songs: the mod's own player. A device: the media session of the player playing into it (no
    // synthetic key presses: a mod that types keys looks like a keylogger to antivirus engines).
    static const char *const names[TRACK_COMMANDS] = {"", "next", "previous", "play/pause"};
    static int reports = 0; // per session: what each button did, so a stray one can be traced
    const char *route = !command                          ? NULL
                        : line_in_songs_command(command)  ? " -> the Songs folder\r\n"
                        : line_in_player_command(command) ? " -> the player in the device\r\n"
                                                          : " -> nothing: no player with a media session found\r\n";
    if (route && reports < 50) {
        ++reports;
        log_append("LINE-IN: button ");
        log_append(names[command]);
        log_append(route);
    }
    (void)lines;
#else
    if (command && g_command_write) {
        DWORD written;
        WriteFile(g_command_write, lines[command], (DWORD)text_length(lines[command]), &written, NULL);
    }
#endif
}

void spotify_bridge_install_controls(unsigned char *game_image) {
    g_controls_image = game_image;
    g_key_state = GetAsyncKeyState;
    g_foreground_window = GetForegroundWindow;
    g_window_process = GetWindowThreadProcessId;
}

#ifndef LINE_IN_RADIO
static wchar_t s_exe_path[PATH_CAPACITY];
static wchar_t s_cmdline[512];
#endif

static DWORD WINAPI spotify_startup_thread(LPVOID param) {
    (void)param;
    // Allow the game to finish window creation and FMOD engine startup
    Sleep(1500);

    SECURITY_ATTRIBUTES sa;
    memory_copy(&sa, &(SECURITY_ATTRIBUTES){0}, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hPipeRead = NULL;
    HANDLE hPipeWrite = NULL;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 16 * 1024)) { // ~0.09s; the pipe is latency too
        log_append("SPOTIFY: failed to create anonymous stdout pipe.\r\n");
        return 0;
    }

    SetHandleInformation(hPipeRead, 1 /* HANDLE_FLAG_INHERIT */, 0);
#ifdef LINE_IN_RADIO
    if (line_in_capture_start(hPipeWrite, s_work_dir)) { // the source's thread owns the write end
        g_pipe_read_handle = hPipeRead;
        g_bridge_running = 1;
        g_pipe_thread = CreateThread(NULL, 0, spotify_pipe_worker, (LPVOID)hPipeRead, 0, NULL);
        if (g_pipe_thread) {
            SetThreadPriority(g_pipe_thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
        }
    } else {
        log_append("LINE-IN: the capture thread could not be started.\r\n");
        CloseHandle(hPipeRead);
    }
#else
    HANDLE hCommandRead = NULL; // librespot's stdin: next/prev/playpause from the track buttons
    if (CreatePipe(&hCommandRead, &g_command_write, &sa, 0)) {
        SetHandleInformation(g_command_write, 1 /* HANDLE_FLAG_INHERIT */, 0);
    }

    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si;
    memory_copy(&si, &(STARTUPINFOW){0}, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0x00000100; // STARTF_USESTDHANDLES; CREATE_NO_WINDOW below already means no console
    si.hStdOutput = hPipeWrite;
    static wchar_t log_path[PATH_CAPACITY];
    HANDLE hLog = INVALID_HANDLE_VALUE;
    static wchar_t previous_log[PATH_CAPACITY]; // keep last session's: disconnects get reported after a restart
    if (path_join(log_path, s_work_dir, L"librespot.log") &&
        path_join(previous_log, s_work_dir, L"librespot.previous.log")) {
        MoveFileExW(log_path, previous_log, MOVEFILE_REPLACE_EXISTING);
        hLog = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    }
    si.hStdError = (hLog != INVALID_HANDLE_VALUE) ? hLog : (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdInput = hCommandRead ? hCommandRead : (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;

    PROCESS_INFORMATION pi;
    memory_copy(&pi, &(PROCESS_INFORMATION){0}, sizeof(pi));

    static const wchar_t cmd[] = L"\"librespot.exe\" -n \"Forza Horizon 2 Radio\" -b 320 --backend pipe --system-cache . --onevent .\\onevent.cmd";
    for (size_t i = 0; i < sizeof(cmd) / sizeof(wchar_t); ++i) {
        s_cmdline[i] = cmd[i];
    }

    static wchar_t stale_np[PATH_CAPACITY];
    if (path_join(stale_np, s_work_dir, L"nowplaying.txt")) {
        DeleteFileW(stale_np); // last session's song must not show before Spotify connects
    }

    if (CreateProcessW(s_exe_path, s_cmdline, NULL, NULL, TRUE /* InheritHandles */, 0x08000000 | 0x00008000 /* CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS */, NULL, s_work_dir, &si, &pi)) {
        if (g_spotify_job) {
            AssignProcessToJobObject(g_spotify_job, pi.hProcess);
        }
        g_spotify_process = pi.hProcess;
        CloseHandle(pi.hThread);

        g_pipe_read_handle = hPipeRead;
        g_bridge_running = 1;
        g_pipe_thread = CreateThread(NULL, 0, spotify_pipe_worker, (LPVOID)hPipeRead, 0, NULL);
        if (g_pipe_thread) { // an audio thread: the game must not starve it while the ring drains
            SetThreadPriority(g_pipe_thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
        }
        log_append("SPOTIFY: librespot started.\r\n");
    } else {
        log_append("SPOTIFY: librespot could not be started.\r\n");
        CloseHandle(hPipeRead);
    }

    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog); // the child keeps its inherited copy
    }
    if (hNul && hNul != INVALID_HANDLE_VALUE) {
        CloseHandle(hNul);
    }
    CloseHandle(hPipeWrite);
    if (hCommandRead) {
        CloseHandle(hCommandRead); // the child keeps its copy
    }
#endif

    for (unsigned tick = 0; g_bridge_running; ++tick) {
        poll_track_controls();
        if (tick % 32 == 0) {
            now_playing_poll();
        }
        Sleep(15);
    }
    return 0;
}

/* folder: the mod's own folder (librespot.exe, onevent.cmd, Spotify login, nowplaying.txt). */
void spotify_bridge_start(const wchar_t *folder) {
    if (g_spotify_process != NULL || g_startup_thread != NULL) {
        return;
    }
    size_t n = 0;
    while (folder[n] && n < PATH_CAPACITY - 1) {
        s_work_dir[n] = folder[n];
        ++n;
    }
    s_work_dir[n] = 0;
#ifndef LINE_IN_RADIO
    if (!path_join(s_exe_path, s_work_dir, L"librespot.exe")) {
        log_append("SPOTIFY: mod folder path too long; bridge skipped.\r\n");
        return;
    }

    if (GetFileAttributesW(s_exe_path) == INVALID_FILE_ATTRIBUTES) {
        log_append("SPOTIFY: librespot.exe not found; Spotify Radio is off.\r\n");
        return;
    }

    // Job Object for clean process tree termination
    g_spotify_job = CreateJobObjectW(NULL, NULL);
    if (g_spotify_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memory_copy(&jeli, &(JOBOBJECT_EXTENDED_LIMIT_INFORMATION){0}, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = 0x00002000; // KILL_ON_JOB_CLOSE
        SetInformationJobObject(g_spotify_job, 9, &jeli, sizeof(jeli));
    }
#endif

    g_startup_thread = CreateThread(NULL, 0, spotify_startup_thread, NULL, 0, NULL);
}

void spotify_bridge_stop(void) {
    g_bridge_running = 0;
    if (g_startup_thread) {
        WaitForSingleObject(g_startup_thread, 1000);
        CloseHandle(g_startup_thread);
        g_startup_thread = NULL;
    }
    if (g_pipe_read_handle) {
        CloseHandle(g_pipe_read_handle);
        g_pipe_read_handle = NULL;
    }
#ifdef LINE_IN_RADIO
    line_in_capture_stop(); // after the read end closed: a write blocked on a full pipe fails now
#endif
    if (g_pipe_thread) {
        WaitForSingleObject(g_pipe_thread, 1000);
        CloseHandle(g_pipe_thread);
        g_pipe_thread = NULL;
    }
    if (g_spotify_process) {
        TerminateProcess(g_spotify_process, 0);
        CloseHandle(g_spotify_process);
        g_spotify_process = NULL;
    }
    if (g_spotify_job) {
        CloseHandle(g_spotify_job);
        g_spotify_job = NULL;
    }
    log_append("SPOTIFY: " AUDIO_SOURCE " stopped.\r\n");
}
