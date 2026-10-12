# Farming Simulator → MOZA yoke FFB bridge

Drives force feedback on a MOZA FFB flight base from Farming Simulator 25 telemetry, with the
yoke's roll axis used as steering.

| Path | What |
| --- | --- |
| `bridge/` | Windows program that reads the game's telemetry and drives the base through the MOZA SDK (C++20, CMake). The everyday program has a settings window; the console one serves the same settings as a web page. |
| `fs-mod/` | `FS25_MozaYokeLink`, the FS25 Lua mod that writes the telemetry file. See `fs-mod/README.md`. |
| `docs/telemetry.md` | The telemetry file format both sides are built against. |
| `sdk/` | Where the MOZA SDK goes. **Not included in this repository**; see "Getting the MOZA SDK". |

Version 0.1.3. Built for and tested on a MOZA AY210 yoke base with FS25 1.24 on Windows 11.

**Website:** <https://dbryant4.github.io/moza-farmsim-link/> (the page is `docs/index.html`, served by GitHub Pages from `docs/`).

**To just use it**, run the installer from the [latest release](https://github.com/dbryant4/moza-farmsim-link/releases/latest) and follow [docs/INSTALL.md](docs/INSTALL.md). It keeps itself up to date from there: when a newer release is published here, the icon by the clock offers to install it.

## Status

Every part works on the real hardware, and the mod has been driven in the game:

- **Bridge and yoke:** tested on the real AY210 (2026-10-05), including the whole chain driven by
  simulated telemetry: centering that grows with speed, weight, and rumble all reach the motors.
- **Settings:** `moza_fs_link.exe` has a native settings window (`settings_window_win.cpp`), opened from the tray icon, in everyday use by the owner. It can also be drawn to an image and have its controls worked unseen (`--render-settings`, `--selftest-settings`). The web page it replaces is still served by `moza_fs_bridge.exe`, and by the tray program with `--web-ui`; it was tested in a browser on the Mac.
- **FS25 mod:** loads in FS25 1.24, writes telemetry about 60 times a second, and the bridge
  follows it live (first drive 2026-10-05). The individual values (slip, surface roughness,
  implement state) have only been spot-checked. `fs-mod/README.md` lists what to look for.
- **Tuning:** the defaults are a first guess. Rumble in particular is too weak by default; see
  "Still to find out".

## Using it with Farming Simulator 25

1. Close X-Plane and any other flight simulator. The bridge refuses to start while one is running,
   because its start-up reset would cut that simulator's forces.
2. On the PC, run `moza_fs_link.exe`. No window opens: it puts a steering-wheel icon by the clock
   (on Windows 11 new icons start in the `^` overflow; drag it out to keep it visible) and the yoke
   gets a light centering. Click the icon to open the settings window.
3. Start FS25. When loading the savegame, tick **MOZA Yoke Link** in the mod list.
4. Get in a vehicle and drive. The page's status should change to "live, driving".
5. Tune with the sliders; changes apply at once. **Save** keeps them for next time
   (`moza_fs_bridge.ini` next to the program).

To stop: right-click the icon and choose Exit. The settings window's **Release forces** button
drops all forces without stopping the bridge. Starting `moza_fs_link.exe` again while it is running
just opens the settings window; `moza_fs_link.exe --quit` stops it from a script. What it prints goes to
`moza_fs_bridge.log` beside the program (right-click the icon, *Open log file*).

`moza_fs_bridge.exe` is the same bridge as a console program. Use it for the test options below,
or for the link itself if you want to watch its once-a-second status line.

### What the bridge does and does not touch

- It sends the base temporary force effects only. They vanish when it stops, and they lapse by
  themselves within two seconds if it crashes.
- It never writes a setting to the base, and never touches MOZA Cockpit profiles or X-Plane files.
- The mod turns off FS25's own steering force feedback while the bridge is running, so the game
  and the bridge do not fight. That is inside FS25 only, and without the bridge the mod leaves the
  game's force feedback alone. The bridge announces itself through a small heartbeat file
  (`docs/telemetry.md`).
- In FS25, bind the yoke's roll axis to steering (Options > Controls). The game then maps it
  straight to steering angle with no centering of its own; centering comes from the bridge.

### Multiplayer

Untested. FS25 requires every player who joins a game to have the same mods the host has active,
as identical files, and it can only auto-download mods that come from the ModHub. So if you enable
this mod in a multiplayer save, everyone joining needs the same `FS25_MozaYokeLink.zip` in their
mods folder (and a dedicated server needs it too). The bridge and the yoke are only needed by you.

For the others the mod does nothing they would notice: it only turns off the game's steering force
feedback on a machine where the bridge is running, so their wheels keep theirs. On a client, slip
is estimated and "working" may read 0, because those are computed on the server.

### If something is off on the first drive

- **Status stays "waiting for the game":** the mod is not writing. Check FS25's `log.txt` for
  lines starting `[MozaYokeLink]`.
- **The pull pushes away from center:** turn on *Reverse steering pull* on the settings page.
- **The game feels like it has its own spring as well:** look in `log.txt` for
  `game force feedback suppressed` and `game asked about steering force feedback`.
- **Forces vanish mid-drive:** switch *Forces* off and on again on the page; that re-registers
  every effect on the base.

## What the SDK turned out to be

- Native C API (`moza/moza.h`) with a header-only C++20 wrapper and typed per-device accessors
  (`moza/device_as.hpp`). There is no .NET binding, so the bridge is C++.
- `MOZA_SDK.dll` does not talk to USB. It is a client of the background device service installed
  with MOZA Cockpit / Pit House, which must be running.
- Flight bases have a dedicated effect API, the `MainCtrl_Ffb*` commands (ids 603–610), described
  in `sdk/docs/Flight_Base_Force_Feedback_Command_Guide.md`. It mirrors USB HID PID: register an
  effect block (spring, damper, constant, sine, …), update its parameters, start/stop/free it.
  Everything in the force model maps onto it directly.
- `Main_CtrlModeSet` on the base selects `0` telemetry FFB, `1` DirectInput FFB, or `2` both.
  This is the setting behind both risks in the plan; the bridge prints it but does not change it.
- `Main_Steer_TorqueOutX/Y` report the motor's actual output torque, so force can be confirmed
  from the console as well as by feel.

## Getting the MOZA SDK

The bridge is built against MOZA's SDK (1.0.0.4), which is MOZA's to distribute and is not in this
repository. Get it from MOZA and unpack its `x64-release` folder, plus `docs` if you want them, so
that these exist:

```
sdk/x64-release/include/moza/moza.h
sdk/x64-release/lib/MOZA_SDK.lib
sdk/x64-release/bin/MOZA_SDK.dll
```

The tests do not need the DLL or a device (they use a fake of the SDK's C API), but they do need
the headers.

## Build and deploy

The exe can be cross-compiled on macOS or Linux with mingw-w64 (`brew install mingw-w64`), so the
gaming PC needs no compiler. It is statically linked; only it and `MOZA_SDK.dll` go to the PC.

```bash
cmake -S bridge -B bridge/build-win -G Ninja --toolchain cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build bridge/build-win
```

```bash
scp bridge/build-win/moza_fs_link.exe bridge/build-win/moza_fs_bridge.exe bridge/build-win/MOZA_SDK.dll <pc>:moza-farmsim-link/
```

`scripts/package-release.sh` builds what a release carries, in `dist/`: the installer
(`moza-farmsim-link-<version>-setup.exe`, made with NSIS: `brew install makensis`), a zip of the
same files, and the mod on its own (`FS25_MozaYokeLink.zip`) for other players in a multiplayer
game. The installer carries the mod and puts it in the game's mods folder. The program finds new versions through GitHub's "latest release" of this repository
and installs the file there whose name ends in `-setup.exe`, so every release needs one attached.

### Releases and pre-releases

A release is a tag with the three files from `dist/` attached. There are two kinds:

- **A finished version**, `vX.Y.Z`. Offered to every install.
- **A pre-release**, `vX.Y.Z-preN`: the Nth try at what will become `vX.Y.Z`. Offered only to
  installs with *Include pre-releases* switched on. In order, `0.1.3-pre1` comes before
  `0.1.3-pre2`, which comes before `0.1.3`.

The version is set in two lines at the top of `bridge/CMakeLists.txt`: the number in `project(...)`
and `set(BRIDGE_PRERELEASE N)`. For a finished version, leave the number out of the second line.
The mod keeps the plain number (`X.Y.Z.0`), which is all Farming Simulator's version field allows.

So a round of changes goes: set the next number and `BRIDGE_PRERELEASE 1`, publish `vX.Y.Z-pre1`;
for each further try raise it to 2, 3 and so on; when it has proved itself, clear
`BRIDGE_PRERELEASE` and publish `vX.Y.Z`. Mark each pre-release as one on GitHub as well
(`gh release create --prerelease`, or `gh release edit vX.Y.Z-preN --prerelease`), so that GitHub
does not show it as the latest release.

Two things keep a pre-release away from normal installs even if that mark is missing: the program
treats any `-preN` version as a pre-release whatever GitHub says, and it leaves every release alone
for its first 15 minutes (`kReleaseSettleSeconds`), while its files are still being attached.
Version 0.1.2, which knows nothing of pre-releases, does not recognise a `-preN` tag as a version
at all.

`moza_fs_bridge.exe --check-update --prereleases` shows what an install with pre-releases on would
be offered.

The mod is packaged by `fs-mod/package.sh` and installed by copying the zip into FS25's mods folder:

```bash
fs-mod/package.sh && scp fs-mod/dist/FS25_MozaYokeLink.zip "<pc>:Documents/My Games/FarmingSimulator2025/mods/"
```

Building natively with Visual Studio 2022 and CMake ≥ 3.21 (`cmake -S bridge -B bridge/build -A x64`)
is set up in `CMakeLists.txt` but has never been tried.

## Test modes (no game needed)

Run `moza_fs_bridge.exe` from a terminal in the bridge's folder on the PC, with the windowless link
not running. All but `--list` apply force, so keep clear of the yoke.

```bash
.\moza_fs_bridge.exe --list
```

prints the devices and the base's settings and applies no force.

```bash
.\moza_fs_bridge.exe --stroke pitch --damper 100
```

runs pitch through its whole travel twice and back to center, watching its position.

| Option | What it does |
| --- | --- |
| `--hold` | Fixed spring + damper (`--axis`, `--spring`, `--damper`) |
| `--force pitch` | Pushes back and forth with a constant force; spring and damper stay on as limits |
| `--sweep pitch` | Swings a spring's center; gentle, too weak to beat pitch friction |
| `--sine roll` | Shakes an axis with a sine the base generates itself |
| `--stroke roll` | End-to-end travel under position feedback |

`--help` lists everything. In every mode, forces carry a short duration and are refreshed, so the
base lets go on its own if the bridge dies.

## What the hardware runs showed

Measured on the AY210 with X-Plane closed and MOZA Cockpit running (`Main_CtrlModeSet` 2,
spring scale 34%, damper scale 20%, torque limit 100%).

**Effects**
- **Axes:** X is roll, Y is pitch. For force effects, with direction enabled the first angle picks
  the motor: 0 degrees is pitch, 90 is roll. Roll at 0 degrees does nothing.
- **Raw full scale is 32767** for coefficient, center offset and magnitude (now the default `--scale`).
- **Spring:** a 100% spring gives roughly 0.30 N*m per half-travel of offset on pitch.
- **Constant force:** about 0.41 N*m at 100% on pitch (0.02 N*m measured at 5%, 0.17 at 60%).
- **Pitch needs about 0.15 N*m to break free.** Below that it does not move at all, and it tends to
  stick at exact center. At 60% constant force with a 100% spring as the limit it strokes about
  13% of full travel (0.42 to 0.56). Roll is much lighter: 0.05 N*m moves it.
- **Full stroke:** at 80% constant force (`--stroke pitch`), pitch travels 0.000 to about 0.95 and
  back in roughly 3 s each way, stalling just short of the upper target; 60% only covers 0.23 to 0.69
  because the base's own centering grows toward the ends. The return fades out and settles within
  about 0.05 of center.
- **Sine:** period is in milliseconds; 10% gives about +/-0.035 N*m on roll. Started once with a long
  duration it keeps running, and restarting the other effects does not disturb it.
- **Idle torque:** the base holds a small torque of its own (0.02 to 0.10 N*m, growing with pitch
  offset) with no effects registered, and it survives the bridge's reset and stop-all.

**Lifetime of an effect**
- **Durations are enforced.** With Start sent once and a 1 s duration, force stopped at exactly 1 s
  even though parameters kept being re-sent. So Start must be re-sent to keep an effect alive
  (the bridge does this on every update), and forces lapse on their own if the bridge dies.
- **Hard kill:** after `Stop-Process -Force` mid-push, roll was back at center with idle torque by
  the second reading. The first reading showed it about 2% of travel further out with a restoring
  torque, which is unexplained; the readings were too slow to time it.

**SDK defects worked around**
- **Constant force must be sent as a flat argument list.** The typed `MainCtrlFfbConstantRequest`
  (and the guide's own example) nests the three `enableAxis` flags; the SDK then logs a type
  mismatch, sends an empty payload, and still returns success. See `FfbSession::setConstant`.
- **`Moza_Shutdown` sometimes never returns** (about half the force runs). The bridge releases
  forces and closes the device first, then ends its own process after 5 s. Cause unknown.
- **Async sends hide failures.** Neither defect above surfaced through a return code or an error
  event; only `--verbose`, which prints the SDK's debug log, showed them. Use it when adding an effect.
- The SDK prints that debug log to the console unless a log callback is registered, and it writes
  `devsdk.log` next to the exe.
- `Main_Steer_TorqueOutX/Y` come back as N*m decimals, not the raw integer the headers suggest.
- The base lists its HID interface with 0 axes; it still reports X and Y once opened.
- **The SDK can lock itself up for good.** On 2026-10-11, nine minutes into a drive with 0.1.2, the
  link stopped sending forces while its process stayed alive. A memory snapshot showed three
  threads queued on one read/write lock inside `MOZA_SDK.dll` 1.0.0.4: one of the SDK's own
  threads holding it for reading and asking for it again further down its own call chain, a
  second SDK thread waiting to write, and this program's loop waiting to read in
  `RawParameter::getValue()`. A read/write lock that has a writer waiting lets no further reader
  in, so none of the three could ever move. None of this program's code was in the SDK threads'
  call chains. A second snapshot on 2026-10-12 showed the same three places. Both SDK threads were
  each handling a message that had just come in from a device: the SDK takes the lock to store a
  reply, lets it go, then takes it for reading twice over while telling its listeners, and a
  second reply arriving on another thread in between is what jams it. So what it takes is two
  replies at once. The link was producing those itself: it asked for the motor torque of both
  axes four times a second for the status display, and with *Lights follow the engine* on it was
  also writing the handle's brightness and colour as the engine speed moved. Every lock-up seen
  (seven over two days) happened while driving with the lights setting on, at worst every minute;
  with it off there were none, including ten hours idle. 0.1.3-pre3 took the torque
  readout out of the link loop to halve those replies, and with it gone **no force reached the
  yoke at all**: the link ran, sent every command and reported nothing wrong, and the yoke stayed
  limp. Something in the SDK or the base needs those requests to keep coming for effects to play
  (a heartbeat, perhaps); what exactly is not known. 0.1.3-pre4 put the readout back. So the
  lock-up cannot be avoided from this side while the lights setting is on, and that setting says
  so. Since nothing can free a thread stuck like that, the
  link loop is watched (`stall_guard.hpp`): after 10 s without a pass the process ends itself and,
  in the tray program, starts a fresh copy, at most three times in a row.

## Still to find out

0. **What 0.1.3 has and has not been through.** The owner used every pre-release of it on the real
   setup on 2026-10-11 and 12: installed 0.1.2 with the installer's wizard, took pre-releases 2 and
   3 through the tray icon's menu, used the settings window by hand, and drove with pre4, which is
   the same code as 0.1.3. In the game the mod was seen reporting each machine's engine speed
   range and start time, electric vehicles, the kind of surface, wheel bumps and the machine's
   movement. What that leaves open:
   - MOZA's current Flight SDK download is 1.0.0.6; only 1.0.0.4 has been used.
   - With *Lights follow the engine* on, MOZA's SDK locks up (see "What the hardware runs
     showed"). The link recovers by restarting itself, but the setting is best left off.
   - How the newer forces feel has not been judged one by one. They are listed below with what
     each depends on.
0. **The forces, one by one:**
   - *Engine start shake*, and no engine vibration for electric vehicles. The settings page shows
     what the mod reports under *Engine* (off, starting, running, electric).
   - *Obstacle kick*: the wheel jerks toward the side whose front wheel hits a log or kerb, less
     for heavier machines. *Thump* and *wiggle*: a short in-out and left-right shake when both
     front wheels are jolted together, as on rail tracks. These depend on the mod spotting a wheel
     load spike; its thresholds are first guesses, and FS25's `log.txt` gets `first strong bump`
     and `bump check` lines to calibrate them from.
   - *Surface patterns*: the mod reports what kind of ground the wheels are on (`surface`) and the
     bridge gives each its own rumble: where in the frequency band it sits, how lumpy it is, a
     pulse per ridge, and small pulls on the wheel on broken ground. The patterns are in
     `kPatterns` in `bridge/src/force_model.cpp` and are first guesses: none has been felt on the
     yoke, and whether FS25 reports ridges and tilled ground the way the mod expects is unchecked
     (the settings page shows the reported surface under *Game*).
   - *Machine movement* (0.1.3): the mod reports how fast the machine's body is rocking, pitching
     and bouncing (`rollrate`, `pitchrate`, `heave`, from the root node's place and attitude on
     each frame), and the bridge pushes the wheel and the yoke with it. The game does report the
     values; which way round the pushes should go and how strong they should be are first guesses.
   - *Engine vibration* (0.1.3) follows the revs within each machine's own range, the load and
     the road speed, in strength and in frequency. The start shake lasts half of each machine's
     own start, which the mod reads from `spec_motorized.motorStartDuration` (`startms`); FS25
     does fill it in (3.2 s and 3.5 s on the two machines looked at).
   - *Acceleration sway*: pitch leans out when pulling away and in when braking. Pitch has too
     much friction for a plain push, so a small position controller moves it; it is tuned against
     a simulated axis only.
   - *Yoke lights follow the engine* (off by default): dark with the engine off, dim at idle,
     full brightness at the top of each machine's own engine speed range, with the colour moving
     from yours through amber to red over the last part of it. The handle has one brightness and
     one colour for all its lights and no per-hat control. Unlike the forces this writes values the
     handle stores; the bridge restores what it found when it stops, but whether each write wears
     the handle's memory is unknown, so writes are limited to step changes a few times a second.
0. **Nothing here can test whether force reaches the yoke.** The tests run against a stand-in for
   MOZA's SDK that accepts every command, which is how 0.1.3-pre3 passed all of them and did
   nothing on the real yoke. Hands on the yoke are the only check there is.

1. **Tuning from real driving.** Default rumble is too weak to feel: it is capped at 30%, scales
   with speed up to 29 km/h, and goes through pitch, where friction swallows anything under about
   0.15 N*m. Raising *Surface rumble*, *Rumble limit* and *Engine vibration* on the settings page
   makes it felt; better defaults need more seat time. Slip, surface roughness and implement
   state from the mod also still need checking against what the game is doing.
2. **Coexistence with FS25.** FS25 sends its own centering spring to whichever axis steers, if the
   engine thinks that axis can take force feedback. The mod suppresses that from Lua; whether the
   engine does anything below Lua is unknown. (X-Plane was running during the first runs; pitch
   then moved to one side only, so sims do interact.)
3. **Sine restarts.** Whether re-sending Start resets a sine's phase is untested; the bridge avoids
   the question by giving sines a long duration.
4. **Is 0.41 N*m of constant force a setting or a limit?** The base reports up to 1.69 N*m.

## Developing without the hardware

Off Windows the build links `bridge/tests/fake_moza_sdk.cpp`, an in-process implementation of the
SDK's C API that records every command. The tests assert on the exact command stream:

```bash
cmake -S bridge -B bridge/build -G Ninja && cmake --build bridge/build && ctest --test-dir bridge/build --output-on-failure
```

Pass `-DBRIDGE_FAKE_SDK=ON` to do the same on Windows.

## License

MIT; see `LICENSE`. That covers this repository's code. The MOZA SDK it builds against is MOZA's
and is not included.
