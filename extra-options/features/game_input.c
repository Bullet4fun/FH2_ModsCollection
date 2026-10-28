#include "game_input.h"
#include "autodrive_game.h"
#include "logging.h"
#include "bindings.h"
#include "support.h"

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
            log_append("INPUT: keyboard API unavailable; keyboard bindings are inactive.\r\n");
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

int game_key_down(unsigned key) {
    if (!key || key > 255) return 0;
    return initialize_input() && autodrive_game_window_focused() && (key_state((int)key)&0x8000)!=0;
}

int game_triggers_down(unsigned triggers) {
    if (!triggers) return 1;
    void *controller;
    unsigned short buttons;
    unsigned char values[2];
    return autodrive_game_buttons(&controller, &buttons) == 0 &&
           memory_read((unsigned char *)controller + 0x70, values, sizeof(values)) &&
           (!(triggers & 1) || values[0]) && (!(triggers & 2) || values[1]);
}

KeyboardInput game_input_read(void) {
    KeyboardInput input={0};
    input.focused=autodrive_game_window_focused();
    input.toggle=binding_key_down(BIND_AUTODRIVE);
    input.cancel=binding_key_down(BIND_CANCEL_AUTODRIVE);
    void *controller;unsigned short buttons;
    if (input.focused && autodrive_game_buttons(&controller,&buttons)==0)
        input.cancel|=binding_pad_down(BIND_CANCEL_AUTODRIVE,buttons);
    return input;
}
