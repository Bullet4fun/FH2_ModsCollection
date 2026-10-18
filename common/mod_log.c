#include "mod_log.h"
#include "logging.h"
#include "support.h"

static wchar_t log_path[PATH_CAPACITY];

void mod_log_open(const wchar_t *path) {
    size_t n = 0;
    while (path[n] && n < PATH_CAPACITY - 1) {
        log_path[n] = path[n];
        ++n;
    }
    log_path[n] = 0;
    HANDLE file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

void log_append(const char *message) { // one open per line: callers include the FMOD thread
    if (!log_path[0]) {
        return;
    }
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, message, (DWORD)text_length(message), &written, NULL);
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
    line[n++] = ' ';
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
