# Extra Options

[RokkuDayo's Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) 0.4.4 for Forza
Horizon 2, as a mod for the Azimuth Mod Loader.

## Features

- Change your car anywhere, "Return to Garage" in the pause menu
- "Jump to Manufacturer" in the car and rim menus
- All one-off races from online playable offline, your own car in showcase events
- Autodrive, drift camera, FOV for every camera
- Headlights (they light the road in daytime too), hazards, turn signals and roof toggles
- Rev your engine in the garage and the engine swap menu
- Photo mode in the "View Car" menus, time (with an hour slider), weather, driver and car lights in photo mode
- Master volume and dialogue toggles, spring and power units, a hide-the-UI key, vignette toggles
- Option to turn off Drivatars in free roam; Drivatars can use your local liveries
- All DLC, VIP and Treasure Map unlocked
- 4K liveries, unrestricted photo mode, photo supersampling
- All stock and aftermarket rims in one category, resizable stock rims
- Custom spawn location, minimap that follows the camera, intro skip
- `mediapc` modding folder, add-on mods, bypass for Azimuth's save checks
- Experimental: aero tuning without aero parts, every engine and gearbox as a swap

The custom gamerpic of the original is left out: Azimuth sets your profile picture itself.

Most features are set in the game's own menus. The config file `FH2_ExtraOptions.ini` turns each feature on or off;
a feature turned off there disappears from the menus. Its `[Bindings]` section sets every
controller and keyboard shortcut.

## Install

Unzip `FH2_ExtraOptions.zip` into the Azimuth folder (where `azimuth.exe` is).

Add-on mods made for Extra Options (a folder with a `mod.json`, for example RokkuDayo's Weather
Control and Garage Revving) go in the same `mods` folder, as in the original release. Don't install
the original release's `winmm.dll` as well: Extra Options would run twice.

## Controls

| Action | Button |
|---|---|
| Autodrive | D-pad down (F2 on keyboard) |
| Headlights | click the right stick (F3) |
| Hazards | tap D-pad up |
| Turn signals | hold D-pad left/right for half a second |
| Roof | hold the right stick for 1.5 seconds while standing still (F4) |
| Hide the UI | F1 |

Headlights, hazards, turn signals and roof are off by default: set them to 1 in the `[Vehicle]`
section of `mods\FH2_ExtraOptions\FH2_ExtraOptions.ini`.

Some cars have glitched turn signals or roofs: see the original mod's
[wiki](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/wiki/4.-Oddities).

The Treasure Map is off by default: set `UnlockTreasureMap=1` in the config file.

## AI usage disclosure (from the original author)

Codex was used in this project to create the DLL. The actual research was done manually, mostly based on things I already toyed with in the other titles from the series, but I don't yet have the C language knowledge to code something like this fully from scratch (I was initially editing the raw executable). Leaving this here as I believe people have the right to know.
