/* Mod-loader adapter: this DLL is loaded from data\bin\mods\ by the FH2 mod loader, which owns
   Azimuth's helper ABI and has already located the game. */
#include "fh2_mod.h"
#include "logging.h"
#include "runtime.h"
#include "support.h"

FH2_MOD_GAMES(FH2_GAME_FORZA_HORIZON_2); // every patch is a Forza Horizon 2 address

void adapter_process_attach(HINSTANCE instance) {
    (void)instance; // the work happens in FH2ModLoad, called by the loader
}

__declspec(dllexport) int FH2ModLoad(const FH2ModHost *host) {
    if (!host || host->size < sizeof(*host) || host->api_version != FH2_MOD_API_VERSION ||
        !host->game_image) {
        return 0;
    }
    if (host->game != FH2_GAME_FORZA_HORIZON_2) {
        return 0; // every patch is an FH2 address: another build of the engine is left alone
    }
    CreateDirectoryW(host->data_dir, NULL); // ini, .dat, log, Gamerpic.png; fails harmlessly if it exists
    if (!runtime_initialize(host->game_image, host->game_exe, host->data_dir)) {
        return 0;
    }
    char message[] = "LOADER: FH2 image at 0x0000000000000000; loaded as a mod.\r\n";
    const char digits[] = "0123456789abcdef";
    uintptr_t value = (uintptr_t)host->game_image;
    for (unsigned i = 0; i < 16; ++i) {
        message[sizeof("LOADER: FH2 image at 0x") - 1 + i] = digits[(value >> (60 - 4 * i)) & 15];
    }
    log_set_directory(host->data_dir);
    log_append(message);
    return 1;
}
