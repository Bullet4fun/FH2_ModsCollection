# Extra Options

[RokkuDayo's Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) for Forza
Horizon 2, as an FH2 Mod Loader mod.

## Features

- Change your car anywhere, "Return to Garage" in the pause menu
- Autodrive, drift camera, FOV for every camera
- Headlights, hazards, turn signals and roof toggles
- All DLC, VIP and Treasure Map unlocked
- 4K liveries, unrestricted photo mode, photo supersampling
- Aero tuning without aero parts, all stock and aftermarket rims in one category
- Custom spawn location, minimap that follows the camera, intro skip
- Custom gamerpic, `mediapc` modding folder, bypass for Azimuth's save checks

Most features are set in the game's own menus. `FH2_ExtraOptions.ini` turns each feature on or off;
a feature turned off there disappears from the menus.

## Install

Unzip `FH2_ExtraOptions.zip` into `data\bin\`.

## Controls

| | |
|---|---|
| Autodrive | D-pad down (F2 on keyboard) |
| Headlights | click the right stick |
| Hazards | tap D-pad up |
| Turn signals | hold D-pad left/right for a second |
| Roof | hold the right stick while standing still |

Some cars have glitched [turn signals](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/wiki/Turn-Signals)
or [roofs](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/wiki/Roofs).

The Treasure Map is off by default: set `UnlockTreasureMap=1` in the INI.

## Custom gamerpic

Put a `Gamerpic.png` (up to 2048x2048 and 16 MB) next to the INI.

## AI usage disclosure (from the original author)

Codex was used in this project to create the DLL. The actual research was done manually, mostly based on things I already toyed with in the other titles from the series, but I don't yet have the C language knowledge to code something like this fully from scratch (I was initially editing the raw executable). Leaving this here as I believe people have the right to know.
