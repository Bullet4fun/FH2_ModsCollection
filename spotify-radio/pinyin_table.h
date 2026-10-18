#ifndef FH2_PINYIN_TABLE_H
#define FH2_PINYIN_TABLE_H
#include "extra_options.h"
/* Toneless Mandarin reading of a CJK Unified Ideograph (U+4E00..U+9FFF), or NULL. */
const char *pinyin_lookup(unsigned int codepoint);
#endif
