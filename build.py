"""Build the mods with Zig 0.15.2, including cross-compilation on Linux.
Usage: python build.py /path/to/zig (--mod | --spotify | --discord) [--output PATH]
The mod loader (azimuth-tls.dll) lives in its own repo, FH2_ModLoader.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent
COMMON_SOURCES = [
    "extra-options/dllmain.c",
    "extra-options/runtime.c",
    "extra-options/settings.c",
    "common/support.c",
    "common/logging.c",
    "extra-options/patch_engine.c",
    "extra-options/verification.c",
    "extra-options/sha256.c",
    "extra-options/patches_generated.c",
    "extra-options/features/garage.c",
    "extra-options/features/road_placement.c",
    "extra-options/features/gamerpic.c",
    "extra-options/features/treasure_map.c",
    "extra-options/features/minimap.c",
    "extra-options/features/rims.c",
    "extra-options/features/return_garage.c",
    "extra-options/features/stock_wheels.c",
    "extra-options/features/wheel_variants.c",
    "extra-options/features/drift_camera.c",
    "extra-options/features/video_settings.c",
    "extra-options/features/camera_fov.c",
    "extra-options/features/video_options.c",
    "extra-options/features/video_options_text.c",
    "extra-options/features/video_options_xaml.c",
    "extra-options/features/photo_sampling.c",
    "extra-options/features/sun_shadows.c",
    "extra-options/features/livery.c",
    "extra-options/features/media_overrides.c",
    "extra-options/features/vehicle_controls.c",
    "extra-options/features/blinkers.c",
    "extra-options/features/aero_tuning.c",
    "extra-options/features/roof_fade.c",
    "extra-options/features/autodrive.c",
    "extra-options/features/game_input.c",
    "extra-options/features/autodrive_game.c",
    "extra-options/features/autodrive_notice.c",
    "extra-options/features/autodrive_ui.c",
    "extra-options/features/png_validation.c",
    "extra-options/features/png_decode.c",
]
# Separate mods for the loader; each is its own DLL in data\bin\mods\.
DISCORD_SOURCES = ["discord-rpc/discord_mod.c", "discord-rpc/discord_rpc.c", "common/support.c"]
SPOTIFY_SOURCES = ["spotify-radio/spotify_mod.c", "common/mod_log.c", "spotify-radio/spotify_bridge.c", "spotify-radio/track_controls.c",
                   "spotify-radio/pinyin_table.c", "spotify-radio/unidecode_table.c", "common/support.c"]
INCLUDE_DIRS = ["common", "sdk", "extra-options", "spotify-radio", "discord-rpc"]
SPOTIFY_VERSION = "1.1.0"
DISCORD_VERSION = "1.0.0"


def version_resource(compiler, directory, filename, description, product, version):
    """A VERSIONINFO resource for the DLL (file description, version, license), compiled by zig rc:
    an unsigned DLL without one looks more suspicious to antivirus heuristics."""
    numbers = ",".join(version.split(".") + ["0"])
    strings = {"CompanyName": "Bullet (FH2 mods)", "FileDescription": description, "FileVersion": version,
               "InternalName": filename.rsplit(".", 1)[0], "LegalCopyright": "GPL-3.0-or-later",
               "OriginalFilename": filename, "ProductName": product, "ProductVersion": version}
    values = "\n".join(f'            VALUE "{key}", "{value}"' for key, value in strings.items())
    script = directory / "version.rc"
    script.write_text(f"""1 VERSIONINFO
FILEVERSION {numbers}
PRODUCTVERSION {numbers}
FILEOS 0x40004
FILETYPE 0x2
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
{values}
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
""", encoding="utf-8")
    resource = directory / "version.res"
    subprocess.run([str(compiler), "rc", "/fo", str(resource), str(script)], check=True)
    return str(resource)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zig", type=Path)
    parser.add_argument("--mod", action="store_true",
                        help="Build ExtraOptions as data/bin/mods/FH2_ExtraOptions.dll for the loader")
    parser.add_argument("--discord", action="store_true", help="Build the Discord Rich Presence mod")
    parser.add_argument("--spotify", action="store_true", help="Build the Spotify radio mod")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-dir", type=Path)
    arguments = parser.parse_args()
    if sum((arguments.mod, arguments.discord, arguments.spotify)) != 1:
        parser.error("choose one of --mod, --discord, --spotify")
    return arguments


def compiler_arguments(compiler, options):
    arguments = [
        str(compiler),
        "clang",
        "-target",
        "x86_64-w64-windows-gnu",
        "-c",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fms-extensions",
        "-funwind-tables",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *(argument for folder in INCLUDE_DIRS for argument in ("-I", str(SOURCE / folder))),
        "-isystem",
        str(compiler.parent / "lib/include"),
        "-isystem",
        str(compiler.parent / "lib/libc/include/any-windows-any"),
    ]
    if options.mod:
        arguments.append("-DFH2_AZIMUTH=1")
    return arguments


def describe(options):
    """(file description, product, version) for the VERSIONINFO resource."""
    if options.spotify:
        return "Spotify radio for Forza Horizon 2 (FH2_ModLoader mod)", "FH2 Mods Collection", SPOTIFY_VERSION
    if options.discord:
        return "Discord Rich Presence for Forza Horizon 2 (FH2_ModLoader mod)", "FH2 Mods Collection", DISCORD_VERSION
    header = (SOURCE / "common/extra_options.h").read_text(encoding="utf-8")
    version = header.split('#define MOD_VERSION "', 1)[1].split('"', 1)[0]  # ExtraOptions' own version
    return "Forza Horizon 2 Extra Options by RokkuDayo (FH2_ModLoader mod)", "Forza Horizon 2 Extra Options", version

def build(compiler, output, directory, options):
    directory.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    objects = []
    azimuth_host = [
        "extra-options/adapters/azimuth_host.c",
        "extra-options/adapters/azimuth_save.c",
        "extra-options/adapters/azimuth_markers.c",
    ]
    if options.discord:
        sources = DISCORD_SOURCES
    elif options.spotify:
        sources = SPOTIFY_SOURCES
    else:  # --mod: the loader owns Azimuth's helper ABI and finds the game
        sources = [*COMMON_SOURCES, "extra-options/adapters/mod.c", *azimuth_host]
    arguments = compiler_arguments(compiler, options)
    for filename in sources:
        object_file = directory / (Path(filename).stem + ".obj")
        command = ([str(compiler), "clang", "-target", "x86_64-w64-windows-gnu", "-c"]
                   if filename.endswith(".s") else arguments)
        subprocess.run([*command, str(SOURCE / filename), "-o", str(object_file)], check=True)
        objects.append(str(object_file))
    import_library = directory / "kernel32.lib"
    subprocess.run(
        [
            str(compiler),
            "dlltool",
            "-m",
            "i386:x86-64",
            "-d",
            str(SOURCE / "common/kernel32.def"),
            "-l",
            str(import_library),
        ],
        check=True,
    )
    exception_library = directory / "ntdll.lib"
    subprocess.run([str(compiler), "dlltool", "-m", "i386:x86-64", "-d", str(SOURCE / "common/ntdll.def"),
                    "-l", str(exception_library)], check=True)
    # /release (a PE checksum) and a debug directory keep antivirus ML engines quiet; the PDB
    # stays in the build folder.
    link_arguments = [
        str(compiler),
        "lld-link",
        "/dll",
        "/entry:DllMain",
        "/machine:x64",
        "/nodefaultlib",
        "/dynamicbase",
        "/nxcompat",
        "/out:" + str(output),
        "/implib:" + str(directory / "extra_options.lib"),
        *objects,
        str(import_library),
        str(exception_library),
        version_resource(compiler, directory, output.name, *describe(options)),
        "/release",
        "/debug",
        "/pdb:" + str(directory / output.with_suffix(".pdb").name),
        "/pdbaltpath:%_PDB%",
    ]
    subprocess.run(link_arguments, check=True)


def main():
    options = parse_arguments()
    compiler = options.zig.resolve()
    filename = ("FH2_ExtraOptions.dll" if options.mod
                else "FH2_DiscordRPC.dll" if options.discord
                else "FH2_SpotifyRadio.dll")
    output = (options.output or SOURCE.parent / filename).resolve()
    if options.build_dir:
        build(compiler, output, options.build_dir.resolve(), options)
    else:
        with tempfile.TemporaryDirectory(prefix="fh2-extra-options-build-") as temporary:
            build(compiler, output, Path(temporary), options)
    print(output)


if __name__ == "__main__":
    main()
