# Spotify Radio

Plays Spotify in Forza Horizon 2 as the game's **Spotify Radio** station that replaces Xbox Music (yes, that was a thing, but it was cut out from the game). It sounds like the other
stations: in the car, at car meets and from the festival speakers. The radio HUD shows the song.

**It needs Spotify Premium.** Spotify plays to other devices (Spotify Connect, which this mod is)
only for Premium accounts.

## Known problem: some Premium accounts can't play

Spotify refuses the songs' decryption keys to some Premium accounts when they play through
[librespot](https://github.com/librespot-org/librespot), the Spotify Connect player this mod uses.
Sign-in works and the songs are listed, but every track is skipped, and
`mods\FH2_SpotifyRadio\librespot.log` says `error audio key 0 1` for each one. Other accounts
play fine on the same PC, so it depends on the account, not on your setup; signing in again doesn't
help. It is an open librespot issue with no fix yet:
[librespot#1649](https://github.com/librespot-org/librespot/issues/1649).

If your account is one of them, use [Line-in Radio](../line-in-radio/README.md) instead: it plays the
Spotify app (or any other player) through a virtual cable, on the same in-game station.

## Install

Unzip `FH2_SpotifyRadio.zip` into the Azimuth folder (where `azimuth.exe` is). librespot needs the
[VC++ 2015-2022 x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Use

The first time Spotify Radio starts, your browser opens the Spotify sign-in page: sign in once and
the login is saved.

Start the game, open Spotify on any device and pick **Forza Horizon 2 Radio** as the speaker, then
tune the car radio to Spotify Radio.

While Spotify Radio plays: tap the left stick for the next track, tap it twice for the previous one
(F6 and F5 on the keyboard, F7 play/pause). Change the buttons in the config file,
`mods\FH2_SpotifyRadio\FH2_SpotifyRadio.ini`; it explains itself.

Your Spotify login is saved in `mods\FH2_SpotifyRadio\credentials.json`. Never share that
file.

## Linux (Proton)

The same files work under Proton. Tested with DW-Proton, Proton-GE, Proton (stable) and
Proton-CachyOS, Azimuth started from Steam with no extra launch options; the sign-in page opens in
the system browser.
Other Proton versions are untested: feedback on how it runs for you is very welcome.

## Build librespot

`librespot.exe` is [librespot](https://github.com/librespot-org/librespot) 0.8.0 with the
patches in `librespot/` applied (`git am`), built with Rust's MSVC toolchain:

```text
cargo build --release --no-default-features --features rustls-tls-webpki-roots,with-libmdns
```

The release's `librespot.exe` is built so that the same source gives the same bytes, in PowerShell
from the patched librespot folder, with a build folder outside it:

```text
$env:SOURCE_DATE_EPOCH = git log -1 --format=%ct      # the build id: the last patch's time, not a random one
$t = "C:\librespot-target"
$env:RUSTFLAGS = "-C link-arg=/Brepro -C link-arg=/PDBALTPATH:%_PDB% " +
    "--remap-path-prefix=$env:USERPROFILE\.cargo=cargo --remap-path-prefix=$env:USERPROFILE\.rustup=rustup " +
    "--remap-path-prefix=$PWD=librespot --remap-path-prefix=$t=target"
cargo build --release --target-dir $t --no-default-features --features rustls-tls-webpki-roots,with-libmdns
```

rustls keeps TLS inside librespot: Wine's own TLS breaks its connections under Proton.

`hud_logo/` makes the station icon, `generate_pinyin.py` and `generate_unidecode.py` make the
tables that romanize song titles for the HUD.
