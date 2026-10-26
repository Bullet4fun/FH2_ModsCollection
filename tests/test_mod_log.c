/* The shared log writer: every line starts "[HH:MM:SS] [LEVEL] [Source]: ". */
#include "mod_log.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); ++fails; } } while (0)

int main(void) {
    SYSTEMTIME before, after;
    GetLocalTime(&before);
    mod_log_open(L"test_mod_log.tmp", 1, "SpotifyRadio");
    log_append("FH2 Spotify Radio 1.2.0\r\n");
    log_append("SPOTIFY: librespot started.\r\n");
    log_append("SPOTIFY report: underrun\r\n");
    log_append_value("DEBUG: new stream ", 3);
    log_append("Volume");
    log_append_value("=", 5);
    log_append("GARAGE: hook could not be installed\r\nsecond line\r\n");
    mod_log_open(L"", 0, "SpotifyRadio");
    GetLocalTime(&after);

    char text[2048] = {0};
    FILE *file = fopen("test_mod_log.tmp", "rb");
    size_t len = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if (file) fclose(file);
    remove("test_mod_log.tmp");
    static const char *expected[] = {
        "[INFO] [SpotifyRadio]: FH2 Spotify Radio 1.2.0\r\n",
        "[INFO] [SpotifyRadio]: librespot started.\r\n",
        "[WARN] [SpotifyRadio]: underrun\r\n",
        "[DEBUG] [SpotifyRadio]: new stream 3\r\n",
        "[INFO] [SpotifyRadio]: Volume=5\r\n",
        "[WARN] [Garage]: hook could not be installed\r\n",
        "[INFO] [SpotifyRadio]: second line\r\n",
    };
    char *line = text;
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        int clock = line[0] == '[' && line[3] == ':' && line[6] == ':' && line[9] == ']' && line[10] == ' ';
        char what[96];
        snprintf(what, sizeof(what), "line %zu: %.40s", i + 1, expected[i]);
        CHECK(clock && strncmp(line + 11, expected[i], strlen(expected[i])) == 0, what);
        line += 11 + strlen(expected[i]);
    }
    CHECK((size_t)(line - text) == len, "nothing else written");
    // the stamp is local time: the first line's clock lies between the times read around the writes
    int stamp = ((text[1] - '0') * 10 + text[2] - '0') * 3600 + ((text[4] - '0') * 10 + text[5] - '0') * 60 +
                (text[7] - '0') * 10 + text[8] - '0';
    int from = before.wHour * 3600 + before.wMinute * 60 + before.wSecond;
    int to = after.wHour * 3600 + after.wMinute * 60 + after.wSecond;
    CHECK(from <= to ? stamp >= from && stamp <= to : stamp >= from || stamp <= to, "clock is local time");
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
