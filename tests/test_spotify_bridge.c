/* Spotify bridge: the Xbox Music stream's read callback, the station lock and the mixer snapshot
   copy (the ring itself: test_live_buffer). */
#include "spotify_bridge.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void log_append(const char *message) { (void)message; }
void log_append_value(const char *label, int value) { (void)label; (void)value; }

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* the game's lock check says "locked" for everything; names are MSVC std::strings (inline < 16) */
static int fake_station_locked(void *profile, void *name) { (void)profile; (void)name; return 1; }
static float param_values[4];
static int fake_param_set(void *instance, float value) { param_values[(uintptr_t)instance & 3] = value; return 0; }
static void *std_string(const char *text) {
    static unsigned char s[4][32];
    static int next;
    unsigned char *b = s[next++ & 3];
    size_t n = strlen(text);
    memset(b, 0, 32);
    unsigned long long size = n, capacity = 15;
    if (n > 15) { *(const char **)b = text; capacity = n; } else memcpy(b, text, n);
    memcpy(b + 0x10, &size, 8);
    memcpy(b + 0x18, &capacity, 8);
    return b;
}

int main(void) {
    /* nowplaying.txt: UTF-16LE from `cmd /u` (Wine and Windows) becomes UTF-8; plain UTF-8 passes through. */
    {
        static const char utf16[] = "\x1f\x04\x48\x04\r\0\n\0A\0\r\0\n\0"; /* "Пш\r\nA\r\n" */
        char out[64];
        DWORD n = now_playing_to_utf8(utf16, sizeof(utf16) - 1, out, sizeof(out));
        CHECK(n == 9 &&strcmp(out, "\xd0\x9f\xd1\x88\r\nA\r\n") == 0, "nowplaying: UTF-16LE is converted to UTF-8");
        n = now_playing_to_utf8("\xd0\x9f\r\nB", 5, out, sizeof(out));
        CHECK(n == 5 && strcmp(out, "\xd0\x9f\r\nB") == 0, "nowplaying: UTF-8 is kept as is");
    }

    /* The stream's read callback fills from the ring and names the stream for the playSound hook. */
    static PcmFrame got[2048];
    g_pcm_written = PCM_TARGET_FRAMES;
    CHECK(xbox_stream_read_hook((void *)1, got, sizeof(got)) == 0 && g_xbox_sound == (void *)1 && g_pcm_read == 2048,
          "stream read: the ring is read, the stream remembered as the Xbox Music sound");
    /* A recreated stream (timeline rewind) starts where the old channel stopped playing. */
    g_xbox_channel = g_played_channel = (void *)7;
    g_played_frames = 1024;
    CHECK(xbox_stream_read_hook((void *)2, got, sizeof(got)) == 0 && g_pcm_read == 2048 - 1024 + 2048 &&
              g_stream_frames == 2048 && g_xbox_channel == NULL,
          "stream recreated: the frames read ahead but never played are read again");
    /* FMOD may give the new stream the old one's address: the destroy callback marks it. */
    g_xbox_channel = g_played_channel = (void *)8;
    g_played_frames = 1024;
    g_stream_replaced = 1;
    CHECK(xbox_stream_read_hook((void *)2, got, sizeof(got)) == 0 && g_pcm_read == 3072 - 1024 + 2048 && !g_stream_replaced,
          "stream recreated at the same address: still read again");

    /* The station lock: only "Xbox Music" is forced open; story-locked stations keep the game's answer. */
    g_orig_station_locked = fake_station_locked;
    CHECK(station_locked_hook(NULL, std_string("Xbox Music")) == 0, "Xbox Music: not locked");
    CHECK(station_locked_hook(NULL, std_string("Radio Levante")) == 1 &&
              station_locked_hook(NULL, std_string("Innovative Leisure")) == 1 &&
              station_locked_hook(NULL, std_string("Xbox Music Extra")) == 1,
          "story-locked stations stay locked");

    /* The snapshot copy: RADIO_Station 8 (Xbox Music) reaches Studio as 6; other values and params pass. */
    g_param_set_value = fake_param_set;
    static unsigned char sources[3][0x88], entries[3][0x20], manager[0x58];
    const char *params[3] = {"RADIO_Station", "RADIO_Station", "AOPT_SpeakerMode"};
    float values[3] = {8.0f, 3.0f, 8.0f};
    for (int i = 0; i < 3; ++i) {
        memcpy(sources[i] + 0x60, std_string(params[i]), 32);
        memcpy(sources[i] + 0x14, &values[i], 4);
        void *instance = (void *)(uintptr_t)(i + 1), *source = sources[i];
        memcpy(entries[i] + 8, &instance, 8);
        memcpy(entries[i] + 0x10, &source, 8);
    }
    unsigned char *begin = &entries[0][0], *end = (unsigned char *)entries + sizeof(entries);
    memcpy(manager + 0x48, &begin, 8);
    memcpy(manager + 0x50, &end, 8);
    snapshot_copy_hook(manager);
    CHECK(param_values[1] == 6.0f && param_values[2] == 3.0f && param_values[3] == 8.0f,
          "snapshot copy: Xbox Music's RADIO_Station becomes a stock station, nothing else changes");

    /* nowplaying.txt: the new track goes into the spare buffer, and the HUD sees it only after the flip. */
    {
        GetTempPathW(PATH_CAPACITY, s_work_dir);
        wchar_t file[PATH_CAPACITY];
        path_join(file, s_work_dir, L"nowplaying.txt");
        HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w;
        WriteFile(h, "Song\r\nArtist\r\n", 14, &w, NULL);
        CloseHandle(h);
        LONG before = g_np_shown;
        now_playing_poll();
        DeleteFileW(file);
        CHECK(g_np_shown != before && strcmp(g_np[g_np_shown].title, "Song") == 0 && strcmp(g_np[g_np_shown].artist, "Artist") == 0,
              "nowplaying: the new track is shown from the other buffer");
        CHECK(strcmp(g_np[before].title, "Spotify Radio") == 0, "nowplaying: the shown buffer is not written in place");
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
