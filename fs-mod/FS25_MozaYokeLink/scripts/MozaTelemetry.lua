--
-- MozaTelemetry.lua  --  pure telemetry gathering and line formatting.
--
-- Part of FS25_MozaYokeLink. Nothing in this file touches files, timers or mod
-- event hooks, so it also runs under plain Lua with mocked game objects (see
-- fs-mod/tests). The engine-facing half is MozaYokeLink.lua.
--
-- Output contract: docs/telemetry.md. One line, for example
--   MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 slip=0.04 rough=0.70 field=1 lowered=1 working=1 end=1
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

-- The game's own thresholds for the terrain "depth" attribute
-- (WheelsUtil.getGroundType): below 0.1 is road, above 0.8 is soft terrain.
M.DEPTH_ROAD = 0.1
M.DEPTH_SOFT = 0.8

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
    { "slip",    function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "rough",   function(v) return M.fmtFixed(clamp(v, 0, 1), 2) end },
    { "field",   flag },
    { "lowered", flag },
    { "working", flag },
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
    if isFunc(vehicle.getIsMotorStarted) and vehicle:getIsMotorStarted() ~= true then
        if rpm ~= nil then rpm = 0 end
        if load ~= nil then load = 0 end
        source[3] = "(engine off)"
    end

    if rpm ~= nil then rpm = math.max(rpm, 0) end
    if load ~= nil then load = clamp(load, 0, 1) end
    return rpm, load, table.concat(source, "/")
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

local function fieldRoughTable()
    local types = FieldGroundType
    if type(types) ~= "table" and type(types) ~= "userdata" then
        return nil
    end
    if fieldRoughCacheFor ~= types then
        local cache = {}
        for name, rough in pairs(M.FIELD_ROUGH_BY_NAME) do
            local ok, value = pcall(function() return types[name] end)
            if ok and value ~= nil then
                cache[value] = rough
            end
        end
        fieldRoughCacheFor, fieldRoughCache = types, cache
    end
    return fieldRoughCache
end

--- Roughness under one wheel.
-- @param physics the wheel's WheelPhysics object (wheel.physics in FS25)
-- @return rough 0..1 or nil when the wheel is in the air / unknown, isField
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
            return M.ROUGH.ROAD, false
        elseif contact == contactTypes.GROUND_HEIGHT then
            return (physics.hasSnowContact == true) and M.ROUGH.SNOW or M.ROUGH.HEAP, false
        end
        -- otherwise WheelContactType.GROUND: terrain, classified below
    elseif physics.hasGroundContact == false then
        return nil, false
    end

    local groundTypes = FieldGroundType
    local densityType = physics.densityType
    if groundTypes ~= nil and densityType ~= nil and densityType ~= groundTypes.NONE then
        if densityType == groundTypes.GRASS or densityType == groundTypes.GRASS_CUT then
            return M.ROUGH.GRASS, false
        end
        local known = fieldRoughTable()
        return (known ~= nil and known[densityType]) or M.ROUGH.FIELD, true
    end

    -- Plain terrain: use the layer's "depth" attribute with the game's own
    -- road / hard / soft thresholds.
    local depth = physics.groundDepth
    if isFinite(depth) then
        if depth < M.DEPTH_ROAD then
            return M.ROUGH.ROAD, false
        elseif depth > M.DEPTH_SOFT then
            return M.ROUGH.GRASS, false
        end
        local span = M.DEPTH_SOFT - M.DEPTH_ROAD
        return M.ROUGH.DIRT_ROAD + (depth - M.DEPTH_ROAD) / span * 0.15, false
    end
    return nil, false
end

--- Surface under the vehicle: rough 0..1 and field 0/1.
function M.getSurface(vehicle)
    local wheelSpec = vehicle.spec_wheels
    local wheels = type(wheelSpec) == "table" and wheelSpec.wheels or nil

    local sum, count, onField = 0, 0, 0
    if type(wheels) == "table" then
        for _, wheel in pairs(wheels) do
            if type(wheel) == "table" then
                local rough, isField = M.classifyWheel(wheel.physics)
                if rough ~= nil then
                    sum = sum + rough
                    count = count + 1
                    if isField then
                        onField = onField + 1
                    end
                end
            end
        end
    end
    if count > 0 then
        return sum / count, (onField * 2 >= count), string.format("wheel.physics (%d wheels in contact)", count)
    end

    -- No usable wheel data: all we can ask is whether the vehicle is on a field.
    if isFunc(vehicle.getIsOnField) then
        local isOnField = vehicle:getIsOnField()
        if isOnField == true then
            return M.ROUGH.FIELD, true, "getIsOnField"
        elseif isOnField == false then
            return nil, false, "getIsOnField"
        end
    end
    return nil, nil, nil
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

    try("slip", function()
        local slip, source = M.getSlip(vehicle, sample.speed)
        if isFinite(slip) then
            sample.slip = slip
            info.sources.slip = source
        end
    end)

    try("surface", function()
        local rough, field, source = M.getSurface(vehicle)
        if isFinite(rough) then sample.rough = rough end
        if type(field) == "boolean" then sample.field = field end
        info.sources.surface = source
    end)

    try("implements", function()
        local lowered, working, count = M.getImplementState(vehicle, sample.speed)
        sample.lowered = lowered
        sample.working = working
        info.sources.implements = string.format("%d attached", count)
    end)

    return sample, info
end
