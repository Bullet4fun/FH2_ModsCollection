#ifndef FH2_MOD_LOG_H
#define FH2_MOD_LOG_H
#include "extra_options.h"
/* A mod's own log file. Provides log_append / log_append_value (logging.h) for feature code
   built into a separate mod; nothing is written until mod_log_open. */
void mod_log_open(const wchar_t *path); // truncates the file
#endif
