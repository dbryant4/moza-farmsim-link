# Installing

Force feedback for Farming Simulator 25 on a MOZA flight yoke, with the yoke's roll axis as the
steering wheel.

## What you need

- A MOZA force feedback flight base. Built for and tested on the **AY210**; other MOZA flight bases
  use the same commands but are untested.
- **MOZA Cockpit** installed and running (the bridge talks to its background device service).
- **Farming Simulator 25** on Windows.
- **MOZA's Flight SDK**, a free download from <https://mozaracing.com/pages/sdk>. One file from it,
  `MOZA_SDK.dll`, is needed. It is MOZA's to hand out, so it is not part of this download; the
  installer takes it from the zip you download. Tested with version 1.0.0.4.

## Install

1. Download the **Flight SDK** zip from <https://mozaracing.com/pages/sdk>. The page also offers a
   Racing SDK; that is not the one. Leave the zip as it is.
2. Run **`moza-farmsim-link-<version>-setup.exe`** from the
   [latest release](https://github.com/dbryant4/moza-farmsim-link/releases/latest). Windows may
   warn that it does not know the publisher, because the installer is not code-signed: choose
   *More info*, then *Run anyway*. It installs for your Windows account only and asks for no
   administrator rights.
3. When the installer asks for the file from MOZA, **point it to the Flight SDK zip** you
   downloaded. It copies `MOZA_SDK.dll` out of it. This is asked once; updates keep the file.
4. Leave **Farming Simulator 25 mod** ticked so the mod is put in the game's mods folder. Tick
   **Start with Windows** if you want the link running whenever you sign in.
5. **Bind steering in FS25:** in Options > Controls, bind steering left/right to the yoke's roll
   axis. Set that axis's deadzone low (0 to 5%).

### Without the installer

The release also has a plain zip. Unzip it into a folder of its own, put `MOZA_SDK.dll` (from
`x64-release\bin` in MOZA's Flight SDK zip) next to `moza_fs_link.exe`, and run that. The mod is
copied into the game's mods folder for you when the program starts. A copy set up this way tells
you about new versions but does not install them itself.

## Updates

Once a day the program asks GitHub whether a newer release exists. When one does, a notification
appears by the clock and the icon's right-click menu gains **Install version x.y.z**. Picking it
downloads that release's installer, checks it against the checksum GitHub lists, and runs it: the
link lets go of the yoke, is replaced, and starts again within a few seconds. Nothing is
downloaded or installed until you pick it, so it never happens under you while driving.

- **Check for updates** in the same menu asks straight away.
- *Look for new versions* on the settings page turns the daily check off.
- The mod is updated along with the program. If Farming Simulator is running at the time, the
  game has the mod's file open; the program replaces it once the game has closed.
- In multiplayer everyone needs the same mod file, so update together.

## Use

1. Close X-Plane, Microsoft Flight Simulator, DCS and IL-2. The bridge will not start while one of
   them is running, because it resets the yoke's force effects when it starts.
2. Start **MOZA FS25 Link** from the Start menu (`moza_fs_link.exe`). No window opens: a steering-wheel icon appears by the clock and
   the yoke gets a light centering force. On Windows 11 new icons start in the `^` overflow next
   to the clock; drag it out to keep it in view. **Click the icon** to open the settings page,
   http://127.0.0.1:8777/.
3. Start FS25 and tick **MOZA Yoke Link** in the mod list when you load your savegame.
4. Get in a vehicle. The settings page should say **live, driving**.
5. Tune with the sliders; changes apply immediately. **Save** keeps them for next time.
6. **Export** downloads the settings as they are now as a file, `moza-fs-link-settings.ini`, to
   keep as a backup or pass to someone else. **Import** loads such a file: the values it holds take
   effect at once and count as unsaved changes, so **Revert** undoes an import and **Save** keeps
   it. Settings the file does not mention are left as they are.

To stop, right-click the icon and choose **Exit**. **Release forces** on the settings page drops
every force without stopping the bridge. To have it start with Windows, run the installer again
and tick **Start with Windows** (or put a shortcut to `moza_fs_link.exe` in the Startup folder:
press Win+R and enter `shell:startup`).

If it cannot start (no yoke found, a flight simulator running, MOZA Cockpit not running) it says
so in a message box. What it prints goes to `moza_fs_bridge.log` beside the program.

Keep hands and objects clear of the yoke when starting the bridge and when using its test options:
it is a motor, and the test options move it on their own.

## First things to adjust

- **No rumble:** the default is cautious. Raise *Surface rumble*, *Rumble limit* and
  *Engine vibration*. Surface rumble only appears while moving, and not on asphalt.
- **Surfaces:** each kind of ground has its own feel: gravel a fast gritty buzz, grass and snow low
  and soft, a sown field medium and steady, a ploughed or cultivated one slow and lumpy, ridges a
  pulse per ridge. The *Surfaces* group has a strength for each, and *Tug on broken ground* for
  the small pulls on the wheel on rough fields. Asphalt starts at nothing. The page shows which
  surface the game reports under *Game > Surface*. *A different feel for each surface* switches
  it all back to one plain rumble.
- **Engine start:** *Engine start shake* sets how hard the wheel shakes while a combustion engine
  is being started. Electric vehicles have no engine vibration at all, only the ground's.
- **Bumps:** *Obstacle kick* jerks the wheel toward the side whose front wheel hits something;
  *Thump* and *Wiggle* shake it when both front wheels hit together, as on rail tracks. Heavier
  machines get less kick and wiggle.
- **Acceleration:** *Acceleration sway* leans the yoke out when you pull away and in when you
  brake. If it leans the wrong way, turn on *Reverse acceleration sway*; set it to 0 to switch it off.
- **Lights:** *Lights follow the engine* (off by default) dims the yoke handle's lights with the
  engine off or idling and brightens them up to each machine's own top engine speed, shifting the
  colour toward red near the top. It changes the handle's stored brightness and colour while the
  bridge runs and restores them on exit.
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
- The installer puts the program in `%LOCALAPPDATA%\Programs\MOZA FS25 Link`, adds a Start menu
  shortcut and an entry under *Installed apps*, and copies the mod into the game's mods folder.
  It changes nothing outside your own Windows account.
- The bridge keeps its settings in `moza_fs_bridge.ini` next to the program and its log in
  `moza_fs_bridge.log`, and MOZA's SDK writes `devsdk.log` there.
- The only thing it sends over the internet is the daily question to GitHub about the latest
  release (and the download, when you pick an update). Turn it off with *Look for new versions*.
- `moza_fs_bridge.exe` is the same bridge as a console program, for the test options in the README.

## Multiplayer

Everyone who joins a game needs the same mods the host has active, as identical files, so the
other players need this `FS25_MozaYokeLink.zip` too; each release has it as a download of its own. They do not need the bridge or a yoke, and
without the bridge the mod does not change how their game behaves. Multiplayer is untested.

## Uninstall

Remove **MOZA FS25 Link** under Windows Settings > Apps > Installed apps. That removes the program,
its shortcuts and the mod from the game's mods folder, and asks whether to remove your saved
settings as well. A copy unzipped by hand is removed by deleting its folder and deleting
`FS25_MozaYokeLink.zip` from the FS25 mods folder.
