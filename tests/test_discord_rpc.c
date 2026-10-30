/* Discord RPC: a packet (header + JSON) leaves in a single write; Vesktop's arRPC drops the
   connection when the header arrives without its body. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int writes;
static unsigned char sent[2048];
static DWORD sent_size;
static BOOL WINAPI counting_write(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    (void)file; (void)overlapped;
    writes++;
    if (size <= sizeof(sent)) { memcpy(sent, data, size); sent_size = size; }
    *written = size;
    return TRUE;
}
#define WriteFile counting_write
#include "discord_rpc.c"
#undef WriteFile

void log_append(const char *message) { (void)message; }

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    const char *json = "{\"v\":1,\"client_id\":\"1\"}";
    CHECK(rpc_send_packet(NULL, 0, json) == 1, "send: a packet is sent");
    CHECK(writes == 1, "send: header and body go out in one write");
    uint32_t head[2];
    memcpy(head, sent, sizeof(head));
    CHECK(sent_size == 8 + strlen(json) && head[0] == 0 && head[1] == strlen(json) && memcmp(sent + 8, json, strlen(json)) == 0,
          "send: the write is the header followed by the JSON");

    static char big[1100];
    memset(big, 'x', sizeof(big) - 1);
    writes = 0;
    CHECK(rpc_send_packet(NULL, 1, big) == 0 && writes == 0, "send: a payload over 1024 bytes is refused");
    char activity[1024];
    g_game_name = "Forza Horizon 3";
    rpc_build_activity_json(activity, sizeof(activity), 7, 100);
    CHECK(strcmp(activity, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":7,\"activity\":{\"timestamps\":{\"start\":100},"
                           "\"assets\":{\"large_image\":\"default\",\"large_text\":\"Forza Horizon 3\"}}},\"nonce\":\"1\"}") == 0,
          "activity: the running game's name, valid JSON");

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
