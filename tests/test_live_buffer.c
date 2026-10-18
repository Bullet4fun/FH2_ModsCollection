/* Spotify live ring buffer: pcm_read (what the Xbox Music stream reads) against a
   simulated librespot writer, fades at every gap (no clicks), and the pipe reader's refill rule. */
#include "spotify_bridge.c"
#include <stdio.h>
#include <stdlib.h>

void log_append(const char *message) { (void)message; }
void log_append_value(const char *label, int value) { (void)label; (void)value; }

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static int constant; /* 0: frame k carries sample value k & 0x7fff; else every frame carries it */
static void produce(unsigned int frames) {
    ULONGLONG w = g_pcm_written;
    for (unsigned int i = 0; i < frames; ++i) {
        short v = constant ? (short)constant : (short)((w + i) & 0x7fff);
        g_pcm_buffer[(w + i) % PCM_BUFFER_FRAMES].left = v;
        g_pcm_buffer[(w + i) % PCM_BUFFER_FRAMES].right = v;
    }
    g_pcm_written = w + frames;
}

static PcmFrame out[4096];
static int all_silent(void) {
    for (int i = 0; i < 4096; ++i) if (out[i].left || out[i].right) return 0;
    return 1;
}
static int contiguous_from(int start) {
    for (int i = start + 1; i < 4096; ++i) if (((out[i - 1].left + 1) & 0x7fff) != (out[i].left & 0x7fff)) return 0;
    return 1;
}
static int largest_step(void) { /* biggest sample jump within this pull: a click shows up here */
    int m = 0;
    for (int i = 1; i < 4096; ++i) if (abs(out[i].left - out[i - 1].left) > m) m = abs(out[i].left - out[i - 1].left);
    return m;
}
static void pull(void) { pcm_read(out, sizeof(out)); }

int main(void) {
    produce(4096); pull();
    CHECK(all_silent() && g_pcm_read == 0, "start-up banks silently");
    produce(PCM_TARGET_FRAMES); pull();
    CHECK(out[PCM_FADE_IN_FRAMES].left == PCM_FADE_IN_FRAMES && contiguous_from(PCM_FADE_IN_FRAMES),
          "plays from frame 0 once banked, after a fade-in");
    CHECK(out[1].left == 0 && out[PCM_FADE_IN_FRAMES / 2].left < PCM_FADE_IN_FRAMES / 2, "fade-in starts from silence");
    for (int i = 0; i < 50; ++i) { produce(4096); pull(); CHECK(contiguous_from(0), "steady state is gapless and unscaled"); }
    ULONGLONG lag = g_pcm_written - g_pcm_read;
    CHECK(lag >= PCM_TARGET_FRAMES - 4096 && lag <= PCM_MAX_FRAMES, "steady lag near target");
    for (int i = 0; i < 108; ++i) produce(4096); /* game paused ~10s while Spotify plays */
    ULONGLONG live = g_pcm_written;
    pull();
    int v = (int)((live - PCM_TARGET_FRAMES + 4095) & 0x7fff);
    CHECK(abs(out[4095].left - v * 4095 / PCM_ENTER_FADE_FRAMES) <= 2, "entering again: a 0.4s fade-in, not a switch");
    for (int i = 0; i < 5; ++i) { produce(4096); pull(); }
    CHECK(contiguous_from(0) && (ULONGLONG)(unsigned short)out[0].left == ((live - PCM_TARGET_FRAMES + 5 * 4096) & 0x7fff),
          "resumes behind live, not stale, and plays unscaled after the fade");
    for (int i = 0; i < PCM_MAX_FRAMES / 4096 + 2; ++i) pull();
    CHECK(all_silent(), "Spotify paused gives silence");
    produce(2000); pull();
    CHECK(all_silent(), "after an underrun, waits to re-bank");
    produce(PCM_TARGET_FRAMES); pull();
    CHECK(!all_silent() && contiguous_from(PCM_FADE_IN_FRAMES), "plays again once re-banked");

    /* Track skip: loud audio, then librespot goes quiet while loading. No hard cut either way. */
    constant = 30000;
    for (int i = 0; i < PCM_TARGET_FRAMES / 4096 + 2; ++i) { produce(4096); pull(); } /* flush the sawtooth */
    int worst = 0;
    for (int i = 0; i < PCM_TARGET_FRAMES / 4096 + 3; ++i) { pull(); if (largest_step() > worst) worst = largest_step(); }
    CHECK(all_silent(), "ring ran dry: silence in the end");
    CHECK(worst <= 300, "running dry fades out instead of cutting from 30000 to 0");
    worst = 0;
    produce(PCM_TARGET_FRAMES + 4096);
    for (int i = 0; i < 3; ++i) { pull(); if (largest_step() > worst) worst = largest_step(); }
    CHECK(out[4095].left == 30000 && worst <= 100, "new track fades in instead of jumping from 0 to 30000");

    /* Jump to live while loud audio was playing (game stopped pulling, then resumed): the old
       fade-in started from 0, a step from the last sample sent - the "pistol shot". */
    for (int i = 0; i < 3; ++i) { produce(4096); pull(); }
    CHECK(out[4095].left == 30000, "playing loud");
    constant = -30000;
    for (int i = 0; i < 20; ++i) produce(4096); /* the game stopped pulling meanwhile */
    pull();
    CHECK(largest_step() <= 300 && out[0].left > 29000 && out[4095].left < -6000 && out[4095].left > -8000,
          "jump to live: no step from the last sample sent, then a slow rise");
    worst = 0;
    for (int i = 0; i < 4; ++i) { produce(4096); pull(); if (largest_step() > worst) worst = largest_step(); }
    CHECK(out[4095].left == -30000 && worst <= 300, "entry fade completes smoothly");

    /* pipe_refill_due: keeps the ring PCM_TARGET_FRAMES ahead of the game's reader, no further */
    const ULONGLONG B = sizeof(PcmFrame);
    CHECK(pipe_refill_due(500, 100000, 100000 - PCM_TARGET_FRAMES, 0) == 500, "margin full: read nothing");
    CHECK(pipe_refill_due(500, 100000, 100000 - PCM_TARGET_FRAMES - 4000, 0) == 500, "over target (no drift build-up): nothing");
    CHECK(pipe_refill_due(500, 100000, 100000 - 1000, 0) == 500 + (PCM_TARGET_FRAMES - 1000) * B, "margin short: refill it");
    CHECK(pipe_refill_due(500, 100000, 100000, 0) == 500 + PCM_TARGET_FRAMES * B, "ring dry: bank the full target");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
