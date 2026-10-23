#include "runtime.h"
#include "features.h"
#include "features/video_settings.h"
#include "logging.h"
#include "patch_engine.h"
#include "settings.h"
#include "support.h"

#ifdef FH2_AZIMUTH
#include "adapters/azimuth_host.h"
#endif

int runtime_initialize(unsigned char *game_image, const wchar_t *executable_path,
                       const wchar_t *settings_directory) {
    ModOptions options;
    settings_defaults(&options);
    gamerpic_set_directory(settings_directory);
    log_reset();
    int initialized = settings_read(settings_directory, &options);
    if (initialized) {
        video_settings_load(settings_directory);
        video_settings_seed(&options);
        video_settings_apply(&options);
        settings_apply_switches(&options);
        video_settings_show_features(&options);
    }
    if (initialized && options.enabled) {
        initialized = verify_game_file(executable_path);
    }
    if (initialized) {
        initialized = patch_engine_apply(game_image, &options);
    }
    log_start(settings_directory, &options);
#ifdef FH2_AZIMUTH
    if (initialized && options.enabled) {
        log_set_directory(settings_directory);
        azimuth_apply_save_compatibility(options.disable_azimuth_save_checksums);
    }
#endif
    if (initialized && options.enabled) {
        log_set_directory(settings_directory); // later lines: only what failed (the settings are above)
    }
    return initialized;
}
