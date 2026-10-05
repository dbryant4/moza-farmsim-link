# Installing

Force feedback for Farming Simulator 25 on a MOZA flight yoke, with the yoke's roll axis as the
steering wheel.

## What you need

- A MOZA force feedback flight base. Built for and tested on the **AY210**; other MOZA flight bases
  use the same commands but are untested.
- **MOZA Cockpit** installed and running (the bridge talks to its background device service).
- **Farming Simulator 25** on Windows.
- `MOZA_SDK.dll` from MOZA's SDK, version 1.0.0.4, 64-bit release build (`x64-release\bin`). It is
  MOZA's file and is not part of this download.

## Install

1. **Unzip** the download into a folder of its own, for example `C:\moza-farmsim-link`.
2. **Add `MOZA_SDK.dll`** to that folder, next to `moza_fs_bridge.exe`.
3. **Install the mod:** copy `FS25_MozaYokeLink.zip` (leave it zipped) into
   `Documents\My Games\FarmingSimulator2025\mods`.
4. **Bind steering in FS25:** in Options > Controls, bind steering left/right to the yoke's roll
   axis. Set that axis's deadzone low (0 to 5%).

## Use

1. Close X-Plane, Microsoft Flight Simulator, DCS and IL-2. The bridge will not start while one of
   them is running, because it resets the yoke's force effects when it starts.
2. Run **`Start FS25 yoke link.bat`**. A console window opens, the yoke gets a light centering
   force, and the settings page opens at http://127.0.0.1:8777/.
3. Start FS25 and tick **MOZA Yoke Link** in the mod list when you load your savegame.
4. Get in a vehicle. The settings page should say **live, driving**.
5. Tune with the sliders; changes apply immediately. **Save** keeps them for next time.

To stop, close the console window or press Ctrl+C in it. **Release forces** on the settings page
drops every force without stopping the bridge.

Keep hands and objects clear of the yoke when starting the bridge and when using its test options:
it is a motor, and the test options move it on their own.

## First things to adjust

- **No rumble:** the default is cautious. Raise *Surface rumble*, *Rumble limit* and
  *Engine vibration*. Surface rumble only appears while moving, and not on asphalt.
- **The pull pushes away from center:** turn on *Reverse steering pull*.
- **Status stays "waiting for the game":** the mod is not running. Check that it is ticked for the
  savegame, and look for lines starting `[MozaYokeLink]` in
  `Documents\My Games\FarmingSimulator2025\log.txt`.
- **Forces disappear mid-drive:** switch *Forces* off and on again on the settings page.

## What it changes on your PC

- The bridge sends the yoke temporary force effects only. They vanish when it stops. It never
  writes a setting to the base and does not touch MOZA Cockpit's profiles or any other simulator.
- The mod writes `telemetry.txt` in `modSettings\FS25_MozaYokeLink` in your FS25 profile. While the
  bridge is running it also turns off FS25's own steering force feedback, so the two do not fight.
  Without the bridge running, the mod leaves the game's force feedback alone.
- The bridge keeps its settings in `moza_fs_bridge.ini` next to the program, and MOZA's SDK writes
  `devsdk.log` there.

## Multiplayer

Everyone who joins a game needs the same mods the host has active, as identical files, so the
other players need this `FS25_MozaYokeLink.zip` too. They do not need the bridge or a yoke, and
without the bridge the mod does not change how their game behaves. Multiplayer is untested.

## Uninstall

Delete the folder, and delete `FS25_MozaYokeLink.zip` from the FS25 mods folder. Nothing else is
installed.
