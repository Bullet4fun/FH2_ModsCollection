#include "game_input.h"
#include "autodrive_game.h"
#include "logging.h"

typedef SHORT(WINAPI *KeyState)(int);
typedef HWND(WINAPI *ForegroundWindow)(void);
typedef DWORD(WINAPI *WindowProcess)(HWND, LPDWORD);

static KeyState key_state;
static ForegroundWindow foreground_window;
static WindowProcess window_process;
static int input_initialized;

static int initialize_input(void) {
    if (!input_initialized) {
        input_initialized = 1;
        key_state = GetAsyncKeyState;
        foreground_window = GetForegroundWindow;
        window_process = GetWindowThreadProcessId;
        if (!key_state || !foreground_window || !window_process) {
            log_append("AUTODRIVE: keyboard API unavailable; F2 is inactive.\r\n");
        }
    }
    return key_state && foreground_window && window_process;
}

int autodrive_game_window_focused(void) {
    if (!initialize_input()) {
        return 0;
    }
    HWND window = foreground_window();
    DWORD process_id = 0;
    if (window) {
        window_process(window, &process_id);
    }
    return process_id == GetCurrentProcessId();
}

KeyboardInput game_input_read(void) {
    KeyboardInput input = {0};
    if (initialize_input()) {
        input.focused = autodrive_game_window_focused();
        input.toggle = (key_state(VK_F2) & 0x8000) != 0;
        input.cancel = (key_state(VK_ESCAPE) & 0x8000) != 0;
    }
    return input;
}
