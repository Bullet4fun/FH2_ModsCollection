#ifndef FH2_MOD_LOG_H
#define FH2_MOD_LOG_H
#include "extra_options.h"
/* The log writer every mod shares: log_append / log_append_value (logging.h) go to the file set
   here; nothing is written until then. truncate: start the file empty; L"" stops logging. Lines get "[HH:MM:SS] [LEVEL] [source]: ". */
void mod_log_open(const wchar_t *path, int truncate, const char *source);
#endif
