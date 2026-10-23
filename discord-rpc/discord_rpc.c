#include "discord_rpc.h"
#include "logging.h"
#include "support.h"

#pragma pack(push, 1)
typedef struct {
    uint32_t opcode;
    uint32_t length;
} RpcHeader;
#pragma pack(pop)

static volatile int g_rpc_running = 0;
static HANDLE g_rpc_thread = NULL;
static HANDLE g_rpc_pipe = INVALID_HANDLE_VALUE;
static const char *g_client_id = ""; // the Discord application shown as the game

static size_t rpc_str_len(const char *s) {
    size_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

static void rpc_str_copy(char *dst, const char *src, size_t *offset, size_t capacity) {
    if (!dst || !src || *offset >= capacity) return;
    size_t i = 0;
    while (src[i] && (*offset + 1) < capacity) {
        dst[*offset] = src[i];
        (*offset)++;
        i++;
    }
    dst[*offset] = '\0';
}

static void rpc_u64_to_str(char *dst, uint64_t val, size_t *offset, size_t capacity) {
    if (val == 0) {
        if (*offset + 1 < capacity) {
            dst[(*offset)++] = '0';
            dst[*offset] = '\0';
        }
        return;
    }
    char buf[24];
    int idx = 0;
    while (val > 0 && idx < 23) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int i = idx - 1; i >= 0; --i) {
        if (*offset + 1 < capacity) {
            dst[(*offset)++] = buf[i];
        }
    }
    dst[*offset] = '\0';
}

static uint64_t rpc_get_unix_time(void) {
    ULARGE_INTEGER ft;
    GetSystemTimeAsFileTime((FILETIME *)&ft);
    return (ft.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

static int rpc_send_packet(HANDLE pipe, uint32_t opcode, const char *json_payload) {
    // One write per packet: Vesktop's arRPC drops the connection when the header arrives without its body
    unsigned char packet[sizeof(RpcHeader) + 1024];
    RpcHeader header;
    header.opcode = opcode;
    header.length = (uint32_t)rpc_str_len(json_payload);
    if (header.length > sizeof(packet) - sizeof(header)) {
        return 0;
    }
    memory_copy(packet, &header, sizeof(header));
    memory_copy(packet + sizeof(header), json_payload, header.length);

    DWORD size = (DWORD)(sizeof(header) + header.length);
    DWORD written = 0;
    return WriteFile(pipe, packet, size, &written, NULL) && written == size;
}

static int rpc_read_packet(HANDLE pipe, uint32_t *opcode, char *buffer, uint32_t buffer_size) {
    RpcHeader header;
    DWORD read_bytes = 0;
    if (!ReadFile(pipe, &header, sizeof(header), &read_bytes, NULL) || read_bytes != sizeof(header)) {
        return 0;
    }
    if (opcode) {
        *opcode = header.opcode;
    }
    if (header.length > 0) {
        uint32_t to_read = header.length;
        if (to_read >= buffer_size) {
            to_read = buffer_size - 1;
        }
        if (!ReadFile(pipe, buffer, to_read, &read_bytes, NULL) || read_bytes != to_read) {
            return 0;
        }
        buffer[read_bytes] = '\0';
        if (header.length > to_read) {
            uint32_t remaining = header.length - to_read;
            char discard[64];
            while (remaining > 0) {
                DWORD chunk = (remaining > sizeof(discard)) ? (DWORD)sizeof(discard) : remaining;
                if (!ReadFile(pipe, discard, chunk, &read_bytes, NULL) || read_bytes == 0) {
                    break;
                }
                remaining -= read_bytes;
            }
        }
    } else if (buffer && buffer_size > 0) {
        buffer[0] = '\0';
    }
    return 1;
}

typedef struct {
    unsigned short family; // AF_UNIX = 1
    char path[108];
} RpcSockaddrUn;

// Under Wine/Proton, Discord runs as a Linux app and listens on a Unix socket, not on
// \\.\pipe\discord-ipc-N. Wine's Winsock has AF_UNIX and takes a Z:\ path; ReadFile/WriteFile/
// CloseHandle work on its socket handles. XDG_RUNTIME_DIR does not reach Windows programs, so every
// Z:\run\user\<uid> is tried, with the Flatpak Discord/Vesktop folders. ws2_32 is loaded only under Wine.
static HANDLE rpc_connect_unix(void) {
    // Winsock without its headers (freestanding build): a SOCKET is a UINT_PTR, ~0 when invalid.
    static int (WINAPI *wsa_startup)(WORD, void *);
    static UINT_PTR (WINAPI *wsa_socket)(int, int, int, void *, unsigned int, DWORD);
    static int (WINAPI *sock_connect)(UINT_PTR, const void *, int);
    static int (WINAPI *sock_close)(UINT_PTR);
    if (!wsa_startup) {
        if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
            return INVALID_HANDLE_VALUE;
        }
        HMODULE ws = LoadLibraryExW(L"ws2_32.dll", NULL, 0);
        static unsigned char wsa[512]; // WSADATA (408 bytes on x64)
        if (!ws) {
            return INVALID_HANDLE_VALUE;
        }
        *(FARPROC *)&wsa_socket = GetProcAddress(ws, "WSASocketW");
        *(FARPROC *)&sock_connect = GetProcAddress(ws, "connect");
        *(FARPROC *)&sock_close = GetProcAddress(ws, "closesocket");
        *(FARPROC *)&wsa_startup = GetProcAddress(ws, "WSAStartup");
        if (!wsa_startup || !wsa_socket || !sock_connect || !sock_close || wsa_startup(MAKEWORD(2, 2), wsa) != 0) {
            wsa_startup = NULL;
            return INVALID_HANDLE_VALUE;
        }
    }
    static const char *const folders[] = {"", "app\\com.discordapp.Discord\\", "app\\dev.vencord.Vesktop\\",
                                          "app\\com.discordapp.DiscordCanary\\", "snap.discord\\"};
    WIN32_FIND_DATAW user;
    HANDLE find = FindFirstFileW(L"Z:\\run\\user\\*", &user);
    if (find == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE result = INVALID_HANDLE_VALUE;
    do {
        char uid[16];
        size_t u = 0;
        while (u < sizeof(uid) - 1 && user.cFileName[u] >= L'0' && user.cFileName[u] <= L'9') {
            uid[u] = (char)user.cFileName[u];
            ++u;
        }
        uid[u] = '\0';
        if (u == 0 || user.cFileName[u] || !(user.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue; // not a numeric uid folder
        }
        for (size_t f = 0; f < sizeof(folders) / sizeof(folders[0]) && result == INVALID_HANDLE_VALUE; ++f) {
            for (int i = 0; i < 10 && result == INVALID_HANDLE_VALUE; ++i) {
                RpcSockaddrUn address = {1, {0}};
                size_t off = 0;
                char index[2] = {(char)('0' + i), '\0'};
                rpc_str_copy(address.path, "Z:\\run\\user\\", &off, sizeof(address.path));
                rpc_str_copy(address.path, uid, &off, sizeof(address.path));
                rpc_str_copy(address.path, "\\", &off, sizeof(address.path));
                rpc_str_copy(address.path, folders[f], &off, sizeof(address.path));
                rpc_str_copy(address.path, "discord-ipc-", &off, sizeof(address.path));
                rpc_str_copy(address.path, index, &off, sizeof(address.path));
                wchar_t wide[sizeof(address.path)];
                for (size_t c = 0; c <= off; ++c) {
                    wide[c] = (wchar_t)address.path[c]; // ASCII only
                }
                if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES &&
                    (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
                    continue; // skip missing paths; Wine answers ERROR_INVALID_PARAMETER for a live socket file
                }
                UINT_PTR s = wsa_socket(1, 1, 0, NULL, 0, 0); // AF_UNIX, SOCK_STREAM, not overlapped: plain ReadFile/WriteFile
                if (s == ~(UINT_PTR)0) {
                    continue;
                }
                if (sock_connect(s, &address, sizeof(address)) == 0) {
                    result = (HANDLE)s;
                } else {
                    sock_close(s);
                }
            }
        }
    } while (result == INVALID_HANDLE_VALUE && FindNextFileW(find, &user));
    FindClose(find);
    return result;
}

static HANDLE rpc_connect_pipe(void) {
    // \\.\pipe\discord-ipc-0 length is 22 chars; index 21 is '0'
    wchar_t pipe_name[] = L"\\\\.\\pipe\\discord-ipc-0";
    for (int i = 0; i < 10; ++i) {
        pipe_name[21] = (wchar_t)(L'0' + i);
        HANDLE pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
    }
    return rpc_connect_unix();
}

static void rpc_build_activity_json(char *output, size_t capacity, uint32_t pid, uint64_t start_time) {
    size_t off = 0;
    rpc_str_copy(output, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":", &off, capacity);
    rpc_u64_to_str(output, (uint64_t)pid, &off, capacity);
    rpc_str_copy(output, ",\"activity\":{\"timestamps\":{\"start\":", &off, capacity);
    rpc_u64_to_str(output, start_time, &off, capacity);
    rpc_str_copy(output, "},\"assets\":{\"large_image\":\"default\",\"large_text\":\"Forza Horizon 2\"}}},\"nonce\":\"1\"}", &off, capacity);
}

static DWORD WINAPI rpc_worker_thread(LPVOID param) {
    (void)param;
    char read_buffer[1024];
    char json_buffer[1024];
    char handshake[96];
    size_t length = 0;
    rpc_str_copy(handshake, "{\"v\":1,\"client_id\":\"", &length, sizeof(handshake));
    rpc_str_copy(handshake, g_client_id, &length, sizeof(handshake));
    rpc_str_copy(handshake, "\"}", &length, sizeof(handshake));
    uint64_t start_time = rpc_get_unix_time();
    DWORD pid = GetCurrentProcessId();

    while (g_rpc_running) {
        g_rpc_pipe = rpc_connect_pipe();
        if (g_rpc_pipe == INVALID_HANDLE_VALUE) {
            for (int i = 0; i < 4 && g_rpc_running; ++i) {
                Sleep(500);
            }
            continue;
        }

        // 1. Send Handshake
        if (!rpc_send_packet(g_rpc_pipe, 0, handshake)) {
            CloseHandle(g_rpc_pipe);
            g_rpc_pipe = INVALID_HANDLE_VALUE;
            Sleep(1000);
            continue;
        }

        // 2. Read Handshake Response (READY event)
        uint32_t resp_opcode = 0;
        if (!rpc_read_packet(g_rpc_pipe, &resp_opcode, read_buffer, sizeof(read_buffer))) {
            CloseHandle(g_rpc_pipe);
            g_rpc_pipe = INVALID_HANDLE_VALUE;
            Sleep(1000);
            continue;
        }

        // 3. Send initial activity
        rpc_build_activity_json(json_buffer, sizeof(json_buffer), pid, start_time);
        if (!rpc_send_packet(g_rpc_pipe, 1, json_buffer)) {
            CloseHandle(g_rpc_pipe);
            g_rpc_pipe = INVALID_HANDLE_VALUE;
            Sleep(1000);
            continue;
        }

        // Read activity response
        rpc_read_packet(g_rpc_pipe, &resp_opcode, read_buffer, sizeof(read_buffer));

        // 4. Connection maintenance loop
        while (g_rpc_running) {
            for (int i = 0; i < 20 && g_rpc_running; ++i) {
                Sleep(500);
            }
            if (!g_rpc_running) break;

            // Refresh activity frame
            rpc_build_activity_json(json_buffer, sizeof(json_buffer), pid, start_time);
            if (!rpc_send_packet(g_rpc_pipe, 1, json_buffer)) {
                // Discord closed or pipe broken
                break;
            }
            // Read response
            rpc_read_packet(g_rpc_pipe, &resp_opcode, read_buffer, sizeof(read_buffer));
        }

        if (g_rpc_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_rpc_pipe);
            g_rpc_pipe = INVALID_HANDLE_VALUE;
        }
    }

    return 0;
}

void discord_rpc_start(const char *client_id) {
    if (g_rpc_running) return;
    g_client_id = client_id;
    g_rpc_running = 1;
    g_rpc_thread = CreateThread(NULL, 0, rpc_worker_thread, NULL, 0, NULL);
}

void discord_rpc_stop(void) {
    if (!g_rpc_running) return;
    g_rpc_running = 0;
    if (g_rpc_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_rpc_pipe);
        g_rpc_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_rpc_thread) {
        CloseHandle(g_rpc_thread);
        g_rpc_thread = NULL;
    }
}
