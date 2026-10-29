/* Line-in Radio: [Audio] in its INI, picking a sound device by part of its name, and the song player
   (decoding to the bridge's 44.1 kHz stereo, the title from a file name). Run with a music file's
   path to see how that file decodes. */
#define LINE_IN_RADIO 1
#include "track_controls.c"
#include "line_in_capture.c"
#include "line_in_songs.c"
#include "line_in_player.c"
#include "line_in_menu.c"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static unsigned long long decoded;
static int count_bytes(const void *data, DWORD size) {
    (void)data;
    decoded += size;
    return 1;
}

/* One second of a 440 Hz tone, 48 kHz mono: a rate and a channel count the bridge doesn't use. */
static void write_test_wav(const wchar_t *path) {
    static short samples[48000];
    for (int i = 0; i < 48000; ++i) samples[i] = (short)(8000 * sin(i * 2 * 3.14159265 * 440 / 48000));
    struct { char riff[4]; unsigned size; char wave[4], fmt[4]; unsigned fmt_size; unsigned short format, channels;
             unsigned rate, bytes_per_second; unsigned short align, bits; char data[4]; unsigned data_size; } header = {
        "RIFF", 36 + sizeof(samples), "WAVE", "fmt ", 16, 1, 1, 48000, 96000, 2, 16, "data", sizeof(samples)};
    FILE *file = _wfopen(path, L"wb");
    fwrite(&header, sizeof(header), 1, file);
    fwrite(samples, sizeof(samples), 1, file);
    fclose(file);
}

int main(int argc, char **argv) {
    track_controls_load(L"Z:\\does-not-exist\\");
    CHECK(!line_in_device[0] && line_in_use_songs && line_in_shuffle && track_bindings[TRACK_NEXT][0].buttons == 0x40,
          "defaults: Songs, shuffled, no device, the usual buttons");

    strcpy(ini_text, "[Audio]\r\nSource = Device\r\nShuffle = 0\r\nDevice =  CABLE Output   // the cable\r\n"
                     "[Controls]\r\nNextTrackKey = F8\r\n");
    parse_ini();
    CHECK(!line_in_use_songs && !line_in_shuffle, "Source = Device, Shuffle = 0");
    CHECK(!strcmp(line_in_device, "CABLE Output"), "Device without padding or note");
    CHECK(track_bindings[TRACK_NEXT][1].key == 0x77, "[Controls] still read after [Audio]");

    strcpy(ini_text, "[Controls]\r\nDevice = Speakers\r\n[Audio]\r\nSource = songs\r\n");
    line_in_device[0] = 0;
    parse_ini();
    CHECK(!line_in_device[0] && line_in_use_songs, "keys count only under [Audio]; any case");

    strcpy(ini_text, "[Audio]\r\nDevice = CABLE-B Output\r\n");
    line_in_use_songs = 1;
    parse_ini();
    CHECK(!line_in_use_songs, "a 0.1.0 config (Device, no Source) plays its device");
    strcpy(ini_text, "[Audio]\r\nSource = Songs\r\nDevice = CABLE-B Output\r\n");
    parse_ini();
    CHECK(line_in_use_songs, "Source = Songs wins over a Device left set");
    line_in_device[0] = 0;

    /* the menu's pick (source.txt) */
    static char state[] = "Source=Device\r\nDevice=CABLE-B Output (VB-Audio Cable B)\r\nShuffle=0\r\n";
    line_in_use_songs = line_in_shuffle = 1;
    line_in_state_parse(state);
    CHECK(!line_in_use_songs && !line_in_shuffle && !strcmp(line_in_device, "CABLE-B Output (VB-Audio Cable B)"),
          "source.txt: a device, Shuffle off");
    static char songs_state[] = "Source=Songs\r\nShuffle=1\r\n";
    line_in_state_parse(songs_state);
    CHECK(line_in_use_songs && line_in_shuffle, "source.txt: the Songs folder");
    line_in_device[0] = 0;

    /* the Audio options rows: resources in the dictionary, rows right under Radio Volume */
    static const char page[] =
        "<UserControl x:Class=\"ForzaUI.A_AudioOptions\"><UserControl.Resources><ResourceDictionary><X/>"
        "</ResourceDictionary></UserControl.Resources><local:CommonListBox x:Name=\"OptionsList\" Width=\"688\">"
        "<Slider Value=\"{Binding MusicVolume}\"/><Slider Value=\"{Binding RadioVolume}\" Tag=\"r\"/>"
        "<Slider Value=\"{Binding SfxVolume}\"/></local:CommonListBox></UserControl>";
    static char xaml[8192], again[8192];
    size_t length = line_in_menu_xaml(page, sizeof(page) - 1, xaml, sizeof(xaml));
    const char *source_key = strstr(xaml, "x:Key=\"LI_Source\""), *dictionary_end = strstr(xaml, "</ResourceDictionary>");
    const char *radio = strstr(xaml, "{Binding RadioVolume}\" Tag=\"r\"/>");
    CHECK(length == strlen(xaml) && source_key && source_key < dictionary_end, "the rows' resources go in the dictionary");
    CHECK(radio && strncmp(radio + strlen("{Binding RadioVolume}\" Tag=\"r\"/>"), "<local:SpinnerControl Tag=\"{StaticResource LI_Source}",
                           strlen("<local:SpinnerControl Tag=\"{StaticResource LI_Source}")) == 0,
          "the source row comes right under Radio Volume");
    CHECK(strstr(xaml, "{Binding SfxVolume}") > strstr(xaml, "LineInShuffleChoices"), "the rest of the page follows");
    CHECK(!line_in_menu_xaml(xaml, length, again, sizeof(again)), "rows are added once");
    CHECK(!line_in_menu_xaml("<UserControl x:Class=\"ForzaUI.A_VideoOptions\"/>", 46, again, sizeof(again)),
          "other pages are left alone");
    CHECK(!strstr(xaml, "StaticResource EO_"), "Extra Options still sees a page it can extend");

    CHECK(line_in_name_matches("CABLE Output (VB-Audio Virtual Cable)", "cable output"), "any case");
    CHECK(line_in_name_matches("Speakers (Realtek(R) Audio)", "realtek"), "part of the name");
    CHECK(!line_in_name_matches("Speakers (Realtek(R) Audio)", "cable"), "no match");
    CHECK(!line_in_name_matches("Speakers", ""), "empty Device matches nothing");
    CHECK(!line_in_name_matches("CAB", "CABLE"), "longer than the name");

    CHECK(is_song(L"a.MP3") && is_song(L"b.flac") && !is_song(L"cover.jpg") && !is_song(L"mp3"), "song files");

    /* previous = the song played before, never a fresh shuffle */
    /* Shuffle off: by name, ignoring case, whatever order the folder lists them in */
    wchar_t folder[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    wcscat(folder, L"line_in_sort_test");
    CreateDirectoryW(folder, NULL);
    static const wchar_t *const files[] = {L"b song.mp3", L"A song.mp3", L"c song.flac", L"cover.jpg"};
    for (unsigned i = 0; i < 4; ++i) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", folder, files[i]);
        CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    }
    list_songs(folder);
    line_in_shuffle = 0;
    shuffle_order();
    CHECK(g_count == 3 && wcsstr(g_songs[g_order[0]], L"A song") && wcsstr(g_songs[g_order[1]], L"b song") &&
              wcsstr(g_songs[g_order[2]], L"c song"),
          "Shuffle off plays by name, ignoring case; other files are skipped");
    for (unsigned i = 0; i < 4; ++i) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", folder, files[i]);
        DeleteFileW(path);
    }
    RemoveDirectoryW(folder);

    g_count = 8;
    for (unsigned i = 0; i < 8; ++i) g_alphabetical[i] = i;
    line_in_shuffle = 1;
    shuffle_order();
    unsigned played[20], n = 0, at = 0;
    played[n++] = g_order[at];
    for (int i = 0; i < 10; ++i) played[n++] = g_order[at = advance(at, 0)]; // across the end of the round
    int back_ok = 1;
    for (int i = 0; i < 10; ++i) back_ok &= g_order[at = advance(at, 1)] == played[n - 2 - i];
    CHECK(back_ok, "back retraces the songs played, across a reshuffle");
    CHECK(g_order[at = advance(0, 1)] == g_order[0] && at == 0, "back from the very first song plays it again");
    g_count = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_STARTUP_VERSION, MFSTARTUP_LITE);
    g_playing = 1;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat(path, L"Some Artist - Some Title.wav");
    write_test_wav(path);
    decoded = 0;
    CHECK(line_in_songs_decode(path, count_bytes) == 1, "a WAV decodes");
    CHECK(decoded >= 44100 * 4 - 4 * 441 && decoded <= 44100 * 4 + 4 * 441,
          "1s at 48 kHz mono comes out as 1s at 44.1 kHz stereo");
    char text[256];
    now_playing_text(path, text, sizeof(text));
    CHECK(!strcmp(text, "Some Title\r\nSome Artist\r\n"), "title and artist from the file name");
    DeleteFileW(path);
    CHECK(line_in_songs_decode(L"Z:\\does-not-exist.mp3", count_bytes) == 0, "a missing file is skipped");

    { // an empty Songs folder lets the Device play
        wchar_t folder[MAX_PATH], songs[MAX_PATH], song[MAX_PATH];
        GetTempPathW(MAX_PATH, folder);
        wcscat(folder, L"fh2-line-in-test");
        CreateDirectoryW(folder, NULL);
        CHECK(line_in_songs_count(folder) == 0, "no Songs folder: no songs");
        path_join(songs, folder, L"Songs");
        CreateDirectoryW(songs, NULL);
        path_join(song, songs, L"a.wav");
        write_test_wav(song);
        CHECK(line_in_songs_count(folder) == 1, "a song in the Songs folder is counted");
        DeleteFileW(song);
        RemoveDirectoryW(songs);
        RemoveDirectoryW(folder);
    }

    CHECK(line_in_app_matches(L"foobar2000.exe", L"foobar2000") && line_in_app_matches(L"Spotify.exe", L"spotify") &&
              line_in_app_matches(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"spotify") &&
              !line_in_app_matches(L"Chrome", L"foobar2000") && !line_in_app_matches(L"x", L""),
          "a media session's app id names its program");
    wchar_t program[64];
    CHECK(line_in_session_program(L"{0.0.0.00000000}.{5c6f0f83-1b0e-4c6e-9d2e-1f0c2a7b9e10}|\\Device\\HarddiskVolume3\\Program Files\\foobar2000\\foobar2000.exe%b{00000000-0000-0000-0000-000000000000}", program) &&
              !wcscmp(program, L"foobar2000"),
          "a session's program from its identifier");
    CHECK(line_in_session_program(L"{0.0.0.00000000}.{guid}|\\Device\\HarddiskVolume3\\Users\\a\\AppData\\Roaming\\Spotify\\Spotify.exe%b{guid}", program) &&
              !wcscmp(program, L"spotify"),
          "upper case is lowered");
    CHECK(!line_in_session_program(L"{0.0.0.00000000}.{guid}|#%b{guid}", program), "the system sounds' session has no program");
    CHECK(!line_in_session_program(L"", program), "an empty identifier");

    { // run under Wine too: there the button goes to command.txt for linux_radio.py
        int wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL; // not shipped: the DLL looks for winedevice.exe
        GetTempPathW(MAX_PATH, path);
        path_join(line_in_command_path, path, L"command.txt");
        DeleteFileW(line_in_command_path);
        int sent = line_in_player_command(TRACK_NEXT);
        FILE *command = _wfopen(line_in_command_path, L"rb");
        size_t got = command ? fread(text, 1, sizeof(text) - 1, command) : 0;
        text[got] = 0;
        if (command) fclose(command);
        DeleteFileW(line_in_command_path);
        CHECK(wine ? sent && !strcmp(text, "next\n") : !sent && !got,
              "a track button: command.txt under Wine, the media keys on Windows (no media session)");
    }

    if (argc > 2 && !strcmp(argv[1], "--player")) { // live: which player plays into this device, and its song
        HANDLE read_end, write_end;
        CreatePipe(&read_end, &write_end, NULL, 1 << 20);
        strcpy(line_in_device, argv[2]);
        line_in_use_songs = 0;
        GetTempPathW(MAX_PATH, path);
        line_in_capture_start(write_end, path);
        Sleep(4000);
        FILE *np = _wfopen(line_in_now_playing_path, L"rb");
        size_t got = np ? fread(text, 1, sizeof(text) - 1, np) : 0;
        text[got] = 0;
        if (np) fclose(np);
        printf("render side: %ls\nmedia session found: %d\nnow playing: %s\n", line_in_render_id, g_have_session, text);
        CloseHandle(read_end);
        line_in_capture_stop();
    } else if (argc > 1) { // a real file: how long it decodes to, and its title
        wchar_t file[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, file, MAX_PATH);
        decoded = 0;
        int played = line_in_songs_decode(file, count_bytes);
        now_playing_text(file, text, sizeof(text));
        printf("%s: %d, %.1f s, now playing: %s", argv[1], played, decoded / 4 / 44100.0, text);
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
