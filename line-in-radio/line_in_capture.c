#define COBJMACROS
#include "line_in_capture.h"
#include "logging.h"
#include "support.h"
#include <mmdeviceapi.h>
#include <audioclient.h>

char line_in_device[256];
int line_in_use_songs = 1;
int line_in_shuffle = 1;
wchar_t line_in_now_playing_path[PATH_CAPACITY];
wchar_t line_in_command_path[PATH_CAPACITY];

static const GUID k_device_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID k_iid_device_enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID k_iid_endpoint = {0x1be09788, 0x6894, 0x4089, {0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5}};
static const GUID k_iid_audio_client = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID k_iid_capture_client = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const PROPERTYKEY k_friendly_name = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

#define STREAM_LOOPBACK 0x00020000u        // AUDCLNT_STREAMFLAGS_LOOPBACK: record what an output device plays
#define STREAM_AUTOCONVERT 0x80000000u     // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM: Windows converts to our format
#define STREAM_SRC_QUALITY 0x08000000u     // AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define BUFFER_100NS 10000000              // 1s: the capture thread may block on a full pipe for a while

static HANDLE g_thread = NULL;
static HANDLE g_pipe_write = NULL;
static volatile int g_running = 0;

static wchar_t g_helper_path[PATH_CAPACITY];
int line_in_linux_helper(void) {
    // linux_radio.py keeps this file in the mod's folder while it runs (no Wine fingerprinting here)
    return g_helper_path[0] && GetFileAttributesW(g_helper_path) != INVALID_FILE_ATTRIBUTES;
}

int line_in_name_matches(const char *name, const char *wanted) {
    if (!wanted[0]) {
        return 0;
    }
    for (; *name; ++name) {
        size_t i = 0;
        for (; wanted[i] && name[i]; ++i) {
            char a = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
            char b = wanted[i] >= 'A' && wanted[i] <= 'Z' ? (char)(wanted[i] + 32) : wanted[i];
            if (a != b) {
                break;
            }
        }
        if (!wanted[i]) {
            return 1;
        }
    }
    return 0;
}

/* Returns the first active device whose name matches `wanted` (AddRef'd), with *render set for an
   output device; render_only skips input devices. `list`: log every device. Its name: g_found_name. */
static char g_found_name[512];
static IMMDevice *find_device(IMMDeviceEnumerator *enumerator, const char *wanted, int render_only, int list, int *render) {
    IMMDeviceCollection *devices = NULL;
    UINT count = 0;
    IMMDevice *found = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eAll, DEVICE_STATE_ACTIVE, &devices))) {
        return NULL;
    }
    IMMDeviceCollection_GetCount(devices, &count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = NULL;
        IMMEndpoint *endpoint = NULL;
        IPropertyStore *store = NULL;
        EDataFlow flow = eRender;
        static char name[512];
        name[0] = 0;
        if (FAILED(IMMDeviceCollection_Item(devices, i, &device))) {
            continue;
        }
        if (SUCCEEDED(IMMDevice_QueryInterface(device, &k_iid_endpoint, (void **)&endpoint))) {
            IMMEndpoint_GetDataFlow(endpoint, &flow);
            IMMEndpoint_Release(endpoint);
        }
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
            PROPVARIANT value;
            memory_copy(&value, &(PROPVARIANT){0}, sizeof(value));
            if (SUCCEEDED(IPropertyStore_GetValue(store, &k_friendly_name, &value)) && value.vt == VT_LPWSTR) {
                int n = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name, sizeof(name), NULL, NULL);
                name[n > 0 ? n - 1 : 0] = 0;
            }
            PropVariantClear(&value);
            IPropertyStore_Release(store);
        }
        if (list) {
            log_append(flow == eRender ? "LINE-IN: output device: " : "LINE-IN: input device: ");
            log_append(name);
            log_append("\r\n");
        }
        if (!found && (!render_only || flow == eRender) && line_in_name_matches(name, wanted)) {
            found = device;
            *render = flow == eRender;
            memory_copy(g_found_name, name, sizeof(g_found_name));
        } else {
            IMMDevice_Release(device);
        }
    }
    IMMDeviceCollection_Release(devices);
    return found;
}

static int write_all(const void *data, DWORD size) {
    DWORD written;
    return WriteFile(g_pipe_write, data, size, &written, NULL) && written == size;
}

static void capture(IMMDevice *device, int render) {
    IAudioClient *client = NULL;
    IAudioCaptureClient *capture_client = NULL;
    static unsigned char silence[4096];
    // The bridge's own format, so the rest of the radio is the same as with librespot.
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    DWORD flags = STREAM_AUTOCONVERT | STREAM_SRC_QUALITY | (render ? STREAM_LOOPBACK : 0);
    HRESULT result = IMMDevice_Activate(device, &k_iid_audio_client, CLSCTX_ALL, NULL, (void **)&client);
    if (SUCCEEDED(result)) {
        result = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_100NS, 0, &format, NULL);
    }
    if (SUCCEEDED(result)) {
        result = IAudioClient_GetService(client, &k_iid_capture_client, (void **)&capture_client);
    }
    if (SUCCEEDED(result)) {
        result = IAudioClient_Start(client);
    }
    if (FAILED(result)) {
        log_append_value("LINE-IN: the device could not be opened; HRESULT ", (int)result);
    } else {
        log_append(render ? "LINE-IN: capturing what the output device plays.\r\n" : "LINE-IN: capturing the input device.\r\n");
    }
    // A loopback device sends no packets while nothing plays on it: the radio fades out, as on a pause.
    while (SUCCEEDED(result) && g_running) {
        Sleep(10);
        UINT32 packet = 0;
        while (g_running && SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture_client, &packet)) && packet) {
            BYTE *data;
            UINT32 frames;
            DWORD buffer_flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(capture_client, &data, &frames, &buffer_flags, NULL, NULL))) {
                break;
            }
            int ok = 1;
            if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                for (DWORD left = frames * 4; ok && left; ) {
                    DWORD chunk = left < sizeof(silence) ? left : sizeof(silence);
                    ok = write_all(silence, chunk);
                    left -= chunk;
                }
            } else {
                ok = write_all(data, frames * 4);
            }
            IAudioCaptureClient_ReleaseBuffer(capture_client, frames);
            if (!ok) { // the bridge closed its end: the game is shutting down
                g_running = 0;
            }
        }
    }
    if (capture_client) {
        IAudioCaptureClient_Release(capture_client);
    }
    if (client) {
        IAudioClient_Stop(client);
        IAudioClient_Release(client);
    }
}

static DWORD WINAPI capture_thread(LPVOID param) {
    (void)param;
    IMMDeviceEnumerator *enumerator = NULL;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        log_append("LINE-IN: COM could not start; no sound devices.\r\n");
        return 0;
    }
    if (SUCCEEDED(CoCreateInstance(&k_device_enumerator, NULL, CLSCTX_ALL, &k_iid_device_enumerator, (void **)&enumerator))) {
        int render = 0;
        // Playback devices only, through their loopback: the player plays into one, and its media session
        // is found there. (Wine also hung on its second frame capturing a PulseAudio monitor input from
        // the game's start.)
        IMMDevice *device = find_device(enumerator, line_in_device, 1, 1, &render);
        const char *at = device ? NULL : text_find(line_in_device, text_length(line_in_device), "Output");
        if (at && (size_t)(at - line_in_device) + text_length(at) < sizeof(line_in_device) - 1) {
            // an older pick, a cable's recording side ("CABLE Output"): the side players play into, "CABLE Input"
            static char twin[256];
            size_t cut = (size_t)(at - line_in_device);
            memory_copy(twin, line_in_device, cut);
            memory_copy(twin + cut, "Input", 5);
            memory_copy(twin + cut + 5, at + 6, text_length(at + 6) + 1);
            device = find_device(enumerator, twin, 1, 0, &render);
            if (device) {
                memory_copy(line_in_device, g_found_name, sizeof(line_in_device) - 1); // the menu shows it picked
                line_in_device[sizeof(line_in_device) - 1] = 0;
            }
        }
        LPWSTR id = NULL;
        if (device && SUCCEEDED(IMMDevice_GetId(device, &id))) {
            size_t n = 0;
            for (; id[n] && n < ARRAY_COUNT(line_in_render_id) - 1; ++n) line_in_render_id[n] = id[n];
            line_in_render_id[n] = 0;
            CoTaskMemFree(id);
            if (!line_in_linux_helper()) { // on Linux there are no media sessions: linux_radio.py reads MPRIS
                line_in_player_start();
            }
        }
        if (!device) {
            log_append(line_in_device[0] ? "LINE-IN: the picked device is not connected; the radio is silent.\r\n"
                                         : "LINE-IN: pick a device in the game's Audio options.\r\n");
        } else {
            capture(device, render);
            IMMDevice_Release(device);
        }
        IMMDeviceEnumerator_Release(enumerator);
    } else {
        log_append("LINE-IN: the sound device list is unavailable.\r\n");
    }
    CoUninitialize();
    return 0;
}

/* The device list for the in-game menu: every playback device but the default one, which the game itself
   plays to (its loopback would feed the radio back in). */
static wchar_t (*g_list)[128];
static unsigned g_list_capacity, g_list_count;
static DWORD WINAPI list_thread(LPVOID param) {
    (void)param;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *devices = NULL;
    IMMDevice *speakers = NULL;
    LPWSTR speakers_id = NULL;
    UINT count = 0;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        return 0;
    }
    if (SUCCEEDED(CoCreateInstance(&k_device_enumerator, NULL, CLSCTX_ALL, &k_iid_device_enumerator, (void **)&enumerator)) &&
        SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE, &devices))) {
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &speakers))) {
            IMMDevice_GetId(speakers, &speakers_id);
            IMMDevice_Release(speakers);
        }
        IMMDeviceCollection_GetCount(devices, &count);
        for (UINT i = 0; i < count && g_list_count < g_list_capacity; ++i) {
            IMMDevice *device = NULL;
            IPropertyStore *store = NULL;
            LPWSTR id = NULL;
            if (FAILED(IMMDeviceCollection_Item(devices, i, &device))) {
                continue;
            }
            int is_speakers = speakers_id && SUCCEEDED(IMMDevice_GetId(device, &id)) && id;
            for (size_t c = 0; is_speakers && (id[c] || speakers_id[c]); ++c) {
                is_speakers = id[c] == speakers_id[c];
            }
            if (id) CoTaskMemFree(id);
            if (!is_speakers && SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
                PROPVARIANT value;
                memory_copy(&value, &(PROPVARIANT){0}, sizeof(value));
                if (SUCCEEDED(IPropertyStore_GetValue(store, &k_friendly_name, &value)) && value.vt == VT_LPWSTR) {
                    size_t n = 0;
                    for (; value.pwszVal[n] && n < 127; ++n) g_list[g_list_count][n] = value.pwszVal[n];
                    g_list[g_list_count][n] = 0;
                    g_list_count += n > 0;
                }
                PropVariantClear(&value);
                IPropertyStore_Release(store);
            }
            IMMDevice_Release(device);
        }
    }
    if (speakers_id) CoTaskMemFree(speakers_id);
    if (devices) IMMDeviceCollection_Release(devices);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize();
    return 0;
}

unsigned line_in_list_devices(wchar_t names[][128], unsigned capacity) {
    g_list = names; // COM on a thread of its own: the game's UI thread may not have it
    g_list_capacity = capacity;
    g_list_count = 0;
    HANDLE thread = CreateThread(NULL, 0, list_thread, NULL, 0, NULL);
    if (thread) {
        WaitForSingleObject(thread, 3000);
        CloseHandle(thread);
    }
    return g_list_count;
}

/* The source the menu picked, kept in source.txt beside the config (the player's choice, not a
   setting to edit): "Source=Songs" or "Source=Device" with "Device=<name>", and "Shuffle=0/1". */
static wchar_t g_folder[PATH_CAPACITY], g_state_path[PATH_CAPACITY];

static void save_state(void) {
    static char text[512];
    size_t n = 0;
    const char *parts[] = {line_in_use_songs ? "Source=Songs\r\n" : "Source=Device\r\nDevice=",
                           line_in_use_songs ? "" : line_in_device, line_in_use_songs ? "" : "\r\n",
                           line_in_shuffle ? "Shuffle=1\r\n" : "Shuffle=0\r\n"};
    for (unsigned p = 0; p < ARRAY_COUNT(parts); ++p) {
        for (const char *c = parts[p]; *c && n < sizeof(text) - 1; ++c) text[n++] = *c;
    }
    HANDLE file = CreateFileW(g_state_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, text, (DWORD)n, &written, NULL);
        CloseHandle(file);
    }
}

static int starts_with(const char *text, const char *prefix) {
    while (*prefix && *text == *prefix) ++text, ++prefix;
    return !*prefix;
}

void line_in_state_parse(char *text) {
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        if (starts_with(line, "Source=")) {
            line_in_use_songs = !text_equal_ignore_case(line + 7, "Device");
        } else if (starts_with(line, "Device=")) {
            size_t n = 0;
            for (const char *c = line + 7; *c && n < sizeof(line_in_device) - 1; ++c) line_in_device[n++] = *c;
            line_in_device[n] = 0;
        } else if (starts_with(line, "Shuffle=")) {
            line_in_shuffle = line[8] != '0';
        }
        *end = saved;
        line = *end ? end + 1 : end;
    }
}

static void load_state(void) {
    static char text[512];
    DWORD got = 0;
    HANDLE file = CreateFileW(g_state_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return; // never picked in the menu: an old config's [Audio] (or the defaults) decides
    }
    ReadFile(file, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(file);
    text[got] = 0;
    line_in_device[0] = 0; // the menu's pick replaces an old config's Device, even with the Songs folder picked
    line_in_state_parse(text);
}

static void start_source(void) {
    // the last source's song must not stay on the HUD: the station's name until the new source has one
    line_in_now_playing_write("Line-in Radio\r\n");
    if (line_in_use_songs) {
        line_in_songs_start(g_pipe_write, g_folder);
        return;
    }
    g_running = 1;
    g_thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!g_thread) {
        g_running = 0;
    }
}

static void stop_source(void) {
    line_in_songs_stop();
    line_in_player_stop();
    line_in_render_id[0] = 0;
    g_running = 0;
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

int line_in_capture_start(HANDLE pipe_write, const wchar_t *folder) { // owns pipe_write from here
    size_t n = 0;
    for (; folder[n] && n < PATH_CAPACITY - 1; ++n) g_folder[n] = folder[n];
    g_folder[n] = 0;
    path_join(line_in_now_playing_path, folder, L"nowplaying.txt");
    path_join(line_in_command_path, folder, L"command.txt");
    path_join(g_state_path, folder, L"source.txt");
    load_state();
    path_join(g_helper_path, folder, L"linux_radio.running");
    if (!line_in_device[0] && line_in_linux_helper()) {
        memory_copy(line_in_device, LINE_IN_LINUX_DEVICE, sizeof(LINE_IN_LINUX_DEVICE));
    }
    if (line_in_use_songs && line_in_device[0] && !line_in_songs_count(folder)) {
        log_append("LINE-IN: the Songs folder is empty; playing the Device.\r\n");
        line_in_use_songs = 0;
    }
    g_pipe_write = pipe_write;
    start_source();
    return 1;
}

static HANDLE g_switch = NULL;
static DWORD WINAPI switch_thread(LPVOID param) {
    (void)param;
    stop_source();
    save_state();
    log_append(line_in_use_songs ? "LINE-IN: switched to the Songs folder.\r\n" : "LINE-IN: switched to a device: ");
    if (!line_in_use_songs) {
        log_append(line_in_device);
        log_append("\r\n");
    }
    start_source();
    return 0;
}

void line_in_select(const char *device) {
    if (g_switch) { // one switch at a time: the menu may scroll through several quickly
        WaitForSingleObject(g_switch, 5000);
        CloseHandle(g_switch);
        g_switch = NULL;
    }
    // the pick shows in the menu at once; stopping one source and starting the other runs behind it
    line_in_use_songs = device == NULL;
    size_t n = 0;
    for (; device && device[n] && n < sizeof(line_in_device) - 1; ++n) line_in_device[n] = device[n];
    if (device) line_in_device[n] = 0;
    g_switch = CreateThread(NULL, 0, switch_thread, NULL, 0, NULL);
}

void line_in_set_shuffle(int on) {
    line_in_shuffle = on;
    save_state();
}

void line_in_capture_stop(void) {
    if (g_switch) {
        WaitForSingleObject(g_switch, 5000);
        CloseHandle(g_switch);
        g_switch = NULL;
    }
    stop_source();
    if (g_pipe_write) {
        CloseHandle(g_pipe_write);
        g_pipe_write = NULL;
    }
}
