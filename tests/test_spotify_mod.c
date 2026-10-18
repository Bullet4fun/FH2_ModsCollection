/* Spotify mod: the logo goes onto the Xbox Music icon, replaces only what this mod placed (also at
   another path, as recorded), never a user's own override. */
#include "spotify_bridge.c"
#include "spotify_mod.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
    wchar_t root[MAX_PATH], data[MAX_PATH], exe[MAX_PATH], hud[MAX_PATH], f[MAX_PATH];
    swprintf(root, MAX_PATH, L"%hs\\fh2-logo-test-%lu", getenv("TEMP"), GetCurrentProcessId());
    swprintf(data, MAX_PATH, L"%ls\\mods\\FH2_SpotifyRadio\\", root);
    swprintf(exe, MAX_PATH, L"%ls\\Mount\\game.exe", root);
    swprintf(hud, MAX_PATH, L"%ls\\Mount\\mediaoverride\\ui\\textures\\Anthem\\HUD\\", root);
    CreateDirectoryW(root, NULL);
    swprintf(f, MAX_PATH, L"%ls\\mods", root); CreateDirectoryW(f, NULL); CreateDirectoryW(data, NULL);
    swprintf(f, MAX_PATH, L"%ls\\Mount", root); CreateDirectoryW(f, NULL);
    FH2ModHost host = {sizeof(host), FH2_MOD_API_VERSION, NULL, exe, root, root, data, FH2_GAME_FORZA_HORIZON_2};
#define AT(dir, name) (swprintf(f, MAX_PATH, L"%ls%ls", dir, name), f)
#define EXISTS(dir, name) (GetFileAttributesW(AT(dir, name)) != INVALID_FILE_ATTRIBUTES)
    install_logo(&host);
    CHECK(!EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), "no logo file: nothing placed");

    /* the recorded logo sits on another icon */
    FILE *w = _wfopen(AT(data, L"HUD_RadioIcon_Spotify.swatchbin"), L"wb"); fputs("logo", w); fclose(w);
    install_logo(&host); /* creates the folders */
    w = _wfopen(AT(hud, L"HUD_RadioIcon_Pulse.swatchbin"), L"wb"); fputs("logo", w); fclose(w);
    wchar_t old_target[MAX_PATH];
    swprintf(old_target, MAX_PATH, L"%lsHUD_RadioIcon_Pulse.swatchbin", hud);
    w = _wfopen(AT(data, L"logo_installed.txt"), L"wb"); fwrite(old_target, sizeof(wchar_t), wcslen(old_target), w); fclose(w);
    install_logo(&host);
    CHECK(EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin") && !EXISTS(hud, L"HUD_RadioIcon_Pulse.swatchbin"),
          "logo on Xbox Music, the recorded logo removed");
    install_logo(&host);
    CHECK(EXISTS(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), "again: still there");

    /* a user's own Xbox Music icon override is never replaced */
    DeleteFileW(AT(data, L"logo_installed.txt"));
    w = _wfopen(AT(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), L"wb"); fputs("user", w); fclose(w);
    install_logo(&host);
    FILE *r = _wfopen(AT(hud, L"HUD_RadioIcon_Xbox_Music.swatchbin"), L"rb"); char got[8] = ""; fgets(got, 8, r); fclose(r);
    CHECK(!strcmp(got, "user"), "user's override kept");

    wchar_t cmd[MAX_PATH + 32]; swprintf(cmd, MAX_PATH + 32, L"cmd /c rmdir /s /q \"%ls\"", root); _wsystem(cmd);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
