/* Track buttons: the INI syntax, taps against double taps, combinations, keys. */
#include "track_controls.c"
#include <stdio.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static int step(unsigned short buttons, unsigned long long now) {
    unsigned char keys[TRACK_COMMANDS] = {0};
    return track_controls_step(buttons, keys, now);
}

int main(void) {
    TrackBinding b;
    CHECK(track_binding_parse("LS", &b) && b.buttons == 0x40 && b.taps == 1, "LS");
    CHECK(track_binding_parse("ls ls", &b) && b.buttons == 0x40 && b.taps == 2, "LS LS, any case");
    CHECK(track_binding_parse("LS+RB", &b) && b.buttons == 0x240 && b.taps == 1, "LS+RB");
    CHECK(track_binding_parse("F6", &b) && b.key == 0x75 && !b.buttons, "F6");
    CHECK(track_binding_parse("Q", &b) && b.key == 'Q', "a letter key");
    CHECK(track_binding_parse("", &b) && !b.buttons && !b.key, "empty is off");
    CHECK(!track_binding_parse("LS RB", &b), "a double tap repeats the same buttons");
    CHECK(!track_binding_parse("LS LS LS", &b), "no triple tap");
    CHECK(!track_binding_parse("F5+LS", &b) && !track_binding_parse("F13", &b) && !track_binding_parse("Turbo", &b),
          "nonsense is refused");

    /* the shipped defaults: LS = next, LS LS = previous */
    track_controls_load(L"Z:\\does-not-exist\\");
    CHECK(track_bindings[TRACK_NEXT][0].buttons == 0x40 && track_bindings[TRACK_PREVIOUS][0].taps == 2 &&
              track_bindings[TRACK_NEXT][1].key == 0x75 && track_bindings[TRACK_PREVIOUS][1].key == 0x74 &&
              track_bindings[TRACK_PLAY_PAUSE][1].key == 0x76 && !track_bindings[TRACK_PLAY_PAUSE][0].buttons,
          "defaults");
    CHECK(step(0x40, 1000) == TRACK_NONE && step(0, 1050) == TRACK_NONE, "a tap waits for a second one");
    CHECK(step(0, 1400) == TRACK_NEXT, "one tap: next");
    CHECK(step(0x40, 2000) == TRACK_NONE && step(0, 2050) == TRACK_NONE && step(0x40, 2150) == TRACK_PREVIOUS,
          "two taps: previous, at once");
    CHECK(step(0, 2200) == TRACK_NONE && step(0, 2600) == TRACK_NONE, "nothing after");
    CHECK(step(0x40, 2700) == TRACK_NONE && step(0, 2830) == TRACK_NONE && step(0, 2920) == TRACK_NONE && step(0x40, 2990) == TRACK_PREVIOUS &&
              step(0, 2995) == TRACK_NONE,
          "a real double tap: each press is held, the window runs from the release");

    /* a combination with its first button also bound alone */
    track_bindings[TRACK_PLAY_PAUSE][0] = (TrackBinding){0x240, 1, 0};
    setup_pads();
    CHECK(step(0x40, 3000) == TRACK_NONE && step(0x240, 3050) == TRACK_PLAY_PAUSE, "LS then RB: the combination");
    CHECK(step(0x40, 3100) == TRACK_NONE && step(0, 3150) == TRACK_NONE && step(0, 3600) == TRACK_NONE,
          "releasing it is no tap of LS");

    unsigned char keys[TRACK_COMMANDS] = {0, 1, 0, 0};
    CHECK(track_controls_step(0, keys, 4000) == TRACK_NEXT && track_controls_step(0, keys, 4015) == TRACK_NONE,
          "a key fires once per press");

    /* The INI: "Key = value   // note" (the default file), the old "Key=value" and ';' notes. */
    memory_copy(ini_text, default_ini, sizeof(default_ini));
    parse_ini();
    CHECK(track_bindings[TRACK_NEXT][0].buttons == 0x40 && track_bindings[TRACK_PREVIOUS][0].taps == 2 &&
              !track_bindings[TRACK_PLAY_PAUSE][0].buttons && track_bindings[TRACK_NEXT][1].key == 0x75 &&
              double_tap_ms == 200,
          "default INI: padded values with // notes");
    static const char edited[] = "[Controls] // x\r\nNextTrack=RB ; old note\r\nPlayPauseKey =\tF8\t// y\r\nDoubleTapTime = 300 // z\r\n";
    memory_copy(ini_text, edited, sizeof(edited));
    parse_ini();
    CHECK(track_bindings[TRACK_NEXT][0].buttons == 0x200 && track_bindings[TRACK_PLAY_PAUSE][1].key == 0x77 &&
              double_tap_ms == 300,
          "edited INI: old and new note styles");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
