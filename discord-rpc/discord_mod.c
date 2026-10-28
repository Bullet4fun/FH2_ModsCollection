/* Discord Rich Presence mod (mods\FH2_DiscordRPC.dll): "Playing <the game>" with
   the elapsed time while the game runs. No settings; remove the DLL to turn it off. */
#include "azimuth_mod.h"
#include "discord_rpc.h"
#include "logging.h"
#include "mod_log.h"
#include "support.h"

AZIMUTH_MOD_GAMES(AZIMUTH_GAME_FORZA_HORIZON_2 | AZIMUTH_GAME_FAST_AND_FURIOUS); // patches nothing

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        discord_rpc_stop();
    }
    return TRUE;
}

__declspec(dllexport) int AzimuthModLoad(const AzimuthModHost *host) {
    if (!host || host->size < sizeof(*host) || host->api_version != AZIMUTH_MOD_API_VERSION) {
        return 0;
    }
    // no log of its own: one line in the loader's (opened shared for this)
    static wchar_t log_path[PATH_CAPACITY];
    if (path_join(log_path, host->mods_dir, L"AzimuthModLoader.log")) {
        mod_log_open(log_path, 0, "DiscordRPC");
        log_append("FH2 Discord RPC " DISCORD_VERSION " by Bullet4fun: https://github.com/Bullet4fun/FH2_ModsCollection\r\n");
        mod_log_open(L"", 0, "DiscordRPC");
    }
    // the Discord application named after the game being played
    discord_rpc_start(host->game == AZIMUTH_GAME_FAST_AND_FURIOUS ? "1506500541919006861" : "1445162899315363924");
    // background thread; reconnects whenever Discord is running
    return 1;
}
