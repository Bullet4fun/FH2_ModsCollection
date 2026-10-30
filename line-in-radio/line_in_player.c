#define COBJMACROS
#include "line_in_capture.h"
#include "track_controls.h"
#include "logging.h"
#include "support.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>

/* Source = Device: which player plays into the device. The apps playing on the device's render side
   are its active audio sessions (their process ids); the one whose Windows media session (SMTC, the
   media overlay) belongs to the same program gives the HUD its title and artist and takes the track
   buttons, so only that player reacts. No such session: the buttons do nothing. */

static const GUID k_iid_session_manager2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
static const GUID k_iid_session_control2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
static const GUID k_iid_device_enumerator2 = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID k_device_enumerator2 = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID k_iid_async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// Windows.Media.Control (Windows SDK windows.media.control.h)
static const GUID k_iid_smtc_statics = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};

/* WinRT objects through their vtables: IInspectable takes slots 0-5, an interface's own methods
   follow in the SDK header's order. */
typedef HRESULT(WINAPI *OutObject)(void *self, void **out);
typedef HRESULT(WINAPI *OutInt)(void *self, int *out);
typedef HRESULT(WINAPI *OutAt)(void *self, UINT32 index, void **out);
#define METHOD(object, slot, type) ((type)(*(void ***)(object))[slot])
#define RELEASE(object) ((ULONG(WINAPI *)(void *))(*(void ***)(object))[2])(object)
enum {
    STATICS_REQUEST_ASYNC = 6,
    MANAGER_GET_SESSIONS = 7,
    SESSION_APP_ID = 6, SESSION_MEDIA_PROPERTIES = 7, SESSION_PLAYBACK_INFO = 9,
    SESSION_SKIP_NEXT = 16, SESSION_SKIP_PREVIOUS = 17, SESSION_TOGGLE_PLAY_PAUSE = 20,
    PROPERTIES_TITLE = 6, PROPERTIES_ARTIST = 9,
    PLAYBACK_STATUS = 7, STATUS_PLAYING = 4,
    VECTOR_GET_AT = 6, VECTOR_SIZE = 7,
    OPERATION_GET_RESULTS = 8,
};

#define TITLE_DELAY_MS 300

wchar_t line_in_render_id[512]; // the device the players play into (set by the capture thread)

static HANDLE g_session_thread = NULL;
static volatile int g_watching = 0;
static volatile LONG g_command = 0;     // TRACK_* for the player thread to send
static volatile int g_have_session = 0; // a matched media session: the track buttons go to it

/* Waits for an IAsyncOperation (up to 2s) and returns its result, AddRef'd; releases the operation. */
static void *await_result(void *operation) {
    void *result = NULL;
    IAsyncInfo *info = NULL;
    if (!operation) {
        return NULL;
    }
    if (SUCCEEDED(((HRESULT(WINAPI *)(void *, const GUID *, void **))(*(void ***)operation)[0])(operation, &k_iid_async_info, (void **)&info))) {
        AsyncStatus status = Started;
        for (int i = 0; i < 400 && SUCCEEDED(IAsyncInfo_get_Status(info, &status)) && status == Started; ++i) {
            Sleep(5);
        }
        if (status == Completed) {
            METHOD(operation, OPERATION_GET_RESULTS, OutObject)(operation, &result);
        }
        IAsyncInfo_Release(info);
    }
    RELEASE(operation);
    return result;
}

static void hstring_to_utf8(HSTRING text, char *out, size_t capacity, size_t *used) {
    UINT32 length = 0;
    const wchar_t *raw = WindowsGetStringRawBuffer(text, &length);
    int n = length ? WideCharToMultiByte(CP_UTF8, 0, raw, (int)length, out + *used, (int)(capacity - *used - 3), NULL, NULL) : 0;
    *used += n > 0 ? (size_t)n : 0;
    out[(*used)++] = '\r';
    out[(*used)++] = '\n';
    out[*used] = 0;
}

/* A session's program from its identifier, which Windows builds from the program's path:
   "{device}.{guid}|\Device\HarddiskVolume3\...\foobar2000.exe%b{guid}" -> "foobar2000" (lower case).
   No need to open the player's process. 0: no program in it (the system sounds' session). */
unsigned line_in_session_program(const wchar_t *identifier, wchar_t out[64]) {
    const wchar_t *path = identifier, *name = NULL;
    while (*path && *path != L'|') ++path;
    for (const wchar_t *c = path; *c && !(c[0] == L'%' && c[1] == L'b'); ++c) {
        if (*c == L'\\') name = c + 1;
    }
    unsigned n = 0;
    for (; name && name[n] && name[n] != L'.' && name[n] != L'%' && n < 63; ++n) {
        out[n] = name[n] >= L'A' && name[n] <= L'Z' ? name[n] + 32 : name[n];
    }
    out[n] = 0;
    return n;
}

/* The program names (no .exe, lower case) of the apps playing on the render device right now. */
static unsigned playing_programs(IMMDevice *device, wchar_t names[][64], unsigned capacity) {
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionEnumerator *sessions = NULL;
    unsigned found = 0;
    int count = 0;
    if (FAILED(IMMDevice_Activate(device, &k_iid_session_manager2, CLSCTX_ALL, NULL, (void **)&manager))) {
        return 0;
    }
    if (SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions))) {
        IAudioSessionEnumerator_GetCount(sessions, &count);
        for (int i = 0; i < count && found < capacity; ++i) {
            IAudioSessionControl *control = NULL;
            IAudioSessionControl2 *control2 = NULL;
            AudioSessionState state = AudioSessionStateInactive;
            LPWSTR identifier = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) {
                continue;
            }
            if (SUCCEEDED(IAudioSessionControl_GetState(control, &state)) && state == AudioSessionStateActive &&
                SUCCEEDED(IAudioSessionControl_QueryInterface(control, &k_iid_session_control2, (void **)&control2))) {
                if (SUCCEEDED(IAudioSessionControl2_GetSessionIdentifier(control2, &identifier)) && identifier) {
                    found += line_in_session_program(identifier, names[found]) > 0;
                    CoTaskMemFree(identifier);
                }
                IAudioSessionControl2_Release(control2);
            }
            IAudioSessionControl_Release(control);
        }
        IAudioSessionEnumerator_Release(sessions);
    }
    IAudioSessionManager2_Release(manager);
    return found;
}

// A media session's app id names its program: "Spotify.exe", "foobar2000.exe", "Chrome", "MSEdge"...
int line_in_app_matches(const wchar_t *app_id, const wchar_t *program) {
    for (; *app_id; ++app_id) {
        size_t i = 0;
        while (program[i] && app_id[i] && (app_id[i] >= L'A' && app_id[i] <= L'Z' ? app_id[i] + 32 : app_id[i]) == program[i]) ++i;
        if (!program[i] && i) {
            return 1;
        }
    }
    return 0;
}

/* The media session of a program in `programs`, AddRef'd, or NULL. A playing one wins. */
static void *find_session(void *manager, wchar_t programs[][64], unsigned program_count) {
    void *list = NULL, *best = NULL;
    UINT32 size = 0;
    if (!program_count || FAILED(METHOD(manager, MANAGER_GET_SESSIONS, OutObject)(manager, &list))) {
        return NULL;
    }
    METHOD(list, VECTOR_SIZE, OutInt)(list, (int *)&size);
    for (UINT32 i = 0; i < size; ++i) {
        void *session = NULL, *info = NULL;
        HSTRING app = NULL;
        int matched = 0, playing = 0;
        if (FAILED(METHOD(list, VECTOR_GET_AT, OutAt)(list, i, &session))) {
            continue;
        }
        if (SUCCEEDED(METHOD(session, SESSION_APP_ID, OutObject)(session, (void **)&app))) {
            const wchar_t *raw = WindowsGetStringRawBuffer(app, NULL);
            for (unsigned p = 0; p < program_count && !matched; ++p) matched = line_in_app_matches(raw, programs[p]);
            WindowsDeleteString(app);
        }
        if (matched && SUCCEEDED(METHOD(session, SESSION_PLAYBACK_INFO, OutObject)(session, &info))) {
            int status = 0;
            METHOD(info, PLAYBACK_STATUS, OutInt)(info, &status);
            playing = status == STATUS_PLAYING;
            RELEASE(info);
        }
        if (matched && (!best || playing)) {
            if (best) RELEASE(best);
            best = session;
            if (playing) break;
        } else {
            RELEASE(session);
        }
    }
    RELEASE(list);
    return best;
}

static void send_command(void *session, int command) {
    int slot = command == TRACK_NEXT ? SESSION_SKIP_NEXT : command == TRACK_PREVIOUS ? SESSION_SKIP_PREVIOUS : SESSION_TOGGLE_PLAY_PAUSE;
    void *operation = NULL;
    if (SUCCEEDED(METHOD(session, slot, OutObject)(session, &operation)) && operation) {
        RELEASE(operation); // the player acts on it; the answer (a bool) isn't needed
    }
}

static DWORD WINAPI player_thread(LPVOID param) {
    (void)param;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    void *statics = NULL, *manager = NULL, *session = NULL;
    HSTRING class_name = NULL;
    static const wchar_t name[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) {
        return 0;
    }
    if (SUCCEEDED(CoCreateInstance(&k_device_enumerator2, NULL, CLSCTX_ALL, &k_iid_device_enumerator2, (void **)&enumerator))) {
        IMMDeviceEnumerator_GetDevice(enumerator, line_in_render_id, &device);
    }
    if (SUCCEEDED(WindowsCreateString(name, (UINT32)(sizeof(name) / sizeof(name[0]) - 1), &class_name)) &&
        SUCCEEDED(RoGetActivationFactory(class_name, &k_iid_smtc_statics, &statics))) {
        void *operation = NULL;
        if (SUCCEEDED(METHOD(statics, STATICS_REQUEST_ASYNC, OutObject)(statics, &operation))) {
            manager = await_result(operation);
        }
    }
    if (!device || !manager) {
        log_append("LINE-IN: the player can't be told apart (no media sessions); the track buttons do nothing.\r\n");
    }
    static char text[1024], shown[1024];
    shown[0] = 0; // a new start (another source was picked): write the song even if it is the same one
    static char pending[1024]; // a new song, shown once its sound has come through the radio's buffers
    static ULONGLONG pending_at;
    pending[0] = 0;
    static wchar_t programs[8][64];
    for (unsigned tick = 0; g_watching && device && manager; ++tick) {
        LONG command = __atomic_exchange_n(&g_command, 0, __ATOMIC_ACQ_REL);
        if (command && session) {
            send_command(session, (int)command);
            tick = 0; // the title changes: look again soon
        }
        if (tick % 5 == 0) { // four times a second: a new song's title is caught within 0.25 s
            // No player playing into the device (paused, or between two tracks; the game or a chat app may
            // still play there): keep the player found last, so play/pause can start it again.
            unsigned playing = playing_programs(device, programs, 8);
            void *found = find_session(manager, programs, playing);
            if (found) {
                if (session) RELEASE(session);
                session = found;
            }
            g_have_session = session != NULL;
            void *properties = NULL, *operation = NULL;
            size_t used = 0;
            text[0] = 0;
            if (session && SUCCEEDED(METHOD(session, SESSION_MEDIA_PROPERTIES, OutObject)(session, &operation))) {
                properties = await_result(operation);
            }
            if (properties) {
                HSTRING title = NULL, artist = NULL;
                if (SUCCEEDED(METHOD(properties, PROPERTIES_TITLE, OutObject)(properties, (void **)&title)) && WindowsGetStringRawBuffer(title, NULL)[0]) {
                    hstring_to_utf8(title, text, sizeof(text), &used);
                    if (SUCCEEDED(METHOD(properties, PROPERTIES_ARTIST, OutObject)(properties, (void **)&artist))) {
                        hstring_to_utf8(artist, text, sizeof(text), &used);
                    }
                }
                WindowsDeleteString(title);
                WindowsDeleteString(artist);
                RELEASE(properties);
            }
            if (text[0] && !memory_equal(text, shown, used + 1) && !memory_equal(text, pending, used + 1)) {
                memory_copy(pending, text, used + 1);
                pending_at = GetTickCount64();
            }
        }
        // The player names the song as it starts it; the radio plays it about a second later (the bridge's
        // ring and the game's own stream buffer). shortcut: a fixed delay, measure it if it ever drifts.
        if (pending[0] && GetTickCount64() - pending_at >= TITLE_DELAY_MS) {
            memory_copy(shown, pending, sizeof(shown));
            line_in_now_playing_write(shown);
            pending[0] = 0;
        }
        Sleep(50);
    }
    g_have_session = 0;
    if (session) RELEASE(session);
    if (manager) RELEASE(manager);
    if (statics) RELEASE(statics);
    WindowsDeleteString(class_name);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    RoUninitialize();
    return 0;
}

int line_in_player_command(int command) {
    // Proton: Wine has no media sessions and its media keys stay inside Wine, so the button goes to
    // command.txt, which linux_radio.py on the Linux side sends to the player's MPRIS.
    if (line_in_linux_helper()) {
        static const char *const names[TRACK_COMMANDS] = {"", "next\n", "previous\n", "playpause\n"};
        HANDLE file = CreateFileW(line_in_command_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, names[command], (DWORD)text_length(names[command]), &written, NULL);
            CloseHandle(file);
            return 1;
        }
    }
    if (!g_have_session) {
        return 0;
    }
    __atomic_store_n(&g_command, command, __ATOMIC_RELEASE);
    return 1;
}

void line_in_player_start(void) {
    if (!g_session_thread && line_in_render_id[0]) {
        g_watching = 1;
        g_session_thread = CreateThread(NULL, 0, player_thread, NULL, 0, NULL);
    }
}

void line_in_player_stop(void) {
    g_watching = 0;
    if (g_session_thread) {
        WaitForSingleObject(g_session_thread, 3000);
        CloseHandle(g_session_thread);
        g_session_thread = NULL;
    }
}
