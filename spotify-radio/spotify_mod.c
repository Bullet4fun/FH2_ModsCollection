/* Spotify radio mod (data\bin\mods\FH2_SpotifyRadio.dll): Spotify Connect (librespot) plays as the
   game's own "Xbox Music" station, with Now Playing on the radio HUD. No settings. Its folder
   mods\FH2_SpotifyRadio\ holds the log, librespot.exe, onevent.cmd and librespot's files (Spotify
   login, volume, nowplaying.txt). */
#include "fh2_mod.h"
#include "spotify_bridge.h"
#include "logging.h"
#include "mod_log.h"
#include "support.h"

// No FH2_MOD_GAMES: an undeclared mod counts as Forza Horizon 2 only, which is what this one is (every
// hook is an FH2 address), so the loader never loads it into Fast & Furious. An explicit declaration
// or a host->game check makes one antivirus ML engine flag the DLL.

static wchar_t path[PATH_CAPACITY];

static int same_ignore_case(const wchar_t *a, const wchar_t *b) {
    for (;; ++a, ++b) {
        wchar_t x = *a >= L'A' && *a <= L'Z' ? *a + 32 : *a, y = *b >= L'A' && *b <= L'Z' ? *b + 32 : *b;
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
}

static wchar_t logo_source[PATH_CAPACITY], logo_target[PATH_CAPACITY], logo_record[PATH_CAPACITY];
static wchar_t logo_previous[PATH_CAPACITY];

/* Copies the mod's logo over the Xbox Music station's HUD icon in mediaoverride (needs ExtraOptions'
   EnableMediaOverrides=1). Only the file this mod placed last time (logo_installed.txt) is ever
   removed; an icon override the mod did not place is left alone. */
static void install_logo(const FH2ModHost *host) {
    if (!path_join(logo_source, host->data_dir, L"HUD_RadioIcon_Spotify.swatchbin") ||
        !path_join(logo_record, host->data_dir, L"logo_installed.txt") ||
        GetFileAttributesW(logo_source) == INVALID_FILE_ATTRIBUTES) {
        log_append("SPOTIFY: no HUD_RadioIcon_Spotify.swatchbin in the mod folder; Xbox Music keeps its logo.\r\n");
        return;
    }
    size_t cut = 0; // game_exe is ...\Mount\forza_durango_release_final.exe; mediaoverride is beside it
    for (size_t i = 0; host->game_exe[i]; ++i) {
        if (host->game_exe[i] == L'\\' || host->game_exe[i] == L'/') cut = i + 1;
    }
    for (size_t i = 0; i < cut; ++i) logo_target[i] = host->game_exe[i];
    logo_target[cut] = 0;
    if (!path_join(logo_target, logo_target, L"mediaoverride\\ui\\textures\\Anthem\\HUD\\") ||
        !path_join(logo_target, logo_target, L"HUD_RadioIcon_Xbox_Music.swatchbin")) {
        return;
    }
    for (size_t i = cut; logo_target[i]; ++i) { // create mediaoverride\ui\textures\Anthem\HUD\ as needed
        if (logo_target[i] == L'\\') {
            logo_target[i] = 0;
            CreateDirectoryW(logo_target, NULL);
            logo_target[i] = L'\\';
        }
    }

    DWORD got = 0;
    HANDLE record = CreateFileW(logo_record, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (record != INVALID_HANDLE_VALUE) {
        ReadFile(record, logo_previous, sizeof(logo_previous) - sizeof(wchar_t), &got, NULL);
        CloseHandle(record);
    }
    logo_previous[got / sizeof(wchar_t)] = 0;
    int ours = logo_previous[0] && same_ignore_case(logo_previous, logo_target);
    if (logo_previous[0] && !ours) {
        DeleteFileW(logo_previous); // this mod placed it elsewhere last time
    }
    if (!ours && GetFileAttributesW(logo_target) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(logo_record);
        log_append("SPOTIFY: the Xbox Music icon already has an override this mod did not place; left alone.\r\n");
        return;
    }
    if (!CopyFileW(logo_source, logo_target, FALSE)) {
        log_append("SPOTIFY: could not copy the logo into mediaoverride.\r\n");
        return;
    }
    record = CreateFileW(logo_record, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (record != INVALID_HANDLE_VALUE) {
        size_t length = 0;
        while (logo_target[length]) ++length;
        DWORD written;
        WriteFile(record, logo_target, (DWORD)(length * sizeof(wchar_t)), &written, NULL);
        CloseHandle(record);
    }
    log_append("SPOTIFY: logo installed over the Xbox Music HUD icon.\r\n");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        spotify_bridge_stop();
    }
    return TRUE;
}

__declspec(dllexport) int FH2ModLoad(const FH2ModHost *host) {
    if (!host || host->size < sizeof(*host) || host->api_version != FH2_MOD_API_VERSION || !host->game_image) {
        return 0;
    }
    CreateDirectoryW(host->data_dir, NULL); // fails harmlessly if it exists
    static wchar_t previous[PATH_CAPACITY]; // keep last session's log: problems get reported after a restart
    if (path_join(path, host->data_dir, L"FH2_SpotifyRadio.log") &&
        path_join(previous, host->data_dir, L"FH2_SpotifyRadio.previous.log")) {
        MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
        mod_log_open(path);
    }
    install_logo(host);
    spotify_bridge_start(host->data_dir);
    spotify_bridge_apply_audio_hook(host->game_image);
    return 1;
}
