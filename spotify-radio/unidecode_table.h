#ifndef FH2_UNIDECODE_TABLE_H
#define FH2_UNIDECODE_TABLE_H
#include "extra_options.h"
/* Printable-ASCII approximation of a BMP character (no CJK ideographs / Hangul syllables), or NULL. */
const char *unidecode_lookup(unsigned int codepoint);
#endif
