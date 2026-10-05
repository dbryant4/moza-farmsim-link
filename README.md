# Farming Simulator → MOZA yoke FFB bridge

Drives force feedback on a MOZA FFB flight base from Farming Simulator 25 telemetry, with the
yoke's roll axis used as steering.

| Path | What |
| --- | --- |
| `bridge/` | Windows program that reads the game's telemetry and drives the base through the MOZA SDK (C++20, CMake). Serves its own settings page. |
| `fs-mod/` | `FS25_MozaYokeLink`, the FS25 Lua mod that writes the telemetry file. See `fs-mod/README.md`. |
| `docs/telemetry.md` | The telemetry file format both sides are built against. |
| `sdk/` | Where the MOZA SDK goes. **Not included in this repository**; see "Getting the MOZA SDK". |

Version 0.1.0. Built for and tested on a MOZA AY210 yoke base with FS25 1.24 on Windows 11.

## Status

Every part works on the real hardware, and the mod has been driven in the game:

- **Bridge and yoke:** tested on the real AY210 (2026-10-05), including the whole chain driven by
  simulated telemetry: centering that grows with speed, weight, and rumble all reach the motors.
- **Settings page:** tested in a browser on the Mac and served correctly on the PC.
- **FS25 mod:** loads in FS25 1.24, writes telemetry about 60 times a second, and the bridge
  follows it live (first drive 2026-10-05). The individual values (slip, surface roughness,
  implement state) have only been spot-checked. `fs-mod/README.md` lists what to look for.
- **Tuning:** the defaults are a first guess. Rumble in particular is too weak by default; see
  "Still to find out".

## Using it with Farming Simulator 25

1. Close X-Plane and any other flight simulator. The bridge refuses to start while one is running,
   because its start-up reset would cut that simulator's forces.
2. On the PC, run `Start FS25 yoke link.bat` in the folder the bridge was copied to. It starts the
   bridge and opens the settings page, http://127.0.0.1:8777/. The yoke gets a light centering.
3. Start FS25. When loading the savegame, tick **MOZA Yoke Link** in the mod list.
4. Get in a vehicle and drive. The page's status should change to "live, driving".
5. Tune with the sliders; changes apply at once. **Save** keeps them for next time
   (`moza_fs_bridge.ini` next to the program).

To stop: Ctrl+C in the bridge window, or close it. The page's **Release forces** button drops all
forces without stopping the bridge.

### What the bridge does and does not touch

- It sends the base temporary force effects only. They vanish when it stops, and they lapse by
  themselves within two seconds if it crashes.
- It never writes a setting to the base, and never touches MOZA Cockpit profiles or X-Plane files.
- The mod turns off FS25's own steering force feedback while it is loaded, so the game and the
  bridge do not fight. That is inside FS25 only. Set
  `MozaYokeLink.suppressGameForceFeedback = false` in the mod's script to keep the game's.
- In FS25, bind the yoke's roll axis to steering (Options > Controls). The game then maps it
  straight to steering angle with no centering of its own; centering comes from the bridge.

### Multiplayer

Untested. FS25 requires every player who joins a game to have the same mods the host has active,
as identical files, and it can only auto-download mods that come from the ModHub. So if you enable
this mod in a multiplayer save, everyone joining needs the same `FS25_MozaYokeLink.zip` in their
mods folder (and a dedicated server needs it too). The bridge and the yoke are only needed by you.

For the others the mod does nothing they would notice, with one exception: it also turns off
FS25's own steering force feedback on their PC, so anyone with a force-feedback wheel should set
`MozaYokeLink.suppressGameForceFeedback = false` in their copy. Then the files differ and the game
may refuse the join, so for a group with wheels it is simpler not to enable the mod in that save.
On a client, slip is estimated and "working" may read 0, because those are computed on the server.

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
scp bridge/build-win/moza_fs_bridge.exe bridge/build-win/MOZA_SDK.dll "bridge/windows/Start FS25 yoke link.bat" <pc>:moza-farmsim-link/
```

The mod is packaged by `fs-mod/package.sh` and installed by copying the zip into FS25's mods folder:

```bash
fs-mod/package.sh && scp fs-mod/dist/FS25_MozaYokeLink.zip "<pc>:Documents/My Games/FarmingSimulator2025/mods/"
```

Building natively with Visual Studio 2022 and CMake ≥ 3.21 (`cmake -S bridge -B bridge/build -A x64`)
is set up in `CMakeLists.txt` but has never been tried.

## Test modes (no game needed)

Run from the bridge's folder on the PC. All but `--list` apply force, so keep clear of the yoke.

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

## Still to find out

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
