# FH2 Mods Collection

Mods for **Forza Horizon 2** on Azimuth 1.0.3 or newer. They need the
[Azimuth Mod Loader](https://github.com/Bullet4fun/Azimuth_ModLoader).

| Mod | What it does |
|---|---|
| [Extra Options](extra-options/README.md) | RokkuDayo's Extra Options: car change anywhere, autodrive, drift camera, FOV, unlocks and more |
| [Spotify Radio](spotify-radio/README.md) | Spotify as an in-game radio station (Spotify Premium only) |
| [Line-in Radio](line-in-radio/README.md) | Your own music files, or any player through a virtual cable, as the in-game radio |
| [Discord RPC](discord-rpc/README.md) | Discord Rich Presence |

## Install

Unzip the mods you want from the release into the Azimuth folder (where `azimuth.exe` is). Each mod is optional; delete its DLL to
turn it off.

## Build

Python 3.10+ and [Zig 0.15.2](https://ziglang.org/download/):

```text
python build.py path\to\zig.exe --mod     --output FH2_ExtraOptions.dll
python build.py path\to\zig.exe --spotify --output FH2_SpotifyRadio.dll
python build.py path\to\zig.exe --line-in --output FH2_LineInRadio.dll
python build.py path\to\zig.exe --discord --output FH2_DiscordRPC.dll
python tests\run.py path\to\zig.exe
```

## AI usage disclosure

Claude was used in this project to create the mods and fork [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) to make it a standalone mod.

## License

GPL-3.0. Extra Options is a fork of
[RokkuDayo/ForzaHorizon2_ExtraOptions](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions).
Spotify Radio ships librespot (MIT) and uses pypinyin (MIT) and Unidecode (GPL-2.0+) data.
