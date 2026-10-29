#!/usr/bin/env python3
"""Line-in Radio on Linux (Proton): while the game runs, a sound output named "Forza Horizon 2 Radio"
that any player can play into, the radio's Device; and the song of the player playing there on the
radio HUD, with the track buttons sent to that player. Wine has no Windows media sessions, so this
side does it with MPRIS.

It wraps the game's command; in Steam's launch options:

  python3 /path/to/mods/FH2_LineInRadio/linux_radio.py %command%

Keep this file in mods/FH2_LineInRadio/: it writes nowplaying.txt and reads command.txt there."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from urllib.parse import unquote, urlparse

SINK = "fh2_radio"
DESCRIPTION = "Forza Horizon 2 Radio"
FOLDER = Path(__file__).resolve().parent
RUNNING = FOLDER / "linux_radio.running"  # there while this script runs
# The quoting PipeWire's module-null-sink needs for a description with spaces (PulseAudio takes it too).
PROPERTIES = f'sink_properties="device.description=\\"{DESCRIPTION}\\""'
MPRIS = "org.mpris.MediaPlayer2."
COMMANDS = {"next": "Next", "previous": "Previous", "playpause": "PlayPause"}
# Steam's launch environment preloads its overlay and runtime libraries; the host's tools don't want them.
TOOL_ENV = {key: value for key, value in os.environ.items() if key not in ("LD_PRELOAD", "LD_LIBRARY_PATH")}


def run(*command):
    return subprocess.run(command, capture_output=True, text=True, env=TOOL_ENV).stdout


def json_of(*command):
    try:
        return json.loads(run(*command) or "null")
    except json.JSONDecodeError:
        return None


def sink_module():
    for line in run("pactl", "list", "short", "modules").splitlines():  # the JSON list has no index
        if f"sink_name={SINK}" in line.split():
            return line.split()[0]
    return None


def parent_pid(pid):
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[1])
    except (OSError, IndexError, ValueError):
        return 0


def pick_player(inputs, players, parent_of):
    """inputs: (pid, binary) of the streams playing into the output; players: {MPRIS name: pid}.
    The player whose process is a stream's or its parent's (browsers play from a child process),
    else the one named like a stream's program (a Flatpak's pid is the D-Bus proxy's)."""
    family = set()
    for pid, _ in inputs:
        while pid > 1 and pid not in family:
            family.add(pid)
            pid = parent_of(pid)
    programs = {binary.lower() for _, binary in inputs if binary}
    for name, pid in players.items():
        if pid in family:
            return name
    for name in players:
        app = name[len(MPRIS):].split(".")[0].lower()
        if any(app in program or program in app for program in programs):
            return name
    return None


def current_player():
    sinks = json_of("pactl", "-f", "json", "list", "short", "sinks") or []
    index = next((sink["index"] for sink in sinks if sink["name"] == SINK), None)
    inputs = []
    for stream in json_of("pactl", "-f", "json", "list", "sink-inputs") or []:
        if stream.get("sink") == index:
            properties = stream.get("properties", {})
            inputs.append((int(properties.get("application.process.id", 0)), properties.get("application.process.binary", "")))
    if not inputs:
        return None
    names = (json_of("busctl", "--user", "-j", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                     "org.freedesktop.DBus", "ListNames") or {"data": [[]]})["data"][0]
    players = {}
    for name in names:
        if name.startswith(MPRIS):
            answer = json_of("busctl", "--user", "-j", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                             "org.freedesktop.DBus", "GetConnectionUnixProcessID", "s", name)
            players[name] = answer["data"][0] if answer else 0
    return pick_player(inputs, players, parent_pid)


def song(player):
    metadata = json_of("busctl", "--user", "-j", "get-property", player, "/org/mpris/MediaPlayer2",
                       "org.mpris.MediaPlayer2.Player", "Metadata")
    data = metadata["data"] if metadata else {}
    # A file without tags (mpv, VLC): its name, as Songs shows it.
    url = data.get("xesam:url", {}).get("data", "")
    title = data.get("xesam:title", {}).get("data", "") or Path(unquote(urlparse(url).path)).stem
    artists = data.get("xesam:artist", {}).get("data", [])
    return f"{title}\r\n{', '.join(artists)}\r\n" if title else ""


def game_records():
    """The game plays the output (Source = Device): something records its monitor."""
    sources = json_of("pactl", "-f", "json", "list", "short", "sources") or []
    monitor = next((source["index"] for source in sources if source["name"] == SINK + ".monitor"), None)
    return monitor is not None and any(output.get("source") == monitor
                                       for output in json_of("pactl", "-f", "json", "list", "source-outputs") or [])


def helper():
    now_playing, command_file = FOLDER / "nowplaying.txt", FOLDER / "command.txt"
    player = None
    for tick in range(sys.maxsize):
        if command_file.exists():
            command = command_file.read_text().strip()
            command_file.unlink(missing_ok=True)
            if player and command in COMMANDS:
                run("busctl", "--user", "call", player, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
                    COMMANDS[command])
        if tick % 5 == 0:  # once a second
            player = current_player() if game_records() else None  # Source = Songs: the mod shows its own
            text = song(player) if player else ""
            # The mod deletes nowplaying.txt when it starts: write it again then too.
            # the mod puts the station's name there whenever the source changes: compare with the file
            if text and (not now_playing.exists() or now_playing.read_bytes() != text.encode()):
                now_playing.write_bytes(text.encode())
        time.sleep(0.2)


def remove_output():
    module = sink_module()
    if module:
        run("pactl", "unload-module", module)


def pid_alive(pid):
    return pid > 0 and Path(f"/proc/{pid}").exists()


def watch(pid):
    """Started outside Steam's process tree: when Steam closes the game it kills the whole tree, often
    before play()'s own cleanup gets to pactl, and the output stayed. This waits for that run to end."""
    while pid_alive(pid):
        time.sleep(1)
    try:
        owner = int(RUNNING.read_text().strip() or 0)
    except (OSError, ValueError):
        owner = 0
    if pid_alive(owner) and owner != pid:
        return  # the game was started again meanwhile: that run owns the output now
    RUNNING.unlink(missing_ok=True)
    remove_output()


def start_watcher():
    command = [sys.executable, str(Path(__file__).resolve()), "watch", str(os.getpid())]
    try:  # a user unit of its own: nothing Steam kills with the game
        if subprocess.run(["systemd-run", "--user", "--quiet", "--collect", *command], env=TOOL_ENV,
                          capture_output=True).returncode == 0:
            return
    except OSError:
        pass
    subprocess.Popen(command, env=TOOL_ENV, start_new_session=True, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def play(command):
    """The output exists while the game runs; one left by a killed run is taken over and removed too."""
    if not sink_module():
        run("pactl", "load-module", "module-null-sink", f"sink_name={SINK}", PROPERTIES)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))  # Steam's Stop: still remove the output
    # The mod sees this and uses the output and this script (no media sessions on Linux) for the HUD and buttons.
    RUNNING.write_text(f"{os.getpid()}\n")
    start_watcher()
    threading.Thread(target=helper, daemon=True).start()
    try:
        return subprocess.run(command).returncode
    finally:
        RUNNING.unlink(missing_ok=True)
        remove_output()


def self_test():
    parents = {300: 200, 200: 1, 400: 1}
    players = {MPRIS + "chromium.instance200": 200, MPRIS + "spotify": 999}
    assert pick_player([(300, "chrome")], players, parents.get) == MPRIS + "chromium.instance200"
    assert pick_player([(400, "spotify")], players, parents.get) == MPRIS + "spotify"
    assert pick_player([(400, "mpv")], players, parents.get) is None
    print("ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["test"]:
        self_test()
    elif len(sys.argv) == 3 and sys.argv[1] == "watch":
        watch(int(sys.argv[2]))
    elif len(sys.argv) > 1:
        sys.exit(play(sys.argv[1:]))
    else:
        sys.exit(__doc__)
