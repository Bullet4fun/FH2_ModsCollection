# Spotify Radio

Plays Spotify in Forza Horizon 2 as the game's **Xbox Music** station. It sounds like the other
stations: in the car, at car meets and from the festival speakers. The radio HUD shows the song.

## Install

Unzip `FH2_SpotifyRadio.zip` into `data\bin\`. librespot needs the
[VC++ 2015-2022 x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Use

Start the game, open Spotify on any device and pick **Forza Horizon 2 Radio** as the speaker, then
tune the car radio to Xbox Music.

Your Spotify login is saved in `data\bin\mods\FH2_SpotifyRadio\credentials.json`. Never share that
file.

## Build librespot

`librespot.exe` is [librespot](https://github.com/librespot-org/librespot) 0.8.0 with the three
patches in `librespot/` applied (`git am`), built with Rust's MSVC toolchain:
`cargo build --release`.

`hud_logo/` makes the station icon, `generate_pinyin.py` and `generate_unidecode.py` make the
tables that romanize song titles for the HUD.
