"""Build and run the unit tests (native Windows executables via zig cc).
Usage: python tests/run.py path/to/zig.exe"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SPOTIFY = ["common/support.c", "spotify-radio/pinyin_table.c", "spotify-radio/unidecode_table.c", "spotify-radio/track_controls.c"]
TESTS = [  # (source, extra sources)
    ("tests/test_translit.c", SPOTIFY),
    ("tests/test_live_buffer.c", SPOTIFY),
    ("tests/test_spotify_bridge.c", SPOTIFY),
    ("tests/test_spotify_mod.c", SPOTIFY + ["common/mod_log.c"]),
    ("tests/test_track_controls.c", ["common/support.c", "common/mod_log.c"]),
    ("tests/test_mod_log.c", ["common/support.c", "common/mod_log.c"]),
    ("tests/test_eo_settings.c", ["common/support.c", "extra-options/features/bindings.c"]),
    ("tests/test_discord_rpc.c", ["common/support.c", "-lws2_32"]),
]


def main():
    zig = Path(sys.argv[1])
    failed = 0
    with tempfile.TemporaryDirectory(prefix="fh2-tests-") as tmp:
        for source, extra in TESTS:
            name = Path(source).stem
            exe = Path(tmp) / (name + ".exe")
            build = [str(zig), "cc", *(a for d in ("common", "sdk", "spotify-radio", "extra-options", "discord-rpc") for a in ("-I", str(ROOT / d))), "-DFH2_AZIMUTH=1", "-Wall", "-Wextra", "-Wno-unused-result", "-fms-extensions",
                     str(ROOT / source), *(s if s.startswith("-") else str(ROOT / s) for s in extra), "-o", str(exe)]
            subprocess.run(build, check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT)
            lines = result.stdout.strip().splitlines()
            print(f"{name}: {lines[-1] if lines else '(no output)'}")
            if result.returncode:
                failed += 1
                print("\n".join(lines[:20]))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
