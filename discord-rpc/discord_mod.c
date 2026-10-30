/* Discord Rich Presence mod (mods\FH2_DiscordRPC.dll): "Playing <the game>" (any of Azimuth's games) with
   the elapsed time while the game runs. No settings; remove the DLL to turn it off. */
#include "azimuth_mod.h"
#include "discord_rpc.h"
#include "logging.h"
#include "mod_log.h"
#include "support.h"

AZIMUTH_MOD_GAMES(AZIMUTH_GAME_FORZA_HORIZON_2 | AZIMUTH_GAME_FAST_AND_FURIOUS | AZIMUTH_GAME_FORZA_HORIZON_3 |
                  AZIMUTH_GAME_FORZA_MOTORSPORT_5 | AZIMUTH_GAME_FORZA_MOTORSPORT_6); // patches nothing

// the Discord application named after each game, and the game's name for the picture's tooltip
static const struct {
    unsigned game;
    const char *client_id, *name;
} apps[] = {
    {AZIMUTH_GAME_FORZA_HORIZON_2, "1445162899315363924", "Forza Horizon 2"},
    {AZIMUTH_GAME_FAST_AND_FURIOUS, "1506500541919006861", "Forza Horizon 2 Presents Fast & Furious"},
    {AZIMUTH_GAME_FORZA_HORIZON_3, "496760939052269583", "Forza Horizon 3"},
    {AZIMUTH_GAME_FORZA_MOTORSPORT_5, "1445155326671454228", "Forza Motorsport 5"},
    {AZIMUTH_GAME_FORZA_MOTORSPORT_6, "1445155264519995412", "Forza Motorsport 6"},
};

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
    for (unsigned i = 0; i < ARRAY_COUNT(apps); ++i) {
        if (apps[i].game == host->game) {
            discord_rpc_start(apps[i].client_id, apps[i].name);
        }
    }
    // background thread; reconnects whenever Discord is running
    return 1;
}
