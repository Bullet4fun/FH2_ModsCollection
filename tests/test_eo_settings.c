/* Extra Options settings: the supplied FH2_ExtraOptions.ini parses with every key, a switch at 0
   turns its feature off over the in-game value, and an unknown key is still refused. */
#include "settings.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// bindings.c reads input through these; parsing the INI never calls them
int game_key_down(unsigned key) { (void)key; return 0; }
int game_triggers_down(unsigned triggers) { (void)triggers; return 0; }

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void write_ini(const wchar_t *dir, const char *text) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls%ls", dir, MOD_INI_NAME);
    FILE *f = _wfopen(path, L"wb");
    fputs(text, f);
    fclose(f);
}

int main(void) {
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%hs\\fh2-eo-settings-%lu\\", getenv("TEMP"), GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);

    FILE *f = fopen("extra-options/FH2_ExtraOptions.ini", "rb");
    static char ini[32768];
    size_t n = fread(ini, 1, sizeof(ini) - 1, f);
    fclose(f);
    ini[n] = 0;
    write_ini(dir, ini);
    ModOptions options;
    CHECK(settings_read(dir, &options), "the supplied INI parses");
    CHECK(options.drift_camera && options.field_of_view && options.return_to_garage && options.in_game_options &&
              options.minimap_rotation && options.sun_shadows_option,
          "every new switch outside [Vehicle] defaults on");
    CHECK(!options.headlights_toggle && !options.roof_toggle && !options.turn_signals && !options.unlock_aero_tuning,
          "the supplied INI ships [Vehicle] off");

    write_ini(dir, "[Vehicle]\nRoofToggle=1\n[Graphics]\nSunShadows=0\n[Frontend]\nStartupLocation=0\n");
    CHECK(settings_read(dir, &options) && options.roof_toggle && !options.headlights_toggle && !options.turn_signals &&
              !options.unlock_aero_tuning,
          "[Vehicle] RoofToggle=1; a missing key stays off");
    options.smooth_sun_shadows = 1; // as the in-game menu would set them
    options.skip_car_meet_on_startup = options.spawn_at_festival = 1;
    settings_apply_switches(&options);
    CHECK(options.smooth_sun_shadows == 0 && !options.skip_car_meet_on_startup && !options.spawn_at_festival,
          "a switch at 0 overrides the in-game value");

    write_ini(dir, "[Vehicle]\nNoSuchKey=1\n");
    CHECK(!settings_read(dir, &options), "an unknown key is refused");

    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"cmd /c rmdir /s /q \"%ls\"", dir);
    _wsystem(cmd);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
