# FS25_MozaYokeLink: telemetry mod for Farming Simulator 25

A script-only mod (PC) that rewrites one small text file with the state of the vehicle you are
driving, for the bridge in `../bridge/`. The file format is fixed by
[`../docs/telemetry.md`](../docs/telemetry.md). While the bridge is running on the same PC, the
mod also stops the game from sending its own steering force feedback, so the two do not fight
over the yoke. Without the bridge it leaves the game's force feedback exactly as it is.

| Path | What |
| --- | --- |
| `FS25_MozaYokeLink/` | The mod: `modDesc.xml`, icon, `scripts/MozaTelemetry.lua` (pure gathering, formatting and the presence rules), `scripts/MozaYokeLink.lua` (file output, game hooks, bridge detection, force feedback switch) |
| `tests/run_tests.lua` | Runs both scripts under plain Lua with mocked game objects |
| `package.sh` | Checks, tests and builds `dist/FS25_MozaYokeLink.zip` |
| `tools/make_icon.py` | Regenerates the icon (512x512 DXT1 `.dds`, one mip level, the layout other FS25 mods ship) |

## Install

1. Build the zip (or use the one in `dist/`):

   ```bash
   ./package.sh
   ```

2. Copy `dist/FS25_MozaYokeLink.zip` into `Documents\My Games\FarmingSimulator2025\mods\`. Do not
   rename or unpack it: the files must sit at the root of the zip and the name must stay
   `FS25_MozaYokeLink`. (Copying the `FS25_MozaYokeLink` folder itself into `mods\` also works, for
   editing the scripts in place.)
3. Start the game, pick the savegame, and in the mod selection screen that follows tick
   **MOZA Yoke Link** before starting. Mods are chosen per savegame; an existing save needs it
   ticked once.

Scripts are read when the game starts, so restart the game after replacing the zip.

## Where the files are

```
Documents\My Games\FarmingSimulator2025\modSettings\FS25_MozaYokeLink\telemetry.txt   written by the mod
Documents\My Games\FarmingSimulator2025\modSettings\FS25_MozaYokeLink\bridge.xml      written by the bridge
```

The folder is created by the mod. The exact paths are printed in the game log (below), which
matters if Documents is redirected, for example by OneDrive. The mod only reads `bridge.xml`;
it never writes, creates or deletes it.

## Confirm it is working

In `Documents\My Games\FarmingSimulator2025\log.txt`, search for `[MozaYokeLink]`. A healthy
start looks like this, in this order:

```
[MozaYokeLink] version 0.1.1.0 script loaded
[MozaYokeLink] version 0.1.1.0, telemetry file: C:/Users/.../modSettings/FS25_MozaYokeLink/telemetry.txt
[MozaYokeLink] first write ok (io): MOZAFS 1 seq=1 t=0 active=0 end=1
[MozaYokeLink] force feedback override installed on InputBinding class; the game keeps its steering force feedback until the bridge shows up in C:/Users/.../modSettings/FS25_MozaYokeLink/bridge.xml (checked with XMLFile.loadIfExists)
```

and, the first time you sit in a vehicle:

```
[MozaYokeLink] driving 'John Deere 6R 150' (found via g_localPlayer:getCurrentVehicle); sources: speed<-lastSignedSpeed; steer<-rotatedTime; mass<-getTotalMass(true)+implements; engine<-getMotorRpmReal/getMotorLoadPercentage; slip<-getWheelShapeSlip (4 wheels, differentials); surface<-wheel.physics (4 wheels in contact); implements<-1 attached
[MozaYokeLink] sample: MOZAFS 1 seq=2 t=913417 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 slip=0.04 rough=0.70 field=1 lowered=1 working=1 end=1
[MozaYokeLink] input device 0: 'MOZA AY210 Base' category=3 forceFeedbackAxes=0,1
[MozaYokeLink] vehicle spec_drivable.forceFeedback: isActive=false axisIndex=0 intensity=1
[MozaYokeLink] game asked about steering force feedback: device 'MOZA AY210 Base' category=3 axis=0 supported=true -> left to the game (no bridge)
```

and each time the bridge starts or stops:

```
[MozaYokeLink] bridge detected: suppressing the game's steering force feedback
[MozaYokeLink] bridge gone: the game's steering force feedback is back in its hands
```

- The `sources:` list names the game API that supplied each key for that vehicle. `none` means
  the key is left out of the file (the reader then uses its default). Lines starting
  `[MozaYokeLink] problem:` name anything that failed; each is logged once, at most ten in total.
- `input device ...` lines answer "does the game think this device has force feedback axes".
  `game asked about steering force feedback ...` appears once you steer and shows what the game's
  own function says about the steering binding. It ends `-> left to the game (no bridge)` or
  `-> suppressed (bridge running)`; each distinct answer is logged once.
- With the bridge running and no `bridge detected` line within a few seconds, look for
  `bridge.xml is there but its beat is not changing` or `... gets no beat from it`. Neither
  line means the file is not in the folder the log names.
- No `[MozaYokeLink]` lines at all: the mod is not ticked for the savegame, or the game rejected
  the zip. Look for `FS25_MozaYokeLink` near the top of the log.

In the telemetry file (open it in a viewer that reloads, or run
`Get-Content telemetry.txt -Wait` in PowerShell): one line starting `MOZAFS 1 ` and ending
`end=1`; `seq` counts up about 60 times a second while driving and once a second on foot;
`active` follows getting in and out; `steer` goes positive when you steer right; `speed` goes
negative in reverse. With the game paused the file stops changing. That is intended: the bridge
treats a `seq` that has not moved for 500 ms as stale.

## Force feedback: the game or the bridge

The game's steering force feedback is switched off only while the bridge is running on the
same PC. The bridge says so by replacing `bridge.xml` twice a second
([contract](../docs/telemetry.md#presence-file-bridge--mod)).

- The mod reads the file about once a second. The bridge counts as running from the moment
  its `beat` is seen to change, and as stopped once the beat has not changed for 3 seconds or
  the file is gone. A file left behind by a crashed bridge never changes, so it never counts.
- While the bridge is running, the game is told that no steering binding supports force
  feedback, so it never commands the device, and force feedback the game already had active on
  the vehicle is released. When the bridge stops the mod does nothing: the game asks again on
  the next steering input and switches its force feedback back on by itself. No need to leave
  the vehicle.
- Without the bridge the mod passes every call to the game's own function unchanged.
- The file is XML because that is what a mod can read back: the game's `io.open` only writes
  (asking it to read would empty the file). Each read asks `fileExists` first, so a missing
  file never reaches the XML loader, then loads the file, takes `bridge#version` and
  `bridge#beat`, and frees the handle. A file that is there but yields no beat (another
  version, not XML) counts as "no beat"; after a minute of that the mod stops looking until
  the next savegame load, and says so once.
- Paused: the mod keeps checking for as long as the game keeps calling it, so a bridge started
  or stopped during a pause is noticed then. If the game stood still altogether (loading, a
  long freeze), the time that passed is counted from the wall clock: on the first frame
  afterwards the file is read again, and suppression continues only if the beat changed in
  the meantime, and then only until the next read unless it changes again.

**Multiplayer.** Every player and the server need the identical zip, as with any script mod.
Only the player running the bridge gets the game's force feedback switched off; everyone else
has no `bridge.xml` and keeps the game's force feedback as if the mod were not there. Each
player's telemetry file is written on their own PC. A dedicated server loads the mod and does
nothing with it.

## Settings (top of the scripts)

- `MozaYokeLink.suppressGameForceFeedback` in `scripts/MozaYokeLink.lua`, default `true`: the
  master switch for the behaviour above. Set it to `false` to leave the game's force feedback
  alone even while the bridge runs; the log then says `game force feedback left alone` and
  `bridge.xml` is not looked at. Turning the in-game force feedback strength to 0 is not a
  substitute: the game still commands the device every frame, with zero force.
- `MozaTelemetry.ACTIVE_INTERVAL_MS` in `scripts/MozaTelemetry.lua`, default `1000 / 60`. Raise it
  (up to 50, the contract's 20 writes a second) if rewriting the file every frame costs frame
  time, for example with a virus scanner watching the folder.
- `MozaTelemetry.ROUGH` and `MozaTelemetry.FIELD_ROUGH_BY_NAME`: the surface roughness table.

## Tests

```bash
lua tests/run_tests.lua
```

The game cannot be run here, so the tests feed the same code mocked vehicles. They check the
line against the contract (prefix, `end=1` last, plain decimals, key order, the contract's own
example reproduced byte for byte from a mocked tractor with a cultivator), sign conventions,
fallbacks, that vehicles which raise errors or hold junk never break the line or flood the log,
the write rate, the file output with and without `io.open`, the presence file through mocks
of the game's XML functions (no file, beat changing, frozen, gone, wrong version and garbage,
read once a second at any frame rate, every handle freed, pause, a game that stood still) and
the force feedback override
(passes through without the bridge, suppresses with it, hands back, restored on unload,
harmless when the game function is missing). `package.sh` also runs `luac -p` on every Lua
file. The scripts keep to Lua 5.1 syntax; a lint test enforces it.

## Game APIs relied on

"Source" means the FS25 script source GIANTS publishes (read through the community mirrors
[FS25-Community-LUADOC](https://github.com/umbraprior/FS25-Community-LUADOC) and
[fs25-lua-api](https://github.com/MyGameSteamOfficial/fs25-lua-api)). "Mods" means working FS25
mods that use the same call, including the two installed on the target PC (DashboardLive,
mouseSteering). Every use is nil-checked and wrapped, so a wrong guess costs a key, not a crash.

| API | Used for | Confidence |
| --- | --- | --- |
| `modDesc.xml` with `descVersion="111"`, `<extraSourceFiles>`, `<iconFilename>` (`.dds`), `<multiplayer supported>` | Loading | High: mods with 111 and 113 load on the target game (1.24.0.0) |
| `addModEventListener(obj)` with `obj:loadMap()`, `obj:update(dt)` (dt in ms), `obj:deleteMap()` | Hooks | High: mods (Courseplay FS25, DashboardLive, mouseSteering) |
| `print()` to `log.txt` | Logging | High |
| `getUserProfileAppPath()`, `createFolder(path)` | File location | High: mods (Courseplay FS25, VDTelemetry; `createFolder` in both installed mods). Fallback: `g_currentModSettingsDirectory` |
| `io.open(path, "w")`, `file:write`, `file:close` | Writing the file | Confirmed in game (1.24). The game's `io` is its own script, not Lua's: in the published FS22 source `io.open` calls `createFile(path, FileAccess.WRITE)` whatever mode is asked for, and FS25 mods report the same limit. So it is never called in any mode but `"w"` |
| `createFile(path, FileAccess.WRITE)`, `fileWrite`, `delete` | Fallback writer | Medium-high: this is what the game's own `io.open` does in the FS22 source; the functions exist in FS25 |
| `fileExists(path)` | Is `bridge.xml` there | High: mods (mouseSteering, DashboardLive) |
| `XMLFile.loadIfExists(name, path)`, `xmlFile:getInt("bridge#beat")`, `xmlFile:delete()` | Reading `bridge.xml` | High: the same three calls are how VDTelemetry (commands file, every 100 ms) and fs25-farmhand-mod (session file) poll a file another program rewrites, and both depend on each load returning the current content. `loadIfExists` and `getInt` are also used by the installed mouseSteering. In the FS22 source the class is a thin wrapper: every load is a new `loadXMLFile` handle, nothing is cached in script |
| `loadXMLFile(name, path)` (0 on failure), `getXMLInt(handle, path)`, `delete(handle)` | Fallback reader if the class is missing | Medium-high: documented engine functions in FS25; what the class calls in the FS22 source |
| `getTimeSec()` | Noticing that the game stood still | High: source (FS25 `VehicleDebug.lua`) and mods (Courseplay FS25). Without it only frame time is counted |
| `g_localPlayer:getCurrentVehicle()` | Which vehicle | Confirmed in game (1.24); source (`Drivable.lua`) and mods |
| `g_currentMission.hud.controlledVehicle`, `g_currentMission.controlledVehicle` | Fallbacks for the above | Medium (DashboardLive uses the first) / FS22 only |
| `g_currentMission.time` (ms), `g_time` | `t` | High: source (`WorkArea.lua`), mods |
| `g_currentMission.paused` | Stop writing while paused | Medium: mods only |
| `vehicle.lastSignedSpeed` (m/ms), `getLastSpeed()` (km/h), `lastSpeedReal`, `movingDirection` | `speed` | High: source (`Vehicle.lua`) |
| `vehicle.rotatedTime`, `maxRotTime`, `minRotTime`; `spec_drivable.axisSide`, `steeringDirection` | `steer` | High: source (`Drivable.updateVehiclePhysics` maps a left axis to `0..maxRotTime` and a right axis to `0..minRotTime`), mirrored by mouseSteering. Sign to confirm in game |
| `vehicle:getTotalMass(true)` (tonnes), `getAttachedImplements()` and `implement.object`, `spec_attachable.isHardAttached` | `mass` | High: source (`Vehicle.lua`, `AttacherJoints.lua`) |
| `vehicle:getMotorRpmReal()`, `getMotorLoadPercentage()`, `getIsMotorStarted()`; `spec_motorized.motor:getLastModulatedMotorRpm()` etc. | `rpm`, `load` | High: source (`Motorized.lua`, `VehicleMotor.lua`) |
| `spec_motorized.differentials[].diffIndex1/2`, `diffIndex1/2IsWheel`; `spec_wheels.wheels[]` | Which wheels are driven | High: source (`Motorized.lua`, `Wheels.lua`) |
| `getWheelShapeSlip(wheel.node, wheel.physics.wheelShape)`, `physics.hasGroundContact`, `physics.wheelShapeCreated` | `slip` | Medium-high: source (`CrabSteering.lua` treats the first result as a ratio with 1 = full slip). Only where the physics runs (single player, host) |
| `wheel.physics.netInfo.xDriveSpeed` (rad/s), `physics.radius`, `netInfo.slip` | `slip` fallbacks | Medium: source (`WheelPhysics.lua`); reads slightly high in tight turns |
| `wheel.physics.contact` vs `WheelContactType.*`, `physics.densityType` vs `FieldGroundType.*`, `physics.groundDepth`, `physics.hasSnowContact` | `rough`, `field` | Medium-high for the fields (source, `WheelPhysics.lua`). The 0.1 / 0.8 depth thresholds are the game's own (`WheelsUtil.getGroundType`). Field state names other than `NONE`, `GRASS`, `GRASS_CUT`, `SOWN`, `PLANTED`, `HARVEST_READY`, `ROLLER_LINES`, `ROLLED_SEEDBED` are carried over from FS22; a missing name just falls back to 0.7 |
| `vehicle:getIsOnField()` | `field` fallback | High: source |
| `object:getIsLowered(false)`, `attacher:getAttacherJointDescFromObject(object).allowsLowering`, `spec_pickup`, `spec_foldable.foldMiddleAnimTime` | `lowered` | High for the calls (source, `Attachable.lua`); the rule for which joints count is mine |
| `spec_workArea.workAreas[]`, `object:getIsWorkAreaProcessing(area)`, `getIsWorkAreaActive(area)`, `area.requiresGroundContact`, `area.groundReferenceNode`, `WorkAreaType.AUXILIARY` | `working` | High: source (`WorkArea.lua`) |
| `InputBinding.getBindingForceFeedbackInfo(binding)` returning `isSupported, device, axisIndex`; `spec_drivable.forceFeedback.{isActive, device, axisIndex}`; `device:setForceFeedback(axis, force, position)` | Suppressing the game's force feedback, and handing it back | Wrapping the function on the class table is confirmed in game (1.24). High for the caller and the cached state (source, `Drivable.lua`): while `forceFeedback.isActive` is false, `actionEventSteer` asks again on every steering event, which is what switches the game's force feedback back on once the wrapper passes through. The function body is only published for FS22, where "unsupported" is `false, nil, 0` |
| `getNumOfGamepads()`, `getGamepadName(id)`, `getGamepadCategory(id)`, `getHasGamepadAxisForceFeedback(id, axis)`, `Input.MAX_NUM_AXES` | One-time device diagnostics | Medium: the functions exist in FS25; argument order is from FS22 |
