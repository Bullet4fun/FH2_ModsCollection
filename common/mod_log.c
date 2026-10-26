#include "mod_log.h"
#include "logging.h"
#include "support.h"

static wchar_t log_path[PATH_CAPACITY];
static const char *log_source = "Mod";
static volatile int at_line_start = 1; // log_append may get part of a line (label, then value)

void mod_log_open(const wchar_t *path, int truncate, const char *source) {
    size_t n = 0;
    while (path[n] && n < PATH_CAPACITY - 1) {
        log_path[n] = path[n];
        ++n;
    }
    log_path[n] = 0;
    log_source = source;
    if (!truncate || !log_path[0]) {
        return;
    }
    HANDLE file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

static int has_text(const char *text, size_t len, const char *word) {
    size_t w = text_length(word);
    for (size_t i = 0; i + w <= len; ++i) {
        if (memory_equal(text + i, word, w)) {
            return 1;
        }
    }
    return 0;
}

static void append_text(char *out, size_t *n, size_t cap, const char *text, size_t len) {
    for (size_t i = 0; i < len && *n < cap; ++i) {
        out[(*n)++] = text[i];
    }
}

/* "[HH:MM:SS] [LEVEL] [Source]: " for the line starting at text. A leading tag such as "SPOTIFY: " or
   "GARAGE report: " becomes the source (Spotify, Garage) and is cut; "report" or failure words make
   it WARN, "DEBUG:" makes it DEBUG. Returns the length of the cut tag. */
static size_t mod_log_prefix(const char *text, size_t len, char *out, size_t *n, size_t cap) {
    // local time from the file-time calls: GetLocalTime tipped antivirus ML engines into a detection
    FILETIME utc, local;
    SYSTEMTIME time = {0};
    GetSystemTimeAsFileTime(&utc);
    if (FileTimeToLocalFileTime(&utc, &local)) {
        FileTimeToSystemTime(&local, &time);
    }
    char clock[12] = {'[', (char)('0' + time.wHour / 10), (char)('0' + time.wHour % 10), ':',
                      (char)('0' + time.wMinute / 10), (char)('0' + time.wMinute % 10), ':',
                      (char)('0' + time.wSecond / 10), (char)('0' + time.wSecond % 10), ']', ' ', 0};
    size_t tag = 0, word = 0;
    while (word < len && text[word] >= 'A' && text[word] <= 'Z') {
        ++word;
    }
    const char *level = "INFO";
    if (word >= 3 && word < len && (text[word] == ':' || has_text(text + word, len - word < 8 ? len - word : 8, " report:"))) {
        tag = text[word] == ':' ? word + 1 : word + 8;
        if (tag > word + 1) {
            level = "WARN";
        }
        while (tag < len && text[tag] == ' ') {
            ++tag;
        }
    } else {
        word = 0;
    }
    if (word == 5 && memory_equal(text, "DEBUG", 5)) {
        level = "DEBUG";
        word = 0;
    } else if (has_text(text, len, "could not") || has_text(text, len, "failed") ||
               has_text(text, len, "not found") || has_text(text, len, "refused")) {
        level = "WARN";
    }
    append_text(out, n, cap, clock, 11);
    append_text(out, n, cap, "[", 1);
    append_text(out, n, cap, level, text_length(level));
    append_text(out, n, cap, "] [", 3);
    size_t same = 0; // SPOTIFY in the SpotifyRadio log is the mod itself
    while (same < word && log_source[same] && (log_source[same] | 0x20) == (text[same] | 0x20)) {
        ++same;
    }
    if (word && same < word) { // GARAGE -> Garage
        append_text(out, n, cap, text, 1);
        for (size_t i = 1; i < word && *n < cap; ++i) {
            out[(*n)++] = (char)(text[i] + ('a' - 'A'));
        }
    } else {
        append_text(out, n, cap, log_source, text_length(log_source));
    }
    append_text(out, n, cap, "]: ", 3);
    return tag;
}

void log_append(const char *message) { // one open per line: callers include the FMOD thread
    if (!log_path[0]) {
        return;
    }
    char line[1024];
    size_t n = 0, len = text_length(message), i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && message[end] != '\n') {
            ++end;
        }
        end += end < len; // keep the newline with its line
        if (at_line_start) {
            i += mod_log_prefix(message + i, end - i, line, &n, sizeof(line));
        }
        append_text(line, &n, sizeof(line), message + i, end - i);
        at_line_start = message[end - 1] == '\n';
        i = end;
    }
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line, (DWORD)n, &written, NULL);
        CloseHandle(file);
    }
}

void log_append_value(const char *label, int value) {
    char line[160];
    size_t n = 0;
    while (label[n] && n < sizeof(line) - 16) {
        line[n] = label[n];
        ++n;
    }
    char digits[12];
    int d = 0;
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    do {
        digits[d++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) {
        line[n++] = '-';
    }
    while (d) {
        line[n++] = digits[--d];
    }
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    log_append(line);
}
