--
-- MozaTelemetry.lua  --  pure telemetry gathering and line formatting.
--
-- Part of FS25_MozaYokeLink. Nothing in this file touches files, timers or mod
-- event hooks, so it also runs under plain Lua with mocked game objects (see
-- fs-mod/tests). The engine-facing half is MozaYokeLink.lua.
--
-- Output contract: docs/telemetry.md. One line, for example
--   MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 rpmmin=850 rpmmax=2200 slip=0.04 rough=0.70 surface=6 field=1 lowered=1 working=1 starting=0 electric=0 bumpl=0.00 bumpr=0.00 end=1
--
-- It also holds the rules for the bridge's presence file (same document,
-- "Presence file: bridge -> mod"): deciding from a series of reads whether
-- the bridge is running.
--
-- Rules followed throughout:
--   * Syntax stays Lua 5.1 compatible (FS25 runs Luau; the tests run PUC Lua).
--   * Every value read from the game is nil/type checked. A value that cannot
--     be read is left out of the line; the reader has a default for every key.
--   * Each key is gathered inside its own pcall, so one odd vehicle type can
--     cost a key but never the line and never an error in the game log.
--

MozaTelemetry = {}

local M = MozaTelemetry

M.FORMAT_NAME = "MOZAFS"
M.FORMAT_VERSION = 1

-- Timing from the contract: at most 60 writes a second while driving, about
-- one a second while not.
M.ACTIVE_INTERVAL_MS = 1000 / 60
M.IDLE_INTERVAL_MS = 1000

-- Surface roughness scale from the contract.
M.ROUGH = {
    ROAD = 0.0,          -- asphalt, concrete, bridges, anything that is not terrain
    DIRT_ROAD = 0.3,     -- hard terrain: gravel and dirt tracks
    GRASS = 0.5,         -- soft terrain and meadows
    SNOW = 0.4,
    HEAP = 0.6,          -- driving over a heap of tipped material
    FIELD = 0.7,         -- a field whose state we do not know better
    PLOWED = 1.0,
}

-- Roughness per field ground state, keyed by the FieldGroundType member name.
-- Names that a game version does not have are simply skipped at lookup time.
M.FIELD_ROUGH_BY_NAME = {
    PLOWED = 1.0,
    RIDGE = 0.9,
    CULTIVATED = 0.85,
    STUBBLE_TILLAGE = 0.8,
    RIDGE_SOWN = 0.8,
    SOWN = 0.7,
    DIRECT_SOWN = 0.7,
    PLANTED = 0.7,
    HARVEST_READY = 0.7,
    HARVEST_READY_OTHER = 0.7,
    SEEDBED = 0.65,
    ROLLER_LINES = 0.6,
    ROLLED_SEEDBED = 0.55,
}

-- Kind of surface, key `surface`: what the ground feels like rather than how
-- rough it is. The numbers are the contract's and never change meaning.
M.SURFACE = {
    ROAD = 1,       -- asphalt, concrete, bridges, yard slabs
    GRAVEL = 2,     -- gravel and dirt tracks
    GRASS = 3,
    SNOW = 4,
    HEAP = 5,       -- a heap of tipped material
    FIELD = 6,      -- a field with a settled surface, or one we know no better
    TILLED = 7,     -- broken into clods: ploughed, cultivated
    RIDGE = 8,      -- laid up in ridges
}
M.SURFACE_COUNT = 8

-- Field ground states that are not a plain FIELD, keyed like FIELD_ROUGH_BY_NAME.
M.FIELD_SURFACE_BY_NAME = {
    PLOWED = M.SURFACE.TILLED,
    CULTIVATED = M.SURFACE.TILLED,
    STUBBLE_TILLAGE = M.SURFACE.TILLED,
    RIDGE = M.SURFACE.RIDGE,
    RIDGE_SOWN = M.SURFACE.RIDGE,
}

-- The game's own thresholds for the terrain "depth" attribute
-- (WheelsUtil.getGroundType): below 0.1 is road, above 0.8 is soft terrain.
M.DEPTH_ROAD = 0.1
M.DEPTH_SOFT = 0.8

-- Bumps: a steered wheel being jolted by something under it (bumpl, bumpr).
-- The signal is the wheel's tire load against its own running average: a log
-- or a kerb multiplies the load for an instant, soft or uneven ground moves
-- it a little, and braking or a lifted implement shift it slowly, which the
-- running average follows. All of these are first guesses to be calibrated
-- in the game; the mod logs what it measures (see updateBumps).
M.BUMP_HOLD_MS = 150        -- a peak is held and falls back to 0 over this time
M.BUMP_MIN_SPEED = 0.25     -- m/s (0.9 km/h): slower than this nothing counts
M.BUMP_LOAD_START = 1.5     -- tire load of 1.5 times its running average: bump 0
M.BUMP_LOAD_FULL = 4.0      -- 4 times: bump 1 (so 2.25 times is the "clear bump" 0.3)
M.BUMP_BASELINE_MS = 1000   -- time constant of the running average
M.BUMP_WARMUP_MS = 500      -- no bumps until the average has settled
M.BUMP_GAP_MS = 250         -- a frame longer than this is a hitch, not a jolt
-- Fallback when the tire load cannot be read: how fast the suspension moves.
M.BUMP_SUSP_START = 0.25    -- m/s: bump 0 (the game's dampers call 0.10 m/s "low speed")
M.BUMP_SUSP_FULL = 1.25     -- m/s: bump 1
-- Which wheels: the steered ones of the front-most steered axle.
M.BUMP_AXLE_DEPTH = 0.5     -- m: wheels this close behind the front-most one are the same axle
M.BUMP_CENTRE_WIDTH = 0.05  -- m: a wheel this close to the centre line counts for both sides
-- One-time calibration lines in the game log.
M.BUMP_NOTE_STRONG = 0.5    -- the first bump at least this big is logged
M.BUMP_NOTE_AFTER_MS = 20000 -- driving time on a field / off a field before its summary

local MAX_IMPLEMENT_DEPTH = 8

---------------------------------------------------------------------------
-- small helpers
---------------------------------------------------------------------------

local function isFinite(x)
    return type(x) == "number" and x == x and x > -math.huge and x < math.huge
end
M.isFinite = isFinite

local function clamp(x, lo, hi)
    if x < lo then
        return lo
    elseif x > hi then
        return hi
    end
    return x
end
M.clamp = clamp

local function isFunc(f)
    return type(f) == "function"
end

--- Integer as text, or nil if the value is not a usable number.
function M.fmtInt(x)
    if not isFinite(x) or math.abs(x) > 1e12 then
        return nil
    end
    local n = math.floor(math.abs(x) + 0.5)
    if n == 0 then
        return "0"
    end
    return string.format("%s%d", x < 0 and "-" or "", n)
end

--- Fixed-point decimal as text, or nil if the value is not a usable number.
-- Built from integers on purpose: "%f" follows the C locale (a decimal comma
-- on some systems) and "%g" switches to exponent notation, and the contract
-- allows neither.
function M.fmtFixed(x, decimals)
    if not isFinite(x) or math.abs(x) > 1e12 then
        return nil
    end
    local scale = 1
    for _ = 1, decimals do
        scale = scale * 10
    end
    local n = math.floor(math.abs(x) * scale + 0.5)
    local whole = math.floor(n / scale)
    local frac = n - whole * scale
    local sign = (x < 0 and n ~= 0) and "-" or ""
    return string.format("%s%d.%0" .. decimals .. "d", sign, whole, frac)
end

local function flag(x)
    if x == true or x == 1 then
        return "1"
    elseif x == false or x == 0 then
        return "0"
    end
    return nil
end

---------------------------------------------------------------------------
-- line formatting
---------------------------------------------------------------------------

-- key, formatter, lower clamp, upper clamp -- in the order they are written.
local KEYS = {
    { "speed",   function(v) return M.fmtFixed(clamp(v, -200, 200), 2) end },
    { "steer",   function(v) return M.fmtFixed(clamp(v, -1, 1), 3) end },
    { "mass",    function(v) return M.fmtInt(clamp(v, 0, 10000000)) end },
    { "rpm",     function(v) return M.fmtInt(clamp(v, 0, 100000)) end },
    { "load",    function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "rpmmin",  function(v) return M.fmtInt(clamp(v, 0, 100000)) end },
    { "rpmmax",  function(v) return M.fmtInt(clamp(v, 0, 100000)) end },
    { "slip",    function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "rough",   function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "surface", function(v)
        local kind = math.floor(v + 0.5)
        if kind >= 1 and kind <= M.SURFACE_COUNT then
            return M.fmtInt(kind)
        end
        return nil
    end },
    { "field",   flag },
    { "lowered", flag },
    { "working", flag },
    { "starting", flag },
    { "electric", flag },
    { "bumpl",   function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "bumpr",   function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "rollrate",  function(v) return M.fmtFixed(clamp(v, -9.99, 9.99), 2) end },
    { "pitchrate", function(v) return M.fmtFixed(clamp(v, -9.99, 9.99), 2) end },
    { "heave",     function(v) return M.fmtFixed(clamp(v, -99.9, 99.9), 1) end },
    { "startms",   function(v) return M.fmtInt(clamp(v, 0, 60000)) end },
}

--- Build the telemetry line (including the trailing "\n").
-- @param seq    write counter (required)
-- @param timeMs game time in ms (optional)
-- @param sample table of values, or nil / {active = false} when not driving
function M.formatLine(seq, timeMs, sample)
    local parts = { M.FORMAT_NAME, tostring(M.FORMAT_VERSION) }
    parts[#parts + 1] = "seq=" .. (M.fmtInt(seq) or "0")

    local t = M.fmtInt(timeMs)
    if t ~= nil and string.sub(t, 1, 1) ~= "-" then
        parts[#parts + 1] = "t=" .. t
    end

    local active = type(sample) == "table" and (sample.active == true or sample.active == 1)
    parts[#parts + 1] = "active=" .. (active and "1" or "0")

    if active then
        for i = 1, #KEYS do
            local key, fmt = KEYS[i][1], KEYS[i][2]
            local value = sample[key]
            if value ~= nil then
                local okNumber = type(value) == "boolean" or isFinite(value)
                local text = okNumber and fmt(value) or nil
                if text ~= nil then
                    parts[#parts + 1] = key .. "=" .. text
                end
            end
        end
    end

    parts[#parts + 1] = "end=1"
    return table.concat(parts, " ") .. "\n"
end

---------------------------------------------------------------------------
-- write-rate limiter (pure, driven by the per-frame dt in ms)
---------------------------------------------------------------------------

function M.newThrottle()
    return { acc = 0, wasActive = nil }
end

--- Advance the limiter by one frame and say whether to write on this frame.
-- Active: at most 60 writes a second, every frame when the frame rate is lower.
-- Not active: about one write a second. A change of state writes at once.
function M.throttleStep(state, dt, active)
    if not isFinite(dt) or dt < 0 then
        dt = 0
    end
    active = active == true

    if state.wasActive ~= active then
        state.wasActive = active
        state.acc = 0
        return true
    end

    local interval = active and M.ACTIVE_INTERVAL_MS or M.IDLE_INTERVAL_MS
    state.acc = state.acc + dt
    -- small tolerance so a steady 60 fps (16.66.. ms frames) writes every frame
    if state.acc + 0.01 >= interval then
        state.acc = state.acc - interval
        if state.acc > interval or state.acc < 0 then
            state.acc = 0 -- do not bank time after a long frame
        end
        return true
    end
    return false
end

---------------------------------------------------------------------------
-- bridge presence (docs/telemetry.md, "Presence file: bridge -> mod")
---------------------------------------------------------------------------
--
-- The bridge replaces bridge.xml about twice a second with
--   <bridge version="1" beat="4821"/>
-- The mod looks at it about once a second. The bridge counts as present
-- from the moment the beat is seen to change, until it has not changed for
-- three seconds or the file is gone. A file left behind by a crash never
-- changes, so it never counts.
--
-- These functions only do the bookkeeping; MozaYokeLink.lua does the reading.

M.BRIDGE_FORMAT_VERSION = 1
M.PRESENCE_READ_INTERVAL_MS = 1000
M.PRESENCE_TIMEOUT_MS = 3000

-- What a read can report instead of a beat:
--   nil    nothing usable this time (unreadable, not our format)
--   false  the file is not there
M.FILE_GONE = false

--- The beat, if the two attributes read from bridge.xml are usable; else nil.
function M.bridgeBeat(version, beat)
    if version ~= M.BRIDGE_FORMAT_VERSION then
        return nil
    end
    if not isFinite(beat) or beat < 1 or beat ~= math.floor(beat) then
        return nil
    end
    return beat
end

function M.newPresence()
    return {
        present = false,
        token = nil,      -- the beat the last usable read returned
        tokenAgeMs = 0,   -- time since that read
        unchangedMs = 0,  -- time since the token was last seen to change
        sinceReadMs = nil -- time since the last read; nil before the first
    }
end

--- Advance the clocks by one frame. @return true when a read is due
-- @param elapsedMs real time since the previous call
-- @param force     read now (after a stretch in which no time was counted)
function M.presenceTick(state, elapsedMs, force)
    if not isFinite(elapsedMs) or elapsedMs < 0 then
        elapsedMs = 0
    end
    state.tokenAgeMs = state.tokenAgeMs + elapsedMs
    state.unchangedMs = state.unchangedMs + elapsedMs

    if state.sinceReadMs == nil then
        state.sinceReadMs = 0
        return true
    end
    local interval = M.PRESENCE_READ_INTERVAL_MS
    state.sinceReadMs = state.sinceReadMs + elapsedMs
    if force == true then
        state.sinceReadMs = 0
        return true
    end
    if state.sinceReadMs + 0.01 >= interval then
        state.sinceReadMs = state.sinceReadMs - interval
        if state.sinceReadMs > interval or state.sinceReadMs < 0 then
            state.sinceReadMs = 0
        end
        return true
    end
    return false
end

--- Take the result of one read. @return true while the bridge is present
function M.presenceObserve(state, token)
    local timeout = M.PRESENCE_TIMEOUT_MS

    if token == M.FILE_GONE then
        state.token = nil
        state.present = false
        return false
    end
    if type(token) ~= "number" and type(token) ~= "string" then
        token = nil
    end

    if token ~= nil then
        if state.token ~= nil and token ~= state.token then
            if state.tokenAgeMs <= timeout + 1 then
                -- changed since a recent read: the bridge is writing
                state.present = true
                state.unchangedMs = 0
            elseif state.present then
                -- Changed, but against a read from before a long gap (the game
                -- stood still). That shows the bridge wrote at some point, not
                -- that it still does: it gets one more read to change again.
                state.unchangedMs = math.max(timeout - M.PRESENCE_READ_INTERVAL_MS, 0)
                state.token = token
                state.tokenAgeMs = 0
                return true
            end
            -- otherwise: a change against an old read proves nothing; this
            -- read becomes the one to compare the next against
        end
        state.token = token
        state.tokenAgeMs = 0
    end

    if state.present and state.unchangedMs + 1 >= timeout then
        state.present = false
    end
    return state.present
end

---------------------------------------------------------------------------
-- which vehicle is the local player driving
---------------------------------------------------------------------------

--- @return vehicle or nil, name of the API that answered
function M.findVehicle()
    -- FS25: the player object knows its vehicle.
    local player = g_localPlayer
    if player ~= nil and isFunc(player.getCurrentVehicle) then
        local ok, vehicle = pcall(player.getCurrentVehicle, player)
        if ok then
            -- nil here is a real answer: the player is on foot
            return vehicle, "g_localPlayer:getCurrentVehicle"
        end
    end

    local mission = g_currentMission
    if type(mission) == "table" then
        -- FS25 HUD keeps the vehicle it is drawing for
        local hud = mission.hud
        if type(hud) == "table" and hud.controlledVehicle ~= nil then
            return hud.controlledVehicle, "g_currentMission.hud.controlledVehicle"
        end
        -- FS22 name
        if mission.controlledVehicle ~= nil then
            return mission.controlledVehicle, "g_currentMission.controlledVehicle"
        end
    end
    return nil, "none"
end

---------------------------------------------------------------------------
-- per-key gathering. Each returns value(s) plus a short source tag that the
-- engine layer logs once per vehicle, so the first in-game run shows which
-- API supplied which key.
---------------------------------------------------------------------------

--- Signed forward speed in m/s.
function M.getSpeed(vehicle)
    -- Vehicle.lastSignedSpeed: m per ms, lightly smoothed, negative in reverse
    local signed = vehicle.lastSignedSpeed
    if isFinite(signed) then
        return signed * 1000, "lastSignedSpeed"
    end

    local dir = vehicle.movingDirection
    local sign = (isFinite(dir) and dir < 0) and -1 or 1

    if isFunc(vehicle.getLastSpeed) then
        local kmh = vehicle:getLastSpeed() -- km/h, unsigned
        if isFinite(kmh) then
            return kmh / 3.6 * sign, "getLastSpeed*movingDirection"
        end
    end

    local real = vehicle.lastSpeedReal -- m per ms, unsigned
    if isFinite(real) then
        return real * 1000 * sign, "lastSpeedReal*movingDirection"
    end
    return nil, nil
end

--- Steering position, -1..1, positive = right.
-- rotatedTime runs from minRotTime (negative, full right) to maxRotTime
-- (positive, full left): Drivable.updateVehiclePhysics maps a negative
-- (left) axis to 0..maxRotTime and a positive (right) axis to 0..minRotTime.
function M.getSteer(vehicle)
    local rotatedTime = vehicle.rotatedTime
    local maxRotTime = vehicle.maxRotTime
    local minRotTime = vehicle.minRotTime
    if isFinite(rotatedTime) and isFinite(maxRotTime) and isFinite(minRotTime) then
        local steer = nil
        if rotatedTime >= 0 then
            if maxRotTime ~= 0 then
                steer = -(rotatedTime / math.abs(maxRotTime))
            elseif rotatedTime == 0 then
                steer = 0
            end
        else
            if minRotTime ~= 0 then
                steer = math.abs(rotatedTime) / math.abs(minRotTime)
            end
        end
        if steer ~= nil then
            return clamp(steer, -1, 1), "rotatedTime"
        end
    end

    -- Fallback: the smoothed steering input, positive = right.
    local spec = vehicle.spec_drivable
    if type(spec) == "table" and isFinite(spec.axisSide) then
        local dir = isFinite(spec.steeringDirection) and spec.steeringDirection or 1
        return clamp(spec.axisSide * dir, -1, 1), "spec_drivable.axisSide"
    end
    return nil, nil
end

local function attachedImplements(vehicle)
    if isFunc(vehicle.getAttachedImplements) then
        local list = vehicle:getAttachedImplements()
        if type(list) == "table" then
            return list
        end
    end
    local spec = vehicle.spec_attacherJoints
    if type(spec) == "table" and type(spec.attachedImplements) == "table" then
        return spec.attachedImplements
    end
    return nil
end

local function massOf(vehicle, depth, seen)
    if type(vehicle) ~= "table" or seen[vehicle] or depth > MAX_IMPLEMENT_DEPTH then
        return 0
    end
    seen[vehicle] = true
    if not isFunc(vehicle.getTotalMass) then
        return 0
    end

    -- getTotalMass(true): this vehicle only, in tonnes.
    local own = vehicle:getTotalMass(true)
    if not isFinite(own) or own < 0 then
        own = 0
    end
    local total = own

    local implements = attachedImplements(vehicle)
    if implements ~= nil then
        for _, implement in pairs(implements) do
            local object = type(implement) == "table" and implement.object or nil
            if type(object) == "table" and not seen[object] then
                local attachable = object.spec_attachable
                local hard = type(attachable) == "table" and attachable.isHardAttached == true
                local objectOwn = 0
                if hard and isFunc(object.getTotalMass) then
                    local m = object:getTotalMass(true)
                    if isFinite(m) and m > 0 then
                        objectOwn = m
                    end
                end
                local sub = massOf(object, depth + 1, seen)
                -- A hard-attached implement is already folded into the mass of
                -- the component it hangs on (AttacherJoints.getAdditionalComponentMass),
                -- so only what hangs behind it is new.
                total = total + math.max(sub - objectOwn, 0)
            end
        end
    end
    return total
end

--- Mass of the vehicle and everything attached to it, in kg.
function M.getMassKg(vehicle)
    if not isFunc(vehicle.getTotalMass) then
        return nil, nil
    end
    local tonnes = massOf(vehicle, 0, {})
    if not isFinite(tonnes) or tonnes <= 0 then
        return nil, nil
    end
    return tonnes * 1000, "getTotalMass(true)+implements"
end

--- Engine speed (1/min) and load (0..1).
function M.getEngine(vehicle)
    local spec = vehicle.spec_motorized
    if type(spec) ~= "table" then
        return nil, nil, nil
    end
    local motor = spec.motor
    if motor == nil and isFunc(vehicle.getMotor) then
        motor = vehicle:getMotor()
    end

    local rpm, load, source = nil, nil, {}

    if isFunc(vehicle.getMotorRpmReal) then
        rpm = vehicle:getMotorRpmReal()
        source[1] = "getMotorRpmReal"
    end
    if not isFinite(rpm) and type(motor) == "table" then
        if isFunc(motor.getLastModulatedMotorRpm) then
            rpm = motor:getLastModulatedMotorRpm()
            source[1] = "motor:getLastModulatedMotorRpm"
        end
        if not isFinite(rpm) and isFunc(motor.getLastMotorRpm) then
            rpm = motor:getLastMotorRpm()
            source[1] = "motor:getLastMotorRpm"
        end
        if not isFinite(rpm) and isFinite(motor.lastMotorRpm) then
            rpm = motor.lastMotorRpm
            source[1] = "motor.lastMotorRpm"
        end
    end
    if not isFinite(rpm) then
        rpm = nil
        source[1] = "-"
    end

    if isFunc(vehicle.getMotorLoadPercentage) then
        load = vehicle:getMotorLoadPercentage()
        source[2] = "getMotorLoadPercentage"
    end
    if not isFinite(load) and type(motor) == "table" and isFunc(motor.getSmoothLoadPercentage) then
        load = motor:getSmoothLoadPercentage()
        source[2] = "motor:getSmoothLoadPercentage"
    end
    if not isFinite(load) and isFinite(spec.smoothedLoadPercentage) then
        load = spec.smoothedLoadPercentage
        source[2] = "spec_motorized.smoothedLoadPercentage"
    end
    if not isFinite(load) then
        load = nil
        source[2] = "-"
    end

    -- A stopped engine keeps its last rpm/load in these fields; report zero.
    -- getIsMotorStarted is true only once the engine runs, so rpm also reads 0
    -- while it is being started (the game itself ramps its rpm up from zero
    -- during that time); the "starting" key covers that span.
    if isFunc(vehicle.getIsMotorStarted) and vehicle:getIsMotorStarted() ~= true then
        if rpm ~= nil then rpm = 0 end
        if load ~= nil then load = 0 end
        source[3] = "(engine not running: reported as 0)"
    end

    if rpm ~= nil then rpm = math.max(rpm, 0) end
    if load ~= nil then load = clamp(load, 0, 1) end
    return rpm, load, table.concat(source, "/")
end

-- An engine speed above this is not an engine speed.
M.RPM_RANGE_LIMIT = 30000

--- This vehicle's idle and maximum engine speed (1/min).
-- VehicleMotor is given both when the vehicle loads (motor#minRpm and
-- motor#maxRpm of its XML) and never changes them; what limits the engine
-- while driving (speed limit, rpm limit, PTO) is kept elsewhere. They
-- describe the machine, so they are reported with the engine off as well.
-- @return min, max, source; or nil when they cannot be read or make no sense
function M.getRpmRange(vehicle)
    local spec = vehicle.spec_motorized
    if type(spec) ~= "table" then
        return nil, nil, nil
    end
    local motor = spec.motor
    if motor == nil and isFunc(vehicle.getMotor) then
        local ok, value = pcall(vehicle.getMotor, vehicle)
        motor = ok and value or nil
    end
    if type(motor) ~= "table" then
        return nil, nil, nil
    end

    local low, high, source = nil, nil, nil
    if isFunc(motor.getMinRpm) and isFunc(motor.getMaxRpm) then
        local okLow, valueLow = pcall(motor.getMinRpm, motor)
        local okHigh, valueHigh = pcall(motor.getMaxRpm, motor)
        if okLow and okHigh and isFinite(valueLow) and isFinite(valueHigh) then
            low, high, source = valueLow, valueHigh, "motor:getMinRpm/getMaxRpm"
        end
    end
    if low == nil and isFinite(motor.minRpm) and isFinite(motor.maxRpm) then
        low, high, source = motor.minRpm, motor.maxRpm, "motor.minRpm/maxRpm"
    end
    -- both or neither: half a range is of no use to the reader
    if low == nil or low < 0 or high <= low or high > M.RPM_RANGE_LIMIT then
        return nil, nil, nil
    end
    return low, high, string.format("%s (%s..%s)", source, tostring(M.fmtInt(low)), tostring(M.fmtInt(high)))
end

--- Is the engine being started right now: from the start being triggered
-- until the engine runs?
-- FS25 keeps a motor state per vehicle: OFF, IGNITION (electrics on, engine
-- not turning), STARTING (Motorized.startMotor sets it; onUpdate moves on to
-- ON once motorStartTime has passed) and ON. STARTING is that span exactly;
-- the game's own dashboards use the same test for "only while starting".
-- @return true/false, or nil when the vehicle offers no way to tell; source
function M.getStarting(vehicle)
    local spec = vehicle.spec_motorized
    if type(spec) ~= "table" then
        return nil, nil
    end

    local states = MotorState
    if type(states) == "table" or type(states) == "userdata" then
        local okEnum, starting = pcall(function()
            return states.STARTING
        end)
        if okEnum and starting ~= nil then
            if isFunc(vehicle.getMotorState) then
                local ok, state = pcall(vehicle.getMotorState, vehicle)
                if ok and state ~= nil then
                    return state == starting, "getMotorState"
                end
            end
            if spec.motorState ~= nil then
                return spec.motorState == starting, "spec_motorized.motorState"
            end
        end
    end

    -- FS22 had one flag, true from the moment the start was triggered, and
    -- counted the engine as running once motorStartTime had passed.
    local mission = g_currentMission
    local now = type(mission) == "table" and mission.time or nil
    if type(spec.isMotorStarted) == "boolean" and isFinite(spec.motorStartTime) and isFinite(now) then
        return spec.isMotorStarted and spec.motorStartTime > now, "isMotorStarted+motorStartTime"
    end
    return nil, nil
end

-- What a motor may consume without that making it a combustion engine.
M.ELECTRIC_CONSUMER = "ELECTRICCHARGE"
M.NON_FUEL_CONSUMERS = { AIR = true } -- compressed air for the brakes

--- Fill type names of everything the vehicle's motor consumes, upper case
-- and sorted. @return list (possibly empty), source; or nil when unknown
--- How long this machine takes to start, in ms: the time the game keeps it
-- in STARTING, which it takes from the length of the machine's start sound.
-- @return duration (nil when the game does not say), source
function M.getStartDuration(vehicle)
    local spec = vehicle.spec_motorized
    if type(spec) ~= "table" then
        return nil, nil
    end
    local duration = spec.motorStartDuration
    if isFinite(duration) and duration > 0 and duration <= 60000 then
        return duration, "spec_motorized.motorStartDuration"
    end
    return nil, "not given"
end

function M.getConsumerNames(vehicle)
    local spec = vehicle.spec_motorized
    if type(spec) ~= "table" then
        return nil, nil
    end

    local names = {}
    local byName = spec.consumersByFillTypeName
    if type(byName) == "table" then
        for name in pairs(byName) do
            if type(name) == "string" then
                names[#names + 1] = string.upper(name)
            end
        end
        table.sort(names)
        return names, "consumersByFillTypeName"
    end

    local list = spec.consumers
    local manager = g_fillTypeManager
    if type(list) == "table" and manager ~= nil and isFunc(manager.getFillTypeNameByIndex) then
        for _, consumer in pairs(list) do
            if type(consumer) == "table" and consumer.fillType ~= nil then
                local name = manager:getFillTypeNameByIndex(consumer.fillType)
                if type(name) ~= "string" then
                    return nil, nil -- a consumer that cannot be named: do not guess
                end
                names[#names + 1] = string.upper(name)
            end
        end
        table.sort(names)
        return names, "consumers+getFillTypeNameByIndex"
    end
    return nil, nil
end

--- Is the vehicle driven by an electric motor, with no combustion engine?
-- The game has no flag for this; what tells the drives apart is what the
-- motor consumes. Electric: it consumes ELECTRICCHARGE and nothing else that
-- could be a fuel. Diesel, methane, DEF or a fuel this script does not know
-- next to the charge (a hybrid, a mod's own fuel) all mean "not electric",
-- so that a wrong guess errs towards an engine being there.
-- @return true/false, or nil when what the motor consumes is unknown; source
function M.isElectric(vehicle)
    local names, source = M.getConsumerNames(vehicle)
    if names == nil then
        return nil, nil
    end
    local hasCharge, hasOther = false, false
    for i = 1, #names do
        if names[i] == M.ELECTRIC_CONSUMER then
            hasCharge = true
        elseif not M.NON_FUEL_CONSUMERS[names[i]] then
            hasOther = true
        end
    end
    local listed = (#names > 0) and table.concat(names, ",") or "no consumers"
    return hasCharge and not hasOther, string.format("%s (%s)", tostring(source), listed)
end

-- What is worked out once per driven vehicle and kept while it is driven:
-- whether it is electric, and which wheels are watched for bumps (with their
-- running state). Driving another vehicle, or none, starts afresh.
local driveCache = { vehicle = nil }

function M.resetVehicleCache()
    driveCache = { vehicle = nil }
end

local function cacheFor(vehicle)
    if driveCache.vehicle ~= vehicle then
        driveCache = { vehicle = vehicle }
    end
    return driveCache
end

--- isElectric, remembered per driven vehicle.
function M.getElectric(vehicle)
    local cache = cacheFor(vehicle)
    if cache.electric ~= nil then
        return cache.electric, cache.electricSource
    end
    local electric, source = M.isElectric(vehicle)
    if type(electric) == "boolean" then
        cache.electric, cache.electricSource = electric, source
    end
    return electric, source
end

--- getRpmRange, remembered per driven vehicle.
function M.getRpmRangeCached(vehicle)
    local cache = cacheFor(vehicle)
    if cache.rpmMin ~= nil then
        return cache.rpmMin, cache.rpmMax, cache.rpmSource
    end
    local low, high, source = M.getRpmRange(vehicle)
    if low ~= nil then
        cache.rpmMin, cache.rpmMax, cache.rpmSource = low, high, source
    end
    return low, high, source
end

---------------------------------------------------------------------------
-- bumps
---------------------------------------------------------------------------

--- Where a wheel sits in the vehicle: x (positive = left), z (positive = front).
local function wheelPosition(vehicle, wheel, physics)
    local root = vehicle.rootNode
    local node = wheel.repr or wheel.driveNode
    if isFunc(localToLocal) and root ~= nil and node ~= nil then
        local ok, x, _, z = pcall(localToLocal, node, root, 0, 0, 0)
        if ok and isFinite(x) and isFinite(z) then
            return x, z
        end
    end
    if isFinite(physics.positionX) and isFinite(physics.positionZ) then
        return physics.positionX, physics.positionZ
    end
    return nil, nil
end

--- Does the steering turn this wheel? (Wheels that only follow, like those
-- of a steering-axle trailer, do not count.)
local function isSteeredWheel(physics)
    if isFinite(physics.rotMax) and physics.rotMax ~= 0 then
        return true
    end
    if isFinite(physics.rotMin) and physics.rotMin ~= 0 then
        return true
    end
    return isFinite(physics.rotSpeed) and physics.rotSpeed ~= 0
end

--- The wheels to watch for bumps: the steered wheels of the front-most
-- steered axle, each on its side. A vehicle without steered wheels (it bends
-- in the middle, or runs on tracks) gives the wheels of its front-most axle.
-- @return list of { wheel, physics, left, right }, description; or nil
function M.findBumpWheels(vehicle)
    local wheelSpec = vehicle.spec_wheels
    local wheels = type(wheelSpec) == "table" and wheelSpec.wheels or nil
    if type(wheels) ~= "table" then
        return nil, nil
    end

    local all, steered = {}, {}
    for _, wheel in pairs(wheels) do
        local physics = type(wheel) == "table" and wheel.physics or nil
        if type(physics) == "table" then
            local entry = { wheel = wheel, physics = physics }
            entry.x, entry.z = wheelPosition(vehicle, wheel, physics)
            all[#all + 1] = entry
            if isSteeredWheel(physics) then
                steered[#steered + 1] = entry
            end
        end
    end

    local pool, rule, anySteered = steered, "steered wheels", true
    if #pool == 0 then
        pool, rule, anySteered = all, "front axle (no steered wheels)", false
    end

    -- keep the front-most axle of the pool
    local frontZ = nil
    for i = 1, #pool do
        local z = pool[i].z
        if z ~= nil and (frontZ == nil or z > frontZ) then
            frontZ = z
        end
    end
    local chosen = {}
    for i = 1, #pool do
        local entry = pool[i]
        if frontZ == nil or (entry.z ~= nil and entry.z >= frontZ - M.BUMP_AXLE_DEPTH) then
            if entry.x ~= nil then
                entry.left = entry.x > -M.BUMP_CENTRE_WIDTH
                entry.right = entry.x < M.BUMP_CENTRE_WIDTH
            elseif type(entry.wheel.isLeft) == "boolean" then
                entry.left, entry.right = entry.wheel.isLeft, not entry.wheel.isLeft
            end
            if entry.left or entry.right then
                chosen[#chosen + 1] = entry
            end
        end
    end
    if frontZ == nil and not anySteered then
        return nil, nil -- no steered wheels and no positions: "front" cannot be told
    end
    if #chosen == 0 then
        return nil, nil
    end

    local left, right = 0, 0
    for i = 1, #chosen do
        if chosen[i].left then left = left + 1 end
        if chosen[i].right then right = right + 1 end
    end
    return chosen, string.format("%s (%d left, %d right)", rule, left, right)
end

--- The wheel's tire load, in any unit as long as it is always the same one.
-- @return load (0 when off the ground), or nil when it cannot be read
local function wheelLoad(entry)
    local physics = entry.physics
    if isFunc(physics.getTireLoad) then
        local ok, load = pcall(physics.getTireLoad, physics)
        if ok and isFinite(load) then
            return math.abs(load)
        end
    end
    if isFunc(getWheelShapeContactForce) and entry.wheel.node ~= nil and physics.wheelShape ~= nil
        and physics.wheelShapeCreated ~= false then
        local ok, force = pcall(getWheelShapeContactForce, entry.wheel.node, physics.wheelShape)
        if ok then
            if force == nil then
                return 0 -- no contact
            elseif isFinite(force) then
                return math.abs(force)
            end
        end
    end
    return nil
end

local function wheelSuspension(entry)
    local netInfo = entry.physics.netInfo
    if type(netInfo) == "table" and isFinite(netInfo.suspensionLength) then
        return netInfo.suspensionLength
    end
    return nil
end

--- Build (once per driven vehicle) what updateBumps works on.
-- @return state table, or false when this vehicle gives nothing to go on
local function bumpStateFor(vehicle)
    local cache = cacheFor(vehicle)
    if cache.bump ~= nil then
        return cache.bump
    end
    cache.bump = false

    -- The physics values exist where the simulation runs: single player and
    -- the host. A multiplayer client only gets a coarse, delayed copy of the
    -- suspension and no tire load at all, so there the keys are left out.
    if vehicle.isServer == false then
        cache.bumpSource = "none (multiplayer client: wheel physics is not local)"
        return false
    end

    local entries, description = M.findBumpWheels(vehicle)
    if entries == nil then
        cache.bumpSource = "none (no usable wheels)"
        return false
    end

    local signal = nil
    for i = 1, #entries do
        if wheelLoad(entries[i]) == nil then
            signal = nil
            break
        end
        signal = "load"
    end
    if signal == nil then
        for i = 1, #entries do
            if wheelSuspension(entries[i]) == nil then
                signal = nil
                break
            end
            signal = "suspension"
        end
    end
    if signal == nil then
        cache.bumpSource = "none (neither tire load nor suspension length can be read)"
        return false
    end

    local hasLeft, hasRight = false, false
    for i = 1, #entries do
        hasLeft = hasLeft or entries[i].left == true
        hasRight = hasRight or entries[i].right == true
    end
    cache.bump = {
        entries = entries,
        signal = signal,
        hasLeft = hasLeft,
        hasRight = hasRight,
        left = { peak = 0, age = 0, value = 0 },
        right = { peak = 0, age = 0, value = 0 },
    }
    cache.bumpSource = string.format("%s of the %s",
        signal == "load" and "tire load against its running average" or "suspension speed", description)
    return cache.bump
end

--- How hard one wheel is being jolted this frame.
-- @return jolt 0..1, the raw measurement behind it (load ratio, or m/s)
local function wheelJolt(state, entry, dt)
    if state.signal == "load" then
        local load = wheelLoad(entry)
        if load == nil or load <= 0 then
            return 0, 0 -- unreadable this frame, or off the ground
        end
        entry.warmMs = (entry.warmMs or 0) + dt
        if entry.average == nil then
            entry.average, entry.highest = load, load
            return 0, 1
        end
        -- a wheel carrying next to nothing must not turn a touch into a hit
        local reference = math.max(entry.average, 0.3 * entry.highest, 1e-9)
        local ratio = load / reference
        -- the average follows slowly, and a spike does not drag it along
        local weight = dt / (M.BUMP_BASELINE_MS + dt)
        entry.average = entry.average + (math.min(load, reference * 1.5) - entry.average) * weight
        if entry.average > entry.highest then
            entry.highest = entry.average
        end
        if entry.warmMs < M.BUMP_WARMUP_MS then
            return 0, ratio
        end
        return clamp((ratio - M.BUMP_LOAD_START) / (M.BUMP_LOAD_FULL - M.BUMP_LOAD_START), 0, 1), ratio
    end

    local length = wheelSuspension(entry)
    if length == nil then
        return 0, 0
    end
    local last = entry.lastLength
    entry.lastLength = length
    if last == nil or dt <= 0 then
        return 0, 0
    end
    local speed = math.abs(length - last) / (dt / 1000)
    return clamp((speed - M.BUMP_SUSP_START) / (M.BUMP_SUSP_FULL - M.BUMP_SUSP_START), 0, 1), speed
end

local function holdPeak(side, jolt, dt)
    side.age = side.age + dt
    local held = side.peak * math.max(0, 1 - side.age / M.BUMP_HOLD_MS)
    if jolt > 0 and jolt >= held then
        side.peak, side.age, held = jolt, 0, jolt
    elseif held <= 0 then
        side.peak, held = 0, 0
    end
    side.value = held
end

-- One-time calibration notes for the game log (per savegame load).
local bumpNotes = nil

function M.resetBumpNotes()
    bumpNotes = {
        strong = false,
        field = { ms = 0, left = 0, right = 0, done = false },
        other = { ms = 0, left = 0, right = 0, done = false },
    }
end
M.resetBumpNotes()

local function bumpScale(state)
    if state.signal == "load" then
        return string.format("tire load as a multiple of its running average; a bump counts from %s, full at %s",
            M.fmtFixed(M.BUMP_LOAD_START, 2), M.fmtFixed(M.BUMP_LOAD_FULL, 2))
    end
    return string.format("suspension speed in m/s; a bump counts from %s, full at %s",
        M.fmtFixed(M.BUMP_SUSP_START, 2), M.fmtFixed(M.BUMP_SUSP_FULL, 2))
end

--- Advance the bump detection by one frame. Call every frame while driving.
-- @param dt      frame time in ms (real time)
-- @param onField whether the vehicle is on a field (only for the notes)
-- @return a line for the game log on the few occasions there is one, else nil
function M.updateBumps(vehicle, dt, onField)
    if type(vehicle) ~= "table" then
        return nil
    end
    local state = bumpStateFor(vehicle)
    if not state then
        return nil
    end
    if not isFinite(dt) or dt < 0 then
        dt = 0
    end

    local speed = M.getSpeed(vehicle)
    local absSpeed = isFinite(speed) and math.abs(speed) or 0
    local moving = absSpeed >= M.BUMP_MIN_SPEED
    local hitch = dt > M.BUMP_GAP_MS

    local joltLeft, joltRight, rawLeft, rawRight = 0, 0, 0, 0
    for i = 1, #state.entries do
        local entry = state.entries[i]
        local jolt, raw = wheelJolt(state, entry, hitch and 0 or dt)
        if entry.left then
            joltLeft, rawLeft = math.max(joltLeft, jolt), math.max(rawLeft, raw)
        end
        if entry.right then
            joltRight, rawRight = math.max(joltRight, jolt), math.max(rawRight, raw)
        end
    end
    if not moving or hitch then
        -- standing still the chassis rocks without anything being hit, and
        -- after a hitch the change since the last frame says nothing
        joltLeft, joltRight = 0, 0
    end
    holdPeak(state.left, joltLeft, dt)
    holdPeak(state.right, joltRight, dt)

    -- notes for the first run in the game
    if not moving or hitch then
        return nil
    end
    local notes = bumpNotes
    if not notes.strong and math.max(joltLeft, joltRight) >= M.BUMP_NOTE_STRONG then
        notes.strong = true
        local left = joltLeft >= joltRight
        return string.format("first strong bump: %s wheel %s at %s m/s, measured %s (%s)",
            left and "left" or "right", M.fmtFixed(left and joltLeft or joltRight, 2), M.fmtFixed(absSpeed, 1),
            M.fmtFixed(left and rawLeft or rawRight, 2), bumpScale(state))
    end
    local stats = (onField == true) and notes.field or notes.other
    if not stats.done then
        stats.ms = stats.ms + dt
        stats.left, stats.right = math.max(stats.left, rawLeft), math.max(stats.right, rawRight)
        if stats.ms >= M.BUMP_NOTE_AFTER_MS then
            stats.done = true
            return string.format("bump check after %d s of driving %s: highest left %s, right %s (%s)",
                math.floor(M.BUMP_NOTE_AFTER_MS / 1000 + 0.5), (onField == true) and "on a field" or "off the fields",
                M.fmtFixed(stats.left, 2), M.fmtFixed(stats.right, 2), bumpScale(state))
        end
    end
    return nil
end

--- The held bump values, left and right from the driver's seat.
-- @return bumpl, bumpr (each nil when there is nothing to measure it with), source
function M.getBumps(vehicle)
    local state = bumpStateFor(vehicle)
    local source = cacheFor(vehicle).bumpSource
    if not state then
        return nil, nil, source
    end
    local left = state.hasLeft and state.left.value or nil
    local right = state.hasRight and state.right.value or nil
    -- with the driving position turned round, the driver's left is the vehicle's right
    local reverse = vehicle.spec_reverseDriving
    if type(reverse) == "table" and reverse.isReverseDriving == true then
        left, right = right, left
    end
    return left, right, source
end

---------------------------------------------------------------------------
-- body motion
---------------------------------------------------------------------------

-- How the machine itself is moving under the driver: rocking from side to
-- side, pitching nose up and down, and bouncing. Read from where the
-- vehicle's root node is and which way it faces, frame by frame, which the
-- game has on every machine, clients in multiplayer included.
M.MOTION_SMOOTH_MS = 70     -- low-pass on the three values; one frame's jitter is not motion
M.MOTION_MAX_GAP_MS = 250   -- a longer frame than this is a hitch: start afresh
M.MOTION_MAX_JUMP_M = 3     -- further than this in one frame is a reset or a teleport

local function motionStateFor(vehicle)
    local cache = cacheFor(vehicle)
    if cache.motion == nil then
        cache.motion = { have = false, haveSpeed = false, rollRate = 0, pitchRate = 0, heave = 0 }
    end
    return cache.motion
end

local function forgetMotion(state, source)
    state.have, state.haveSpeed = false, false
    state.rollRate, state.pitchRate, state.heave = 0, 0, 0
    state.source = source
end

--- Follow the vehicle's body through one frame (dt in ms).
function M.updateMotion(vehicle, dt)
    local state = motionStateFor(vehicle)
    local root = vehicle.rootNode
    if root == nil or not isFunc(localDirectionToWorld) or not isFunc(getWorldTranslation) then
        forgetMotion(state, "none (the vehicle's place in the world cannot be read)")
        return
    end
    -- In the engine a vehicle's own +x points to its left and +z forward.
    local okLeft, _, leftY = pcall(localDirectionToWorld, root, 1, 0, 0)
    local okFront, _, frontY = pcall(localDirectionToWorld, root, 0, 0, 1)
    local okPlace, _, height = pcall(getWorldTranslation, root)
    if not (okLeft and okFront and okPlace and isFinite(leftY) and isFinite(frontY) and isFinite(height)) then
        forgetMotion(state, "none (the vehicle's place in the world cannot be read)")
        return
    end
    state.source = "root node, frame by frame"

    -- Leaning right (left side up) is positive roll; nose up is positive pitch.
    local roll = math.asin(clamp(leftY, -1, 1))
    local pitch = math.asin(clamp(frontY, -1, 1))
    local usable = state.have and isFinite(dt) and dt > 0 and dt <= M.MOTION_MAX_GAP_MS
        and math.abs(height - state.height) <= M.MOTION_MAX_JUMP_M
    if not usable then
        state.rollRate, state.pitchRate, state.heave = 0, 0, 0
        state.haveSpeed = false
    else
        local seconds = dt / 1000
        local blend = 1 - math.exp(-dt / M.MOTION_SMOOTH_MS)
        local rise = (height - state.height) / seconds
        state.rollRate = state.rollRate + ((roll - state.roll) / seconds - state.rollRate) * blend
        state.pitchRate = state.pitchRate + ((pitch - state.pitch) / seconds - state.pitchRate) * blend
        if state.haveSpeed then
            state.heave = state.heave + ((rise - state.rise) / seconds - state.heave) * blend
        end
        state.rise, state.haveSpeed = rise, true
    end
    state.roll, state.pitch, state.height, state.have = roll, pitch, height, true
end

--- How the body is moving right now, from the driver's seat.
-- @return rollrate (rad/s, leaning to the right is positive), pitchrate
--         (rad/s, nose rising is positive), heave (m/s^2, upward is positive),
--         source; the three are nil when there is nothing to read them from
function M.getMotion(vehicle)
    local state = motionStateFor(vehicle)
    if not state.have then
        return nil, nil, nil, state.source or "none (not followed yet)"
    end
    local roll, pitch = state.rollRate, state.pitchRate
    -- with the driving position turned round, left and right and front and back swap
    local reverse = vehicle.spec_reverseDriving
    if type(reverse) == "table" and reverse.isReverseDriving == true then
        roll, pitch = -roll, -pitch
    end
    return roll, pitch, state.heave, state.source
end

--- The wheels the engine drives, from the differentials.
function M.getDrivenWheels(vehicle)
    local wheelSpec = vehicle.spec_wheels
    local wheels = type(wheelSpec) == "table" and wheelSpec.wheels or nil
    if type(wheels) ~= "table" then
        return nil, nil
    end

    local driven, seen = {}, {}
    local motorized = vehicle.spec_motorized
    if type(motorized) == "table" and type(motorized.differentials) == "table" then
        for _, diff in pairs(motorized.differentials) do
            if type(diff) == "table" then
                for n = 1, 2 do
                    if diff["diffIndex" .. n .. "IsWheel"] == true then
                        local wheel = wheels[diff["diffIndex" .. n]]
                        if type(wheel) == "table" and not seen[wheel] then
                            seen[wheel] = true
                            driven[#driven + 1] = wheel
                        end
                    end
                end
            end
        end
    end
    if #driven > 0 then
        return driven, "differentials"
    end

    for _, wheel in pairs(wheels) do
        if type(wheel) == "table" then
            driven[#driven + 1] = wheel
        end
    end
    return driven, "all wheels"
end

--- Largest longitudinal slip among the driven wheels, 0..1.
function M.getSlip(vehicle, speed)
    local driven, drivenSource = M.getDrivenWheels(vehicle)
    if driven == nil or #driven == 0 then
        return nil, nil
    end

    local groundSpeed = isFinite(speed) and math.abs(speed) or 0
    -- The physics wheel shapes only exist where the simulation runs.
    local usePhysics = vehicle.isServer ~= false and isFunc(getWheelShapeSlip)

    local best, source = nil, nil
    for i = 1, #driven do
        local wheel = driven[i]
        local physics = wheel.physics
        if type(physics) ~= "table" then
            physics = wheel -- FS22 kept these fields on the wheel itself
        end

        if physics.hasGroundContact ~= false then
            local netInfo = physics.netInfo
            local wheelSpeed = nil -- circumferential speed in m/s
            if type(netInfo) == "table" and isFinite(netInfo.xDriveSpeed) and isFinite(physics.radius) then
                wheelSpeed = math.abs(netInfo.xDriveSpeed) * physics.radius
            end

            local slip, how = nil, nil
            if usePhysics and wheel.node ~= nil and physics.wheelShape ~= nil and physics.wheelShapeCreated ~= false then
                -- The physics engine's own slip ratio; 1 is full slip.
                local ok, longSlip = pcall(getWheelShapeSlip, wheel.node, physics.wheelShape)
                if ok and isFinite(longSlip) then
                    slip, how = math.min(math.abs(longSlip), 1), "getWheelShapeSlip"
                    -- standing still with the wheel at rest: the ratio is noise
                    if wheelSpeed ~= nil and wheelSpeed < 0.25 and groundSpeed < 0.25 then
                        slip = 0
                    end
                end
            end
            if slip == nil and wheelSpeed ~= nil then
                -- wheel speed against ground speed; the 1 m/s floor keeps
                -- crawling speeds from reading as slip
                slip = math.abs(wheelSpeed - groundSpeed) / math.max(wheelSpeed, groundSpeed, 1)
                how = "xDriveSpeed*radius vs speed"
            end
            if slip == nil and type(netInfo) == "table" and isFinite(netInfo.slip) then
                slip, how = netInfo.slip, "netInfo.slip"
            end

            if slip ~= nil and (best == nil or slip > best) then
                best, source = slip, how
            end
        elseif best == nil then
            best, source = 0, "no ground contact"
        end
    end

    if best == nil then
        return nil, nil
    end
    return clamp(best, 0, 1), string.format("%s (%d wheels, %s)", tostring(source), #driven, tostring(drivenSource))
end

local fieldRoughCacheFor, fieldRoughCache = nil, nil

local fieldSurfaceCache = nil

--- @return roughness by field ground state, kind of surface by field ground state
local function fieldRoughTable()
    local types = FieldGroundType
    if type(types) ~= "table" and type(types) ~= "userdata" then
        return nil, nil
    end
    if fieldRoughCacheFor ~= types then
        local cache, kinds = {}, {}
        for name, rough in pairs(M.FIELD_ROUGH_BY_NAME) do
            local ok, value = pcall(function() return types[name] end)
            if ok and value ~= nil then
                cache[value] = rough
            end
        end
        for name, kind in pairs(M.FIELD_SURFACE_BY_NAME) do
            local ok, value = pcall(function() return types[name] end)
            if ok and value ~= nil then
                kinds[value] = kind
            end
        end
        fieldRoughCacheFor, fieldRoughCache, fieldSurfaceCache = types, cache, kinds
    end
    return fieldRoughCache, fieldSurfaceCache
end

--- Roughness and kind of surface under one wheel.
-- @param physics the wheel's WheelPhysics object (wheel.physics in FS25)
-- @return rough 0..1 or nil when the wheel is in the air / unknown, isField,
--         kind (a M.SURFACE value, nil along with rough)
function M.classifyWheel(physics)
    if type(physics) ~= "table" then
        return nil, false
    end

    local contactTypes = WheelContactType
    local contact = physics.contact
    if contactTypes ~= nil and contact ~= nil then
        if contact == contactTypes.NONE then
            return nil, false
        elseif contact == contactTypes.OBJECT then
            -- on a road mesh, bridge, yard slab ... the game plays "asphalt" here
            return M.ROUGH.ROAD, false, M.SURFACE.ROAD
        elseif contact == contactTypes.GROUND_HEIGHT then
            if physics.hasSnowContact == true then
                return M.ROUGH.SNOW, false, M.SURFACE.SNOW
            end
            return M.ROUGH.HEAP, false, M.SURFACE.HEAP
        end
        -- otherwise WheelContactType.GROUND: terrain, classified below
    elseif physics.hasGroundContact == false then
        return nil, false
    end

    local groundTypes = FieldGroundType
    local densityType = physics.densityType
    if groundTypes ~= nil and densityType ~= nil and densityType ~= groundTypes.NONE then
        if densityType == groundTypes.GRASS or densityType == groundTypes.GRASS_CUT then
            return M.ROUGH.GRASS, false, M.SURFACE.GRASS
        end
        local known, kinds = fieldRoughTable()
        return (known ~= nil and known[densityType]) or M.ROUGH.FIELD, true,
            (kinds ~= nil and kinds[densityType]) or M.SURFACE.FIELD
    end

    -- Plain terrain: use the layer's "depth" attribute with the game's own
    -- road / hard / soft thresholds.
    local depth = physics.groundDepth
    if isFinite(depth) then
        if depth < M.DEPTH_ROAD then
            return M.ROUGH.ROAD, false, M.SURFACE.ROAD
        elseif depth > M.DEPTH_SOFT then
            return M.ROUGH.GRASS, false, M.SURFACE.GRASS
        end
        local span = M.DEPTH_SOFT - M.DEPTH_ROAD
        return M.ROUGH.DIRT_ROAD + (depth - M.DEPTH_ROAD) / span * 0.15, false, M.SURFACE.GRAVEL
    end
    return nil, false, nil
end

--- Surface under the vehicle: rough 0..1, field 0/1, and the kind of surface
-- most of the wheels are on (a M.SURFACE value, or nil when unknown).
-- @return rough, field, source, kind
function M.getSurface(vehicle)
    local wheelSpec = vehicle.spec_wheels
    local wheels = type(wheelSpec) == "table" and wheelSpec.wheels or nil

    local sum, count, onField = 0, 0, 0
    local wheelsOn, roughOn = {}, {}
    if type(wheels) == "table" then
        for _, wheel in pairs(wheels) do
            if type(wheel) == "table" then
                local rough, isField, kind = M.classifyWheel(wheel.physics)
                if rough ~= nil then
                    sum = sum + rough
                    count = count + 1
                    if isField then
                        onField = onField + 1
                    end
                    if kind ~= nil then
                        wheelsOn[kind] = (wheelsOn[kind] or 0) + 1
                        roughOn[kind] = (roughOn[kind] or 0) + rough
                    end
                end
            end
        end
    end
    if count > 0 then
        -- The kind most wheels stand on; when it is even, the rougher one, which is the one felt.
        -- Walked in number order so that the answer does not depend on table order.
        local best = nil
        for kind = 1, M.SURFACE_COUNT do
            local n = wheelsOn[kind]
            if n ~= nil and (best == nil or n > wheelsOn[best]
                or (n == wheelsOn[best] and roughOn[kind] >= roughOn[best])) then
                best = kind
            end
        end
        return sum / count, (onField * 2 >= count), string.format("wheel.physics (%d wheels in contact)", count), best
    end

    -- No usable wheel data: all we can ask is whether the vehicle is on a field.
    if isFunc(vehicle.getIsOnField) then
        local isOnField = vehicle:getIsOnField()
        if isOnField == true then
            return M.ROUGH.FIELD, true, "getIsOnField", M.SURFACE.FIELD
        elseif isOnField == false then
            return nil, false, "getIsOnField", nil
        end
    end
    return nil, nil, nil, nil
end

local function hasOwnLoweredState(object)
    if object.spec_pickup ~= nil then
        return true
    end
    local foldable = object.spec_foldable
    return type(foldable) == "table" and foldable.foldMiddleAnimTime ~= nil
end

--- Is this object lowered?
-- @param attacher the vehicle it is attached to, nil for the driven vehicle
function M.isLowered(object, attacher)
    if not isFunc(object.getIsLowered) then
        return false
    end
    local own = hasOwnLoweredState(object)
    if attacher == nil and not own then
        -- the driven vehicle itself: only machines with a pickup or a
        -- fold-middle position (self-propelled mowers) have a lowered state
        return false
    end
    if object:getIsLowered(false) ~= true then
        return false
    end
    if own then
        return true
    end
    -- Attachable.getIsLowered reports "moveDown" of the attacher joint. Joints
    -- that cannot be lowered but count as always-down (front loader tool
    -- carriers) would read as lowered forever, so ask the joint.
    if attacher ~= nil and isFunc(attacher.getAttacherJointDescFromObject) then
        local ok, jointDesc = pcall(attacher.getAttacherJointDescFromObject, attacher, object)
        if ok and type(jointDesc) == "table" and jointDesc.allowsLowering == false then
            return false
        end
    end
    return true
end

--- Is this object working the ground right now?
-- Either one of its work areas processed ground within the last 200 ms (the
-- game's own getIsWorkAreaProcessing), or a work area that needs ground
-- contact has it while the rig is moving (a tool dragged through soil that is
-- already worked changes nothing, so the first test alone would miss it).
function M.isWorking(object, absSpeed)
    local spec = object.spec_workArea
    local areas = type(spec) == "table" and spec.workAreas or nil
    if type(areas) ~= "table" then
        return false
    end

    local auxiliary = (type(WorkAreaType) == "table" or type(WorkAreaType) == "userdata") and WorkAreaType.AUXILIARY or nil
    local mission = g_currentMission
    local now = type(mission) == "table" and mission.time or nil

    for _, area in pairs(areas) do
        if type(area) == "table" and (auxiliary == nil or area.type ~= auxiliary) then
            if isFunc(object.getIsWorkAreaProcessing) then
                local ok, processing = pcall(object.getIsWorkAreaProcessing, object, area)
                if ok and processing == true and (area.lastProcessingTime == nil or area.lastProcessingTime > 0) then
                    return true
                end
            elseif isFinite(area.lastProcessingTime) and area.lastProcessingTime > 0 and isFinite(now) then
                if area.lastProcessingTime + 200 >= now then
                    return true
                end
            end

            if absSpeed > 0.5 and area.requiresGroundContact == true and area.groundReferenceNode ~= nil
                and isFunc(object.getIsWorkAreaActive) then
                local ok, isActive = pcall(object.getIsWorkAreaActive, object, area)
                if ok and isActive == true then
                    return true
                end
            end
        end
    end
    return false
end

--- Walk the driven vehicle and everything attached to it.
-- @return lowered (bool), working (bool), number of attached implements
function M.getImplementState(vehicle, speed)
    local absSpeed = isFinite(speed) and math.abs(speed) or 0
    local lowered, working, count = false, false, 0
    local seen = {}

    local function visit(object, attacher, depth)
        if type(object) ~= "table" or seen[object] or depth > MAX_IMPLEMENT_DEPTH then
            return
        end
        seen[object] = true
        if attacher ~= nil then
            count = count + 1
        end

        if not lowered then
            local ok, result = pcall(M.isLowered, object, attacher)
            lowered = ok and result == true
        end
        if not working then
            local ok, result = pcall(M.isWorking, object, absSpeed)
            working = ok and result == true
        end

        local ok, implements = pcall(attachedImplements, object)
        if ok and type(implements) == "table" then
            for _, implement in pairs(implements) do
                if type(implement) == "table" then
                    visit(implement.object, object, depth + 1)
                end
            end
        end
    end

    visit(vehicle, nil, 0)
    return lowered, working, count
end

---------------------------------------------------------------------------
-- the whole sample
---------------------------------------------------------------------------

--- Gather every contract value for the driven vehicle.
-- Never raises. @return sample table for formatLine, info table with
-- .sources (key -> API used) and .errors (key -> message) for one-time logging
function M.gather(vehicle)
    local sample = { active = false }
    local info = { sources = {}, errors = {} }
    if type(vehicle) ~= "table" then
        M.resetVehicleCache() -- nobody is driving: nothing to remember
        return sample, info
    end
    sample.active = true

    local function try(key, fn)
        local ok, err = pcall(fn)
        if not ok then
            info.errors[key] = tostring(err)
        end
    end

    try("speed", function()
        local speed, source = M.getSpeed(vehicle)
        if isFinite(speed) then
            if math.abs(speed) < 0.005 then
                speed = 0
            end
            sample.speed = speed
            info.sources.speed = source
        end
    end)

    try("steer", function()
        local steer, source = M.getSteer(vehicle)
        if isFinite(steer) then
            sample.steer = steer
            info.sources.steer = source
        end
    end)

    try("mass", function()
        local mass, source = M.getMassKg(vehicle)
        if isFinite(mass) then
            sample.mass = mass
            info.sources.mass = source
        end
    end)

    try("engine", function()
        local rpm, load, source = M.getEngine(vehicle)
        if isFinite(rpm) then sample.rpm = rpm end
        if isFinite(load) then sample.load = load end
        info.sources.engine = source
    end)

    try("rpmrange", function()
        local low, high, source = M.getRpmRangeCached(vehicle)
        if isFinite(low) and isFinite(high) then
            sample.rpmmin, sample.rpmmax = low, high
        end
        info.sources.rpmrange = source
    end)

    local electric = nil
    try("electric", function()
        local value, source = M.getElectric(vehicle)
        if type(value) == "boolean" then
            electric = value
            sample.electric = value
        end
        info.sources.electric = source
    end)

    try("starting", function()
        if electric == true then
            -- by contract never 1 for an electric vehicle, whatever its motor state
            sample.starting = false
            info.sources.starting = "electric: never"
            return
        end
        local value, source = M.getStarting(vehicle)
        if type(value) == "boolean" then
            sample.starting = value
        end
        info.sources.starting = source
    end)

    try("slip", function()
        local slip, source = M.getSlip(vehicle, sample.speed)
        if isFinite(slip) then
            sample.slip = slip
            info.sources.slip = source
        end
    end)

    try("surface", function()
        local rough, field, source, kind = M.getSurface(vehicle)
        if isFinite(rough) then sample.rough = rough end
        if isFinite(kind) then sample.surface = kind end
        if type(field) == "boolean" then sample.field = field end
        info.sources.surface = source
    end)

    try("implements", function()
        local lowered, working, count = M.getImplementState(vehicle, sample.speed)
        sample.lowered = lowered
        sample.working = working
        info.sources.implements = string.format("%d attached", count)
    end)

    try("bumps", function()
        local left, right, source = M.getBumps(vehicle)
        if isFinite(left) then sample.bumpl = left end
        if isFinite(right) then sample.bumpr = right end
        info.sources.bumps = source
    end)

    try("startduration", function()
        local duration, source = M.getStartDuration(vehicle)
        if isFinite(duration) then sample.startms = duration end
        info.sources.startduration = source
    end)

    try("motion", function()
        local roll, pitch, heave, source = M.getMotion(vehicle)
        if isFinite(roll) then sample.rollrate = roll end
        if isFinite(pitch) then sample.pitchrate = pitch end
        if isFinite(heave) then sample.heave = heave end
        info.sources.motion = source
    end)

    return sample, info
end
