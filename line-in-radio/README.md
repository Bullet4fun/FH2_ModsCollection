# Line-in Radio

Plays your own music in Forza Horizon 2 as an in-game radio station: in the car, at car
meets and from the festival speakers, with the song's title and artist on the radio HUD. The music
comes from either:

- **the Songs folder**: music files you put into the mod's `Songs` folder; or
- **any music player** (the Spotify app, YouTube Music in a browser, foobar2000...), playing into a
  sound device you don't listen to, such as a virtual cable.

Line-in Radio and Spotify Radio use the same station, so only one of them can be installed: with
`FH2_SpotifyRadio.dll` in the `mods` folder, Line-in Radio stays off.

## Install

Unzip `FH2_LineInRadio.zip` into the Azimuth folder (where `azimuth.exe` is). It needs the
[Azimuth Mod Loader](https://github.com/Bullet4fun/Azimuth_ModLoader).

## Pick what the radio plays

In the game: *Settings → Audio*, under Radio Volume.

- **Line-in Radio**: `Songs folder`, or one of the playback devices connected now (the one the game
  itself plays to is left out). It switches at once and is remembered.
- **Shuffle** (the Songs folder only): random order, or alphabetical when off.

Then tune the car radio to Line-in Radio.

## The Songs folder

1. Start the game once: the mod makes `mods\FH2_LineInRadio\Songs\`.
2. Put music files there: MP3, FLAC, WAV, M4A, AAC or WMA.
3. Restart the game and pick `Songs folder` in the Audio settings.

The HUD shows each file's title and artist tags, or its name: `Artist - Title.mp3` shows both.

## Any music player (Windows)

The player plays into a virtual cable instead of your speakers, and the game plays the cable.

1. Download **VB-Audio Virtual Cable** from https://vb-audio.com/Cable/, unzip it, run
   `VBCABLE_Setup_x64.exe` as administrator and restart Windows. Check that `CABLE Input` is now
   listed in Windows' *Settings → System → Sound* under Output; if it isn't, run the setup as
   administrator again and restart.
2. Send your player to the cable: in its own audio settings pick `CABLE Input`, or in Windows'
   *Settings → System → Sound → Volume mixer* (*App volume and device preferences* on Windows 10),
   set the player's output to `CABLE Input`. Leave the game's own output on your speakers or
   headphones.
3. Pick the cable in the game as well: start it, open *Settings → Audio* and, under Radio Volume,
   set **Line-in Radio** to `CABLE Input (VB-Audio Virtual Cable)`. It starts on `Songs folder`, so
   without this step the radio doesn't play your player.
4. Tune the car radio to Line-in Radio and start playing music in the player.

Outside the game you won't hear that player any more; set its output back to your speakers when you
are done. A spare output you don't listen to (a monitor's HDMI audio with no speakers, a second sound
card) works the same way as the cable.

When the player shows up in Windows' media overlay (Spotify, browsers, foobar2000 2.x and most other
players do), its song goes on the radio HUD and the track buttons control that player only. A player
that doesn't show up there still plays, without its song on the HUD and without the track buttons.

## Linux (Proton)

No cable is needed: `linux_radio.py` (in `mods/FH2_LineInRadio/`) makes a sound output named
**Forza Horizon 2 Radio** while the game runs, and removes it when the game closes.

1. In Steam: the game's *Properties → Launch Options*, put the script in front of `%command%`
   (keep what is already there, with your own Azimuth folder):

       WINEDLLOVERRIDES="dinput8=n,b" python3 ~/Games/Azimuth/mods/FH2_LineInRadio/linux_radio.py %command%

2. Start the game, send your player to `Forza Horizon 2 Radio` (its own audio settings, pavucontrol
   or the desktop's sound settings) and pick that output in the game's Audio settings. With an empty
   Songs folder, the radio plays it from the start.

The script puts the song of the player playing there on the radio HUD and passes the track buttons to
it (through MPRIS). Under Proton 11, the Songs folder plays MP3, M4A and WAV, not FLAC, and the HUD
shows the file name (`Artist - Title.mp3`), not the tags.

## Track buttons

While Line-in Radio plays in the car: tap the left stick for the next track, twice for the previous
one; F6, F5 and F7 (play/pause) on the keyboard. Change them in the config file,
`mods\FH2_LineInRadio\FH2_LineInRadio.ini`, made at the first start.

## If something doesn't work

The mod's log, `mods\FH2_LineInRadio\FH2_LineInRadio.log`, lists every sound device it can see and
says where each track button went. Please attach it to an issue.
