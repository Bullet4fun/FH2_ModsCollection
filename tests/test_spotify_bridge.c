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
    /* The stream's read callback fills from the ring and names the stream for the playSound hook. */
    static PcmFrame got[2048];
    g_pcm_written = PCM_TARGET_FRAMES;
    CHECK(xbox_stream_read_hook((void *)1, got, sizeof(got)) == 0 && g_xbox_sound == (void *)1 && g_pcm_read == 2048,
          "stream read: the ring is read, the stream remembered as the Xbox Music sound");

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

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
