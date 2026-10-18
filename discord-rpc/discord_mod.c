/* Discord Rich Presence mod (data\bin\mods\FH2_DiscordRPC.dll): "Playing <the game>" with
   the elapsed time while the game runs. No settings; remove the DLL to turn it off. */
#include "fh2_mod.h"
#include "discord_rpc.h"

FH2_MOD_GAMES(FH2_GAME_FORZA_HORIZON_2 | FH2_GAME_FAST_AND_FURIOUS); // patches nothing

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        discord_rpc_stop();
    }
    return TRUE;
}

__declspec(dllexport) int FH2ModLoad(const FH2ModHost *host) {
    if (!host || host->size < sizeof(*host) || host->api_version != FH2_MOD_API_VERSION) {
        return 0;
    }
    // the Discord application named after the game being played
    discord_rpc_start(host->game == FH2_GAME_FAST_AND_FURIOUS ? "1506500541919006861" : "1445162899315363924");
    // background thread; reconnects whenever Discord is running
    return 1;
}
