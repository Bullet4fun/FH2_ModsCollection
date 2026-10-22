/* Spotify mod: the logo goes into Mount\spotifyradio\ as the Xbox Music icon, and a logo this mod
   recorded somewhere else is removed. */
#include "spotify_bridge.c"
#include "spotify_mod.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
    wchar_t root[MAX_PATH], data[MAX_PATH], exe[MAX_PATH], hud[MAX_PATH], old[MAX_PATH], f[MAX_PATH];
    swprintf(root, MAX_PATH, L"%hs\\fh2-logo-test-%lu", getenv("TEMP"), GetCurrentProcessId());
    swprintf(data, MAX_PATH, L"%ls\\mods\\FH2_SpotifyRadio\\", root);
    swprintf(exe, MAX_PATH, L"%ls\\Mount\\game.exe", root);
    swprintf(hud, MAX_PATH, L"%ls\\Mount\\spotifyradio\\ui\\textures\\Anthem\\HUD\\", root);
    swprintf(old, MAX_PATH, L"%ls\\Mount\\HUD_RadioIcon_Xbox_Music.swatchbin", root);
    CreateDirectoryW(root, NULL);
    swprintf(f, MAX_PATH, L"%ls\\mods", root); CreateDirectoryW(f, NULL); CreateDirectoryW(data, NULL);
    swprintf(f, MAX_PATH, L"%ls\\Mount", root); CreateDirectoryW(f, NULL);
    FH2ModHost host = {sizeof(host), FH2_MOD_API_VERSION, NULL, exe, root, root, data, FH2_GAME_FORZA_HORIZON_2};
#define AT(dir, name) (swprintf(f, MAX_PATH, L"%ls%ls", dir, name), f)
#define EXISTS(dir, name) (GetFileAttributesW(AT(dir, name)) != INVALID_FILE_ATTRIBUTES)
    CHECK(!install_logo(&host) && !EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), "no logo file: nothing placed");

    FILE *w = _wfopen(AT(data, L"HUD_RadioIcon_Spotify.swatchbin"), L"wb"); fputs("logo", w); fclose(w);
    w = _wfopen(old, L"wb"); fputs("logo", w); fclose(w); /* placed elsewhere last time, and recorded */
    w = _wfopen(AT(data, L"logo_installed.txt"), L"wb"); fwrite(old, sizeof(wchar_t), wcslen(old), w); fclose(w);
    CHECK(install_logo(&host) && EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin") &&
              GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES,
          "logo in spotifyradio, the recorded old copy removed");
    CHECK(install_logo(&host) && EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), "again: still there");

    wchar_t cmd[MAX_PATH + 32]; swprintf(cmd, MAX_PATH + 32, L"cmd /c rmdir /s /q \"%ls\"", root); _wsystem(cmd);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
