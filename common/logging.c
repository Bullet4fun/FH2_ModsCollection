#include "logging.h"
#include "features.h"
#include "mod_log.h"
#include "settings.h"
#include "support.h"

// Extra Options' log file (MOD_LOG_NAME in its settings folder) on top of the shared writer in mod_log.c.

static wchar_t path[PATH_CAPACITY]; // static: a frame this big needs a stack probe the freestanding build lacks

void log_reset(void) {
    mod_log_open(L"", 0, "ExtraOptions");
}

void log_set_directory(const wchar_t *directory) {
    mod_log_open(path_join(path, directory, MOD_LOG_NAME) ? path : L"", 0, "ExtraOptions");
}

void log_start(const wchar_t *directory, const ModOptions *options) {
    if (!path_join(path, directory, MOD_LOG_NAME)) {
        return;
    }
    mod_log_open(path, 1, "ExtraOptions");
    log_append(MOD_NAME " " MOD_VERSION "\r\n");
    log_append(status_get());
    log_append("\r\n");
    for (size_t index = 0; index < setting_count; ++index) {
        log_append(setting_definitions[index].log_name);
        log_append_value("=", settings_value(options, &setting_definitions[index]));
    }
}
