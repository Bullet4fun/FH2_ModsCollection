#define COBJMACROS
#include "line_in_capture.h"
#include "track_controls.h"
#include "logging.h"
#include "support.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shobjidl.h>

/* Source = Songs: the files in the mod's Songs\ folder, decoded by Media Foundation (Windows' own
   decoders: MP3, FLAC, WAV, M4A/AAC, WMA) into the bridge's format and written to its pipe. The
   song's title goes to nowplaying.txt, which the bridge already shows on the radio HUD. */

static const GUID k_media_type = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID k_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID k_channels = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
static const GUID k_rate = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
static const GUID k_bits = {0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
static const GUID k_block = {0x322de230, 0x9eeb, 0x43bd, {0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d}};
static const GUID k_bytes_per_second = {0x1aab75c8, 0xcfef, 0x451c, {0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31}};
static const GUID k_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID k_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID k_iid_property_store = {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};
static const PROPERTYKEY k_title = {{0xf29f85e0, 0x4ff9, 0x1068, {0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9}}, 2};
static const PROPERTYKEY k_artist = {{0x56a3372e, 0xce9c, 0x11d2, {0x9f, 0x0e, 0x00, 0x60, 0x97, 0xc6, 0x86, 0xf6}}, 2};
#define MF_STARTUP_VERSION 0x00020070u // MF_VERSION of the Windows 7+ SDK (mfapi.h picks by WINVER)
#define FIRST_AUDIO_STREAM 0xfffffffdu // MF_SOURCE_READER_FIRST_AUDIO_STREAM
#define ALL_STREAMS 0xfffffffeu        // MF_SOURCE_READER_ALL_STREAMS

#define MAX_SONGS 2048 // shortcut: Songs\ itself, read once at startup; subfolders and a rescan if anyone asks
static wchar_t g_songs[MAX_SONGS][MAX_PATH];
static unsigned g_order[MAX_SONGS];
static unsigned g_alphabetical[MAX_SONGS]; // Shuffle off: by name, whatever order the file system lists them in
static unsigned g_count;

static HANDLE g_player_thread = NULL;
static HANDLE g_pipe = NULL;
static volatile int g_playing = 0;
static volatile int g_paused = 0;
static volatile LONG g_skip = 0; // +1 next, -1 previous: the song playing stops

int line_in_songs_command(int command) {
    if (!g_player_thread) {
        return 0;
    }
    if (command == TRACK_NEXT || command == TRACK_PREVIOUS) {
        __atomic_store_n(&g_skip, command == TRACK_NEXT ? 1 : -1, __ATOMIC_RELEASE);
        g_paused = 0;
    } else if (command == TRACK_PLAY_PAUSE) {
        g_paused = !g_paused;
    }
    return 1;
}

static int is_song(const wchar_t *name) {
    static const wchar_t *const extensions[] = {L".mp3", L".flac", L".wav", L".m4a", L".aac", L".wma"};
    const wchar_t *dot = NULL;
    for (const wchar_t *c = name; *c; ++c) {
        if (*c == L'.') dot = c;
    }
    for (unsigned e = 0; dot && e < ARRAY_COUNT(extensions); ++e) {
        size_t i = 0;
        while (dot[i] && extensions[e][i] && (dot[i] | 0x20) == extensions[e][i]) ++i;
        if (!dot[i] && !extensions[e][i]) {
            return 1;
        }
    }
    return 0;
}

static int name_after(const wchar_t *a, const wchar_t *b) { // 1: a sorts after b
    for (;; ++a, ++b) {
        wchar_t x = *a >= L'A' && *a <= L'Z' ? *a + 32 : *a, y = *b >= L'A' && *b <= L'Z' ? *b + 32 : *b;
        if (x != y || !x) {
            return x > y;
        }
    }
}

static void list_songs(const wchar_t *folder) {
    static wchar_t pattern[PATH_CAPACITY];
    WIN32_FIND_DATAW found;
    g_count = 0;
    if (!path_join(pattern, folder, L"*")) {
        return;
    }
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_song(found.cFileName) && g_count < MAX_SONGS &&
            path_join(g_songs[g_count], folder, found.cFileName)) {
            ++g_count;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    for (unsigned i = 0; i < g_count; ++i) { // insertion sort by name, ignoring case
        unsigned j = i;
        for (; j > 0 && name_after(g_songs[g_alphabetical[j - 1]], g_songs[i]); --j) {
            g_alphabetical[j] = g_alphabetical[j - 1];
        }
        g_alphabetical[j] = i;
    }
}

static void shuffle_order(void) {
    static unsigned long long seed;
    seed = seed ? seed : GetTickCount64() | 1;
    for (unsigned i = 0; i < g_count; ++i) g_order[i] = g_alphabetical[i];
    for (unsigned i = g_count; line_in_shuffle && i > 1; --i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; // xorshift64
        unsigned j = (unsigned)(seed % i), t = g_order[i - 1];
        g_order[i - 1] = g_order[j];
        g_order[j] = t;
    }
}

/* The next position in g_order. Forward past the last song starts a new round, reshuffled; back goes to
   the song played before, also across that boundary (the last round is kept). Back from the very
   first song plays it again. */
static unsigned g_previous_order[MAX_SONGS];
static int g_has_previous;
static unsigned advance(unsigned at, int back) {
    if (!back) {
        if (++at < g_count) {
            return at;
        }
        memory_copy(g_previous_order, g_order, sizeof(g_order[0]) * g_count);
        g_has_previous = 1;
        shuffle_order();
        return 0;
    }
    if (at > 0) {
        return at - 1;
    }
    if (g_has_previous) {
        memory_copy(g_order, g_previous_order, sizeof(g_order[0]) * g_count);
        g_has_previous = 0;
        return g_count - 1;
    }
    return 0;
}

static void utf8_append(char *out, size_t capacity, size_t *used, const wchar_t *text, int length) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, length, out + *used, (int)(capacity - *used - 1), NULL, NULL);
    *used += n > 0 ? (size_t)n : 0;
    out[*used] = 0;
}

/* "title\r\nartist\r\n" in UTF-8: the tags, else the file name ("Artist - Title.mp3" splits). */
static void now_playing_text(const wchar_t *path, char *out, size_t capacity) {
    size_t used = 0;
    out[0] = 0;
    IPropertyStore *store = NULL;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path, NULL, GPS_DEFAULT, &k_iid_property_store, (void **)&store))) {
        PROPVARIANT title, artist;
        memory_copy(&title, &(PROPVARIANT){0}, sizeof(title));
        memory_copy(&artist, &(PROPVARIANT){0}, sizeof(artist));
        if (SUCCEEDED(IPropertyStore_GetValue(store, &k_title, &title)) && title.vt == VT_LPWSTR && title.pwszVal[0]) {
            utf8_append(out, capacity, &used, title.pwszVal, -1);
            used = text_length(out);
            utf8_append(out, capacity, &used, L"\r\n", 2);
            // System.Music.Artist is a string list: each artist on its own line, as the bridge expects
            if (SUCCEEDED(IPropertyStore_GetValue(store, &k_artist, &artist)) && artist.vt == (VT_VECTOR | VT_LPWSTR)) {
                for (ULONG i = 0; i < artist.calpwstr.cElems; ++i) {
                    utf8_append(out, capacity, &used, artist.calpwstr.pElems[i], -1);
                    used = text_length(out);
                    utf8_append(out, capacity, &used, L"\r\n", 2);
                }
            }
        }
        PropVariantClear(&title);
        PropVariantClear(&artist);
        IPropertyStore_Release(store);
    }
    if (used) {
        return;
    }
    const wchar_t *name = path, *dot = NULL, *dash = NULL;
    for (const wchar_t *c = path; *c; ++c) {
        if (*c == L'\\' || *c == L'/') name = c + 1;
    }
    for (const wchar_t *c = name; *c; ++c) {
        if (*c == L'.') dot = c;
        if (!dash && c[0] == L' ' && c[1] == L'-' && c[2] == L' ') dash = c;
    }
    const wchar_t *end = dot;
    if (!end) for (end = name; *end; ++end) {}
    if (dash && dash < end) {
        utf8_append(out, capacity, &used, dash + 3, (int)(end - dash - 3));
        utf8_append(out, capacity, &used, L"\r\n", 2);
        utf8_append(out, capacity, &used, name, (int)(dash - name));
    } else {
        utf8_append(out, capacity, &used, name, (int)(end - name));
    }
    utf8_append(out, capacity, &used, L"\r\n", 2);
}

void line_in_now_playing_write(const char *text) {
    HANDLE file = CreateFileW(line_in_now_playing_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, text, (DWORD)text_length(text), &written, NULL);
        CloseHandle(file);
    }
}

static void show_now_playing(const wchar_t *path) {
    static char text[1024];
    now_playing_text(path, text, sizeof(text));
    line_in_now_playing_write(text);
}

/* Decodes one file as 44.1 kHz 16-bit stereo into write(). 1: played to the end or skipped,
   0: Media Foundation could not read it, -1: write() failed (the bridge is gone). */
int line_in_songs_decode(const wchar_t *path, int (*write)(const void *data, DWORD size)) {
    IMFSourceReader *reader = NULL;
    IMFMediaType *type = NULL;
    int result = 0;
    if (FAILED(MFCreateSourceReaderFromURL(path, NULL, &reader))) {
        return 0;
    }
    HRESULT hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(type, &k_media_type, &k_audio);
        IMFMediaType_SetGUID(type, &k_subtype, &k_pcm);
        IMFMediaType_SetUINT32(type, &k_channels, 2);
        IMFMediaType_SetUINT32(type, &k_rate, 44100);
        IMFMediaType_SetUINT32(type, &k_bits, 16);
        IMFMediaType_SetUINT32(type, &k_block, 4);
        IMFMediaType_SetUINT32(type, &k_bytes_per_second, 44100 * 4);
        IMFSourceReader_SetStreamSelection(reader, ALL_STREAMS, FALSE);
        IMFSourceReader_SetStreamSelection(reader, FIRST_AUDIO_STREAM, TRUE);
        hr = IMFSourceReader_SetCurrentMediaType(reader, FIRST_AUDIO_STREAM, NULL, type);
        IMFMediaType_Release(type);
    }
    while (SUCCEEDED(hr)) {
        while (g_paused && g_playing && !__atomic_load_n(&g_skip, __ATOMIC_ACQUIRE)) {
            Sleep(20); // nothing written: the radio fades out as on a pause
        }
        if (!g_playing || __atomic_load_n(&g_skip, __ATOMIC_ACQUIRE)) {
            result = 1;
            break;
        }
        DWORD flags = 0;
        IMFSample *sample = NULL;
        hr = IMFSourceReader_ReadSample(reader, FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &sample);
        if (FAILED(hr)) {
            break;
        }
        int ok = 1;
        if (sample) {
            IMFMediaBuffer *buffer = NULL;
            BYTE *data;
            DWORD length;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)) &&
                SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length))) {
                ok = write(data, length);
                IMFMediaBuffer_Unlock(buffer);
            }
            if (buffer) IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
        }
        if (!ok) {
            result = -1;
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            result = 1;
            break;
        }
    }
    IMFSourceReader_Release(reader);
    return result;
}

static int write_pipe(const void *data, DWORD size) {
    DWORD written;
    return WriteFile(g_pipe, data, size, &written, NULL) && written == size;
}

static DWORD WINAPI songs_thread(LPVOID param) {
    const wchar_t *folder = param;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_STARTUP_VERSION, MFSTARTUP_LITE))) {
        log_append("LINE-IN: Media Foundation is unavailable; no songs.\r\n");
        return 0;
    }
    list_songs(folder);
    log_append_value("LINE-IN: songs found in the Songs folder: ", (int)g_count);
    unsigned failed_in_a_row = 0;
    shuffle_order();
    for (unsigned at = 0; g_count && g_playing && failed_in_a_row < g_count; ) {
        const wchar_t *path = g_songs[g_order[at]];
        __atomic_store_n(&g_skip, 0, __ATOMIC_RELEASE);
        show_now_playing(path);
        int played = line_in_songs_decode(path, write_pipe);
        if (played < 0) {
            break;
        }
        if (!played) {
            static char name[MAX_PATH * 3];
            int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, name, sizeof(name), NULL, NULL);
            name[n > 0 ? n - 1 : 0] = 0;
            log_append("LINE-IN: could not play ");
            log_append(name);
            log_append("\r\n");
        }
        failed_in_a_row = played ? 0 : failed_in_a_row + 1;
        at = advance(at, __atomic_load_n(&g_skip, __ATOMIC_ACQUIRE) < 0);
    }
    if (g_count && failed_in_a_row >= g_count) {
        log_append("LINE-IN: none of the songs could be played.\r\n");
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

unsigned line_in_songs_count(const wchar_t *folder) {
    static wchar_t songs[PATH_CAPACITY];
    if (!path_join(songs, folder, L"Songs")) {
        return 0;
    }
    list_songs(songs);
    return g_count;
}

int line_in_songs_start(HANDLE pipe_write, const wchar_t *folder) { // pipe_write stays the caller's
    static wchar_t songs[PATH_CAPACITY];
    if (g_player_thread || !path_join(songs, folder, L"Songs")) {
        return 0;
    }
    CreateDirectoryW(songs, NULL); // so the folder is there to fill
    g_pipe = pipe_write;
    g_playing = 1;
    g_player_thread = CreateThread(NULL, 0, songs_thread, songs, 0, NULL);
    if (!g_player_thread) {
        g_playing = 0;
        return 0;
    }
    return 1;
}

void line_in_songs_stop(void) {
    g_playing = 0;
    if (g_player_thread) {
        WaitForSingleObject(g_player_thread, 3000);
        CloseHandle(g_player_thread);
        g_player_thread = NULL;
    }
    g_pipe = NULL;
}
