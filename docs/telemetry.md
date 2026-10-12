# Telemetry file: FS25 mod → bridge

The FS25 Lua sandbox has no sockets, so the mod rewrites one small text file and the bridge polls it.
This file is the contract between `fs-mod/` (writer) and `bridge/` (reader).

## Location

```
<FS25 user profile>/modSettings/FS25_MozaYokeLink/telemetry.txt
```

The user profile is normally `Documents\My Games\FarmingSimulator2025` (in Lua:
`getUserProfileAppPath()`). The bridge takes the path as an option and defaults to that location.

## Format

The whole file is one ASCII line, rewritten in full on every update:

```
MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 rpmmin=850 rpmmax=2200 slip=0.04 rough=0.70 surface=6 field=1 lowered=1 working=1 starting=0 electric=0 bumpl=0.00 bumpr=0.00 end=1
```

- The line starts with the literal `MOZAFS`, then the format version (`1`), then space-separated
  `key=value` tokens. Line ending is `\n`.
- **The last token is always `end=1`.** The file is not written atomically, so a reader that does
  not see both the `MOZAFS 1 ` prefix and the trailing `end=1` has caught a partial write: it
  discards that read and keeps the previous values.
- Numbers use `.` as the decimal separator, no thousands separators, no exponent notation.
- Readers ignore keys they do not know and use the default for keys that are missing, so keys can
  be added without bumping the version.

## Keys (version 1)

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `seq` | integer | required | Incremented by one on every write. The reader uses it to tell a fresh sample from a file that has stopped changing. |
| `t` | integer ms | 0 | Game time of the sample in milliseconds. |
| `active` | 0/1 | 0 | 1 while the local player is driving a vehicle. When 0 the remaining keys may be omitted. |
| `speed` | m/s, signed | 0 | Forward speed; negative when reversing. |
| `steer` | -1..1 | 0 | Current steering position as the game applies it to the wheels. **Positive is steering right.** |
| `mass` | kg | 0 | Total mass of the vehicle plus everything attached to it. |
| `rpm` | 1/min | 0 | Engine speed. |
| `load` | 0..1 | 0 | Engine load. |
| `rpmmin` | 1/min | 0 | This vehicle's idle engine speed. 0 or missing means unknown. |
| `rpmmax` | 1/min | 0 | This vehicle's maximum engine speed. 0 or missing means unknown. Together with `rpmmin` it lets the bridge place `rpm` between idle and flat out for whatever is being driven. |
| `slip` | 0..1 | 0 | Largest longitudinal slip among the driven wheels. |
| `rough` | 0..1 | 0 | How rough the surface under the vehicle is: 0 smooth asphalt, about 0.3 a dirt road, about 0.5 grass, about 0.7 a field, 1 the roughest (e.g. freshly ploughed). |
| `surface` | 1..8 | unknown | What kind of ground most of the wheels are on; see [Surfaces](#surfaces). Left out when the mod cannot tell. |
| `field` | 0/1 | 0 | 1 while the vehicle is on a field. |
| `lowered` | 0/1 | 0 | 1 while any attached implement is lowered. |
| `working` | 0/1 | 0 | 1 while any attached implement is actively working the ground. |
| `starting` | 0/1 | 0 | 1 while the engine is being started: from the moment the start is triggered until the engine is running. Never 1 for an electric vehicle. |
| `startms` | integer ms | 0 | How long this machine takes to start: the time `starting` stays 1 for one start. 0 or missing means the game does not say. The bridge uses it to time its start-up shake to the machine. |
| `electric` | 0/1 | 0 | 1 when the vehicle is driven by an electric motor and has no combustion engine. `rpm` and `load` are still reported for it; the bridge uses this flag to leave out engine vibration. |
| `bumpl` | 0..1 | 0 | How hard the **left** steered wheel is being jolted by something under it right now, e.g. climbing a log, a kerb or a rock: 0 on even ground, about 0.3 a clear bump, 1 a violent hit. Left and right are from the driver's seat. |
| `bumpr` | 0..1 | 0 | The same for the **right** steered wheel. |

### Surfaces

`rough` says how rough the ground is; `surface` says what it is, so that the bridge can give each
kind its own feel rather than the same rumble at different strengths. The numbers are fixed:

| `surface` | Ground |
|---|---|
| 1 | Paved: asphalt, concrete, bridges, yard slabs. |
| 2 | Gravel and dirt tracks: hard ground that is not paved. |
| 3 | Grass, meadows and other soft ground that is not a field. |
| 4 | Snow. |
| 5 | A heap of tipped material. |
| 6 | A field with a settled surface (sown, planted, a seedbed, rolled, ready to harvest), or a field whose state the mod does not know. |
| 7 | A field broken into clods: ploughed, cultivated, or stubble-tilled. |
| 8 | A field laid up in ridges. |

With wheels on different ground, the kind most of them are on is sent, and on an even split the
rougher of the two. A reader must treat a missing key, and any number it does not know, as
"unknown" and fall back to a plain rumble scaled by `rough`: later versions may add kinds.

### The machine's own movement

| Key | Unit | Default | Meaning |
|---|---|---|---|
| `rollrate` | rad/s, signed | 0 | How fast the machine is rocking sideways. **Positive is leaning over to the right** (its left side rising). |
| `pitchrate` | rad/s, signed | 0 | How fast it is pitching. Positive is the nose rising. |
| `heave` | m/s², signed | 0 | Vertical acceleration of the machine's body. Positive is upward. A steady climb is 0. |

These describe the body the driver sits in, not the wheels: what is seen as the cab swaying
against the horizon when driving off road. The mod takes them from where the vehicle is and which
way it faces on each frame, smooths them over about 70 ms, and reports them from the driver's
seat (with the seat turned round, left and right and front and back swap). They are left out when
the game does not give the vehicle's place in the world. A frame longer than a quarter of a
second, or a jump of more than 3 m, counts as a hitch or a reset and starts them again from 0.

### Bumps

`bumpl` and `bumpr` describe single events, not a steady state, and a hit can be over within one
frame. So the mod holds each value at its peak and lets it fall back to 0 over roughly 150 ms; the
bridge then cannot miss it between two polls. Ordinary field roughness belongs in `rough` and
should leave both near 0. The bridge turns the difference into a kick of the wheel toward the
side that was hit; equal bumps on both sides cancel.

## Timing

- The mod writes at most 60 times a second and at least 20 while `active=1`. When the player is not
  in a vehicle it writes `active=0` about once a second so the file keeps moving.
- The bridge polls at about 60 Hz. If `seq` has not changed for 500 ms the telemetry is stale: the
  game is paused, in a menu, or gone, and the bridge fades to its idle forces.

# Presence file: bridge → mod

The mod turns off the game's own steering force feedback only while the bridge is running on the
same machine. Players without the bridge (for example the others in a multiplayer game) keep the
game's force feedback. The bridge announces itself with a second file in the same folder.

It is XML because that is what the game's Lua can read back: its `io.open` only writes.

## Location

```
<FS25 user profile>/modSettings/FS25_MozaYokeLink/bridge.xml
```

It sits next to `telemetry.txt`; the bridge derives its path from the telemetry file's folder.

## Format

Rewritten in full about twice a second while the bridge is linked:

```xml
<?xml version="1.0" encoding="utf-8" standalone="no"?>
<bridge version="1" beat="4821"/>
```

- `version` is the format version, 1. A reader ignores a file with any other value.
- `beat` is a positive integer that goes up by one on every write.

## Rules

- **Bridge:** writes only while it is running its game link, and only if the folder already
  exists (the mod creates it). It replaces the file atomically (a temporary file in the same
  folder, renamed over `bridge.xml`), so a reader never sees half a file; if the rename is refused
  because the game has the file open at that instant, that beat is skipped. It deletes the file
  when it stops cleanly.
- **Mod:** reads the file about once a second. The bridge counts as present from the moment
  `beat` is seen to change, and stops counting as present when `beat` has not changed for
  3 seconds of real time or the file is gone. A file left behind by a crash therefore never counts.
- While the bridge is present, and the mod's `suppressGameForceFeedback` switch is on, the game's
  steering force feedback is suppressed. At all other times the mod leaves it exactly as the game
  has it.
