--
-- Tests for FS25_MozaYokeLink, run under plain Lua with mocked game globals:
--
--     lua fs-mod/tests/run_tests.lua
--
-- Covers the telemetry line (docs/telemetry.md), the gathering code against a
-- mocked tractor with implements, the write-rate limiter, the engine layer
-- (file output, pause, failure handling), the bridge presence file and the
-- force feedback override that follows it.
--

local realIo, realOs, realPrint = io, os, print

local here = (arg and arg[0] or ""):match("^(.*)/[^/]*$") or "."
local modDir = here .. "/../FS25_MozaYokeLink/"
local contractFile = here .. "/../../docs/telemetry.md"
local tmpRoot = here .. "/.tmp"

---------------------------------------------------------------------------
-- minimal test harness
---------------------------------------------------------------------------

local passed, failed, failures = 0, 0, {}

local function test(name, fn)
    local ok, err = xpcall(fn, function(e)
        return debug.traceback(tostring(e), 2)
    end)
    if ok then
        passed = passed + 1
        realPrint("  ok    " .. name)
    else
        failed = failed + 1
        failures[#failures + 1] = name .. "\n" .. tostring(err)
        realPrint("  FAIL  " .. name)
    end
end

local function check(cond, msg)
    if not cond then
        error(msg or "check failed", 2)
    end
end

local function eq(actual, expected, what)
    if actual ~= expected then
        error(string.format("%s: expected %s, got %s", what or "value", tostring(expected), tostring(actual)), 2)
    end
end

local function near(actual, expected, tol, what)
    if type(actual) ~= "number" or math.abs(actual - expected) > (tol or 1e-6) then
        error(string.format("%s: expected about %s, got %s", what or "value", tostring(expected), tostring(actual)), 2)
    end
end

local function readFile(path)
    local f = realIo.open(path, "rb")
    if f == nil then
        return nil
    end
    local text = f:read("a")
    f:close()
    return text
end

---------------------------------------------------------------------------
-- contract checker: asserts a line follows docs/telemetry.md
---------------------------------------------------------------------------

local INT_KEYS = { seq = true, t = true, mass = true, rpm = true, rpmmin = true, rpmmax = true, surface = true }
local FLAG_KEYS = { active = true, field = true, lowered = true, working = true, starting = true, electric = true, ["end"] = true }
local RANGES = {
    steer = { -1, 1 }, load = { 0, 1 }, slip = { 0, 1 }, rough = { 0, 1 }, bumpl = { 0, 1 }, bumpr = { 0, 1 },
    speed = { -200, 200 }, surface = { 1, 8 },
}
local KNOWN_ORDER = { "seq", "t", "active", "speed", "steer", "mass", "rpm", "load", "rpmmin", "rpmmax", "slip", "rough", "surface", "field", "lowered", "working", "starting", "electric", "bumpl", "bumpr", "end" }

--- @return table of key -> text value
local function checkLine(line)
    check(type(line) == "string", "line is not a string")
    check(line:sub(-1) == "\n", "line does not end with \\n")
    local body = line:sub(1, -2)
    check(not body:find("[\r\n]"), "more than one line")
    check(not body:find("[^\32-\126]"), "non-ASCII or control character in line")
    check(body:sub(1, 9) == "MOZAFS 1 ", "bad prefix: " .. body)
    check(body:sub(-6) == " end=1", "end=1 is not the last token: " .. body)
    check(not body:find("  ", 1, true), "double space in line")

    local values, order = {}, {}
    local tokenIndex = 0
    for token in body:gmatch("%S+") do
        tokenIndex = tokenIndex + 1
        if tokenIndex > 2 then
            local key, value = token:match("^([a-z]+)=(.+)$")
            check(key ~= nil, "token is not key=value: " .. token)
            check(values[key] == nil, "duplicate key " .. key)
            check(value:match("^%-?%d+$") or value:match("^%-?%d+%.%d+$"),
                "value is not a plain decimal number (no exponent, no comma): " .. token)
            check(value ~= "-0" and not value:match("^%-0%.0+$"), "negative zero: " .. token)
            if INT_KEYS[key] then
                check(value:match("^%d+$"), "expected a non-negative integer: " .. token)
            end
            if FLAG_KEYS[key] then
                check(value == "0" or value == "1", "expected 0 or 1: " .. token)
            end
            if RANGES[key] then
                local n = tonumber(value)
                check(n >= RANGES[key][1] and n <= RANGES[key][2], "out of range: " .. token)
            end
            values[key] = value
            order[#order + 1] = key
        end
    end

    check(values.seq ~= nil, "seq missing")
    check(values.active ~= nil, "active missing")
    eq(order[#order], "end", "last key")

    -- keys appear in the documented order
    local position = {}
    for i, key in ipairs(KNOWN_ORDER) do
        position[key] = i
    end
    local last = 0
    for _, key in ipairs(order) do
        check(position[key] ~= nil, "unknown key " .. key)
        check(position[key] > last, "key out of order: " .. key)
        last = position[key]
    end
    return values
end

---------------------------------------------------------------------------
-- mocked game
---------------------------------------------------------------------------

local slipByShape = {}

local function resetGameGlobals()
    WheelContactType = { NONE = 1, GROUND = 2, GROUND_HEIGHT = 3, OBJECT = 4 }
    FieldGroundType = {
        NONE = 0, STUBBLE_TILLAGE = 1, CULTIVATED = 2, SEEDBED = 3, PLOWED = 4, ROLLED_SEEDBED = 5,
        RIDGE = 6, SOWN = 7, DIRECT_SOWN = 8, PLANTED = 9, RIDGE_SOWN = 10, ROLLER_LINES = 11,
        HARVEST_READY = 12, HARVEST_READY_OTHER = 13, GRASS = 14, GRASS_CUT = 15,
    }
    WorkAreaType = { DEFAULT = 1, AUXILIARY = 2, CULTIVATOR = 3, SPRAYER = 4 }
    g_currentMission = { time = 913400, paused = false, hud = {} }
    g_localPlayer = nil
    g_time = nil
    g_fillTypeManager = nil
    localToLocal, getWheelShapeContactForce = nil, nil
    -- the values are arbitrary; only the names are the game's
    MotorState = { OFF = 11, IGNITION = 12, STARTING = 13, ON = 14 }
    slipByShape = {}
    getWheelShapeSlip = function(node, shape)
        return slipByShape[shape] or 0, 0
    end
end

--- Wheels 1 and 2 are the steered front pair (left, right), 3 and 4 the rear pair.
local function newWheel(shape, opts)
    opts = opts or {}
    local front, left = shape <= 2, shape % 2 == 1
    return {
        node = 1000 + shape,
        isLeft = left,
        physics = {
            -- +x is the vehicle's left, +z its front
            positionX = left and 0.95 or -0.95,
            positionZ = front and 1.45 or -1.25,
            rotMax = front and 0.61 or 0,
            rotMin = front and -0.61 or 0,
            rotSpeed = front and 1 or 0,
            load = front and 1.8 or 2.4, -- tonnes on the tire
            getTireLoad = function(self) return self.load end,
            wheelShape = shape,
            wheelShapeCreated = true,
            hasGroundContact = opts.hasGroundContact ~= false,
            contact = opts.contact or WheelContactType.GROUND,
            densityType = opts.densityType or FieldGroundType.NONE,
            groundDepth = opts.groundDepth or 0,
            hasSnowContact = opts.hasSnowContact or false,
            radius = opts.radius or 0.8,
            netInfo = { xDriveSpeed = opts.xDriveSpeed or (5.42 / 0.8), slip = opts.netSlip or 0, suspensionLength = 0.1 },
        },
    }
end

local function setSurface(vehicle, opts)
    for _, wheel in ipairs(vehicle.spec_wheels.wheels) do
        local p = wheel.physics
        p.contact = opts.contact or WheelContactType.GROUND
        p.densityType = opts.densityType or FieldGroundType.NONE
        p.groundDepth = opts.groundDepth or 0
        p.hasSnowContact = opts.hasSnowContact or false
        p.hasGroundContact = p.contact ~= WheelContactType.NONE
    end
end

--- A 7.2 t four wheel drive tractor doing 5.42 m/s on a sown field,
-- steering a quarter of the way to the left.
local function newTractor()
    local v = {
        isServer = true,
        typeName = "tractor",
        ownMass = 7.2,
        lastSignedSpeed = 0.00542,
        lastSpeedReal = 0.00542,
        lastSpeed = 0.00542,
        movingDirection = 1,
        maxRotTime = 0.6,
        minRotTime = -0.8,
        rotatedTime = 0.15, -- a quarter of maxRotTime: left
        spec_drivable = { axisSide = -0.25, steeringDirection = 1, forceFeedback = { isActive = false, axisIndex = 0, intensity = 1 } },
        spec_motorized = {
            motor = {
                rpm = 1450,
                getLastModulatedMotorRpm = function(self) return self.rpm end,
                getLastMotorRpm = function(self) return self.rpm end,
                getSmoothLoadPercentage = function() return 0.62 end,
                minRpm = 850,
                maxRpm = 2200,
                getMinRpm = function(self) return self.minRpm end,
                getMaxRpm = function(self) return self.maxRpm end,
            },
            smoothedLoadPercentage = 0.62,
            motorState = MotorState.ON,
            motorStartTime = 0,
            -- a diesel tractor with exhaust fluid and air brakes
            consumersByFillTypeName = {
                DIESEL = { fillUnitIndex = 1, fillType = 32 },
                DEF = { fillUnitIndex = 2, fillType = 33 },
                AIR = { fillUnitIndex = 3, fillType = 34 },
            },
            differentials = {
                { diffIndex1 = 1, diffIndex1IsWheel = true, diffIndex2 = 2, diffIndex2IsWheel = true },
                { diffIndex1 = 3, diffIndex1IsWheel = true, diffIndex2 = 4, diffIndex2IsWheel = true },
                { diffIndex1 = 0, diffIndex1IsWheel = false, diffIndex2 = 1, diffIndex2IsWheel = false },
            },
        },
        spec_wheels = { wheels = {} },
        spec_attacherJoints = { attachedImplements = {}, attacherJoints = {} },
    }
    for shape = 1, 4 do
        v.spec_wheels.wheels[shape] = newWheel(shape, { densityType = FieldGroundType.SOWN, groundDepth = 1 })
    end
    slipByShape[1], slipByShape[2], slipByShape[3], slipByShape[4] = 0.02, 0.03, 0.04, 0.01

    function v:getFullName() return "Mock Tractor 7R" end
    function v:getLastSpeed() return math.abs(self.lastSpeed) * 3600 end
    function v:getMotorRpmReal() return self.spec_motorized.motor:getLastModulatedMotorRpm() end
    function v:getMotorLoadPercentage() return self.spec_motorized.smoothedLoadPercentage end
    function v:getMotorState() return self.spec_motorized.motorState end
    function v:getIsMotorStarted() return self.spec_motorized.motorState == MotorState.ON end
    function v:getMotor() return self.spec_motorized.motor end
    function v:getIsLowered(default) return false end
    function v:getIsOnField() return true end
    function v:getAttachedImplements() return self.spec_attacherJoints.attachedImplements end
    function v:getAttacherJointDescFromObject(object)
        for _, implement in ipairs(self.spec_attacherJoints.attachedImplements) do
            if implement.object == object then
                return self.spec_attacherJoints.attacherJoints[implement.jointDescIndex]
            end
        end
        return nil
    end
    -- the game's semantics: tonnes; without "only given vehicle" the attached
    -- implements are added; a hard-attached implement is also part of the
    -- vehicle's own component mass
    function v:getTotalMass(onlyGivenVehicle)
        local mass = self.ownMass
        for _, implement in ipairs(self.spec_attacherJoints.attachedImplements) do
            local object = implement.object
            if object.spec_attachable.isHardAttached then
                mass = mass + object:getTotalMass(true)
            end
            if not onlyGivenVehicle then
                mass = mass + object:getTotalMass(onlyGivenVehicle)
            end
        end
        return mass
    end
    return v
end

local function newImplement(opts)
    opts = opts or {}
    local o = {
        typeName = opts.typeName or "cultivator",
        ownMass = opts.mass or 1.0,
        attacherVehicle = nil,
        movingDirection = 1,
        spec_attachable = { isHardAttached = opts.hardAttached == true },
        spec_attacherJoints = { attachedImplements = {}, attacherJoints = {} },
    }
    if opts.workArea ~= false then
        o.spec_workArea = {
            workAreas = {
                { type = WorkAreaType.AUXILIARY, lastProcessingTime = g_currentMission.time },
                {
                    type = opts.areaType or WorkAreaType.CULTIVATOR,
                    lastProcessingTime = opts.processing == false and 0 or g_currentMission.time,
                    requiresGroundContact = opts.requiresGroundContact ~= false,
                    groundReferenceNode = { isActive = true },
                },
            },
        }
        function o:getIsWorkAreaProcessing(area)
            return area.lastProcessingTime + 200 >= g_currentMission.time
        end
        function o:getIsWorkAreaActive(area)
            return self:getIsLowered(false)
        end
    end
    function o:getTotalMass(onlyGivenVehicle)
        local mass = self.ownMass
        if not onlyGivenVehicle then
            for _, implement in ipairs(self.spec_attacherJoints.attachedImplements) do
                mass = mass + implement.object:getTotalMass(onlyGivenVehicle)
            end
        end
        return mass
    end
    function o:getAttachedImplements() return self.spec_attacherJoints.attachedImplements end
    function o:getAttacherJointDescFromObject(object)
        for _, implement in ipairs(self.spec_attacherJoints.attachedImplements) do
            if implement.object == object then
                return self.spec_attacherJoints.attacherJoints[implement.jointDescIndex]
            end
        end
        return nil
    end
    -- Attachable.getIsLowered
    function o:getIsLowered(default)
        local attacher = self.attacherVehicle
        if attacher ~= nil then
            local jointDesc = attacher:getAttacherJointDescFromObject(self)
            if jointDesc ~= nil then
                if jointDesc.allowsLowering or jointDesc.isDefaultLowered then
                    return jointDesc.moveDown
                end
                return default
            end
        end
        return false
    end
    return o
end

local function attach(attacher, object, jointOpts)
    jointOpts = jointOpts or {}
    local joints = attacher.spec_attacherJoints.attacherJoints
    local jointDesc = {
        allowsLowering = jointOpts.allowsLowering ~= false,
        isDefaultLowered = jointOpts.isDefaultLowered == true,
        moveDown = jointOpts.moveDown == true,
    }
    joints[#joints + 1] = jointDesc
    table.insert(attacher.spec_attacherJoints.attachedImplements, { object = object, jointDescIndex = #joints })
    object.attacherVehicle = attacher
    return jointDesc
end

--- The scene in the contract's example line.
local function newContractScene()
    local tractor = newTractor()
    local cultivator = newImplement({ mass = 1.0 })
    local joint = attach(tractor, cultivator, { moveDown = true })
    return tractor, cultivator, joint
end

local function lineFor(vehicle, seq)
    local sample, info = MozaTelemetry.gather(vehicle)
    local line = MozaTelemetry.formatLine(seq or 1, g_currentMission.time, sample)
    return line, checkLine(line), sample, info
end

---------------------------------------------------------------------------
-- load the pure half of the mod
---------------------------------------------------------------------------

resetGameGlobals()
dofile(modDir .. "scripts/MozaTelemetry.lua")
local T = MozaTelemetry

realPrint("formatting")

test("fmtFixed and fmtInt produce plain decimals", function()
    eq(T.fmtFixed(5.424, 2), "5.42")
    eq(T.fmtFixed(5.426, 2), "5.43")
    eq(T.fmtFixed(-0.25, 3), "-0.250")
    eq(T.fmtFixed(0, 2), "0.00")
    eq(T.fmtFixed(-0.0001, 2), "0.00", "no negative zero")
    eq(T.fmtFixed(1e-9, 3), "0.000", "tiny value, no exponent")
    eq(T.fmtFixed(123456.789, 2), "123456.79", "no thousands separator")
    eq(T.fmtFixed(0.07, 2), "0.07", "leading zero in the fraction")
    eq(T.fmtFixed(1, 3), "1.000")
    eq(T.fmtInt(8200.4), "8200")
    eq(T.fmtInt(8199.5), "8200")
    eq(T.fmtInt(0), "0")
    eq(T.fmtInt(-0.2), "0")
    eq(T.fmtInt(-3), "-3")
    eq(T.fmtInt(2000000), "2000000", "no exponent for large integers")
    eq(T.fmtFixed(0 / 0, 2), nil, "NaN")
    eq(T.fmtFixed(math.huge, 2), nil, "inf")
    eq(T.fmtFixed(-math.huge, 2), nil, "-inf")
    eq(T.fmtInt(0 / 0), nil, "NaN")
    eq(T.fmtInt(1e300), nil, "absurd value")
    eq(T.fmtFixed("5", 2), nil, "string")
    eq(T.fmtInt(nil), nil, "nil")
end)

-- The example line in docs/telemetry.md, and the same line with the two keys
-- added after it was written. A real vehicle always has both, so the line a
-- tractor produces is the second one; the doc may show either.
local contractExample = "MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 slip=0.04 rough=0.70 field=1 lowered=1 working=1 end=1"
local driveExample = contractExample:gsub(" end=1$", " starting=0 electric=0 end=1")
local bumpExample = contractExample:gsub(" end=1$", " starting=0 electric=0 bumpl=0.00 bumpr=0.00 end=1")
-- ... and with the engine's idle and maximum speed after load: what the mocked tractor produces now
local rangeExample = bumpExample:gsub(" load=0%.62 ", " load=0.62 rpmmin=850 rpmmax=2200 ")
-- ... and with the kind of surface after rough
local fullExample = rangeExample:gsub(" rough=0%.70 ", " rough=0.70 surface=6 ")

test("the example in docs/telemetry.md is the one tested here", function()
    local doc = readFile(contractFile)
    if doc == nil then
        realPrint("        (docs/telemetry.md not found, using the embedded copy)")
        return
    end
    check(doc:find(contractExample, 1, true) or doc:find(driveExample, 1, true) or doc:find(bumpExample, 1, true)
        or doc:find(rangeExample, 1, true) or doc:find(fullExample, 1, true),
        "docs/telemetry.md no longer contains the example line this test expects")
    for _, key in ipairs({ "starting", "electric", "bumpl", "bumpr", "rpmmin", "rpmmax", "surface" }) do
        check(doc:find("| `" .. key .. "` |", 1, true), "the key table lists " .. key)
    end
end)

test("formatLine reproduces the contract example byte for byte", function()
    local sample = {
        active = true, speed = 5.42, steer = -0.25, mass = 8200, rpm = 1450, load = 0.62,
        slip = 0.04, rough = 0.7, field = true, lowered = true, working = true,
    }
    local line = T.formatLine(4821, 913400, sample)
    eq(line, contractExample .. "\n")
    checkLine(line)

    -- with the two later keys: after working, before end=1
    sample.starting, sample.electric = false, false
    line = T.formatLine(4821, 913400, sample)
    eq(line, driveExample .. "\n")
    checkLine(line)
    sample.starting, sample.electric = true, false
    eq(T.formatLine(4821, 913400, sample), (driveExample:gsub("starting=0", "starting=1")) .. "\n")

    -- and with the bump keys: last before end=1, two decimals like the other fractions
    sample.starting, sample.bumpl, sample.bumpr = false, 0, 0
    line = T.formatLine(4821, 913400, sample)
    eq(line, bumpExample .. "\n")
    checkLine(line)

    -- and with the engine's idle and maximum speed: whole numbers, right after load
    sample.rpmmin, sample.rpmmax = 850, 2200
    line = T.formatLine(4821, 913400, sample)
    eq(line, rangeExample .. "\n")
    checkLine(line)

    -- and with the kind of surface: a whole number, right after rough
    sample.surface = 6
    line = T.formatLine(4821, 913400, sample)
    eq(line, fullExample .. "\n")
    eq(line, "MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 rpmmin=850 rpmmax=2200 slip=0.04 rough=0.70 surface=6 field=1 lowered=1 working=1 starting=0 electric=0 bumpl=0.00 bumpr=0.00 end=1\n")
    checkLine(line)
    -- a kind outside the contract's list is left out rather than sent as something it is not
    for _, odd in ipairs({ 0, 9, -1, 1 / 0 }) do
        sample.surface = odd
        eq(T.formatLine(4821, 913400, sample), rangeExample .. "\n", "surface " .. tostring(odd))
    end
    sample.surface = nil
    sample.rpmmin, sample.rpmmax = 849.6, 2200.4
    local range = checkLine(T.formatLine(4821, 913400, sample))
    eq(range.rpmmin, "850") eq(range.rpmmax, "2200")
    sample.rpmmin, sample.rpmmax = nil, nil
    sample.bumpl, sample.bumpr = 0.304, 7
    local v = checkLine(T.formatLine(4821, 913400, sample))
    eq(v.bumpl, "0.30") eq(v.bumpr, "1.00", "clamped")
    sample.bumpl, sample.bumpr = -0.2, 0 / 0
    v = checkLine(T.formatLine(4821, 913400, sample))
    eq(v.bumpl, "0.00") eq(v.bumpr, nil, "NaN is left out")
    sample.starting, sample.electric, sample.bumpl, sample.bumpr = nil, true, nil, nil
    eq(T.formatLine(4821, 913400, sample), (contractExample:gsub(" end=1$", " electric=1 end=1")) .. "\n", "each key is left out on its own")
end)

test("not driving: active=0 and nothing else", function()
    eq(T.formatLine(7, 1000, { active = false }), "MOZAFS 1 seq=7 t=1000 active=0 end=1\n")
    eq(T.formatLine(8, 1000, nil), "MOZAFS 1 seq=8 t=1000 active=0 end=1\n")
    -- values are ignored when not active
    eq(T.formatLine(9, 1000.7, { active = false, speed = 3 }), "MOZAFS 1 seq=9 t=1001 active=0 end=1\n")
    -- no usable time: t is simply left out
    eq(T.formatLine(10, nil, nil), "MOZAFS 1 seq=10 active=0 end=1\n")
    checkLine(T.formatLine(10, 0 / 0, nil))
end)

test("unusable values are omitted, out of range values are clamped", function()
    local line = T.formatLine(1, 5, {
        active = true, speed = 0 / 0, steer = math.huge, mass = "heavy", rpm = {}, load = 7,
        slip = -3, rough = 1e-12, field = 1, lowered = "yes", working = false,
    })
    local v = checkLine(line)
    eq(v.speed, nil, "NaN speed") eq(v.steer, nil, "inf steer") eq(v.mass, nil, "string mass") eq(v.rpm, nil, "table rpm")
    eq(v.load, "1.00") eq(v.slip, "0.00") eq(v.rough, "0.00")
    eq(v.field, "1") eq(v.lowered, nil, "string flag") eq(v.working, "0")
    local big = checkLine(T.formatLine(1, 5, { active = true, speed = -1e9, steer = -7, mass = 1e30, rpm = -50 }))
    eq(big.speed, "-200.00") eq(big.steer, "-1.000") eq(big.mass, "10000000") eq(big.rpm, "0")
end)

realPrint("gathering from a mocked tractor with a cultivator")

test("tractor + lowered, working cultivator reproduces the contract example", function()
    resetGameGlobals()
    local tractor = newContractScene()
    local sample, info = T.gather(tractor)
    local line = T.formatLine(4821, g_currentMission.time, sample)
    checkLine(line)
    eq(line, fullExample .. "\n")
    check(next(info.errors) == nil, "unexpected gather errors")
    eq(info.sources.starting, "getMotorState")
    eq(info.sources.electric, "consumersByFillTypeName (AIR,DEF,DIESEL)")
    eq(info.sources.bumps, "tire load against its running average of the steered wheels (1 left, 1 right)")
    eq(info.sources.rpmrange, "motor:getMinRpm/getMaxRpm (850..2200)")
    eq(info.sources.speed, "lastSignedSpeed")
    eq(info.sources.steer, "rotatedTime")
    check(info.sources.slip:find("getWheelShapeSlip", 1, true), "slip source: " .. tostring(info.sources.slip))
    check(info.sources.slip:find("4 wheels, differentials", 1, true), "driven wheels: " .. tostring(info.sources.slip))
end)

test("speed is signed and in m/s, with fallbacks", function()
    resetGameGlobals()
    local tractor = newTractor()
    tractor.lastSignedSpeed = -0.002
    eq(select(2, lineFor(tractor)).speed, "-2.00", "reverse")

    tractor.lastSignedSpeed = nil
    tractor.lastSpeed = 0.005 -- 18 km/h
    tractor.movingDirection = -1
    local line, v, _, info = lineFor(tractor)
    eq(v.speed, "-5.00", "km/h fallback, reversing")
    eq(info.sources.speed, "getLastSpeed*movingDirection")

    tractor.getLastSpeed = nil
    tractor.lastSpeedReal = 0.003
    tractor.movingDirection = 1
    eq(select(2, lineFor(tractor)).speed, "3.00", "lastSpeedReal fallback")

    tractor.lastSpeedReal = nil
    eq(select(2, lineFor(tractor)).speed, nil, "no speed source: key omitted")

    tractor.lastSignedSpeed = -0.000001
    eq(select(2, lineFor(tractor)).speed, "0.00", "no negative zero when creeping")
end)

test("steer: positive is right, negative is left", function()
    resetGameGlobals()
    local tractor = newTractor()
    tractor.rotatedTime = tractor.maxRotTime -- the game's full left
    eq(select(2, lineFor(tractor)).steer, "-1.000", "full left")
    tractor.rotatedTime = tractor.minRotTime -- the game's full right
    eq(select(2, lineFor(tractor)).steer, "1.000", "full right")
    tractor.rotatedTime = tractor.minRotTime / 2
    eq(select(2, lineFor(tractor)).steer, "0.500", "half right uses minRotTime")
    tractor.rotatedTime = 0
    eq(select(2, lineFor(tractor)).steer, "0.000", "centre")
    tractor.rotatedTime = 99
    eq(select(2, lineFor(tractor)).steer, "-1.000", "clamped")

    -- the same mapping the game applies from the steering axis (Drivable.updateVehiclePhysics)
    for _, axisSide in ipairs({ -1, -0.3, 0, 0.4, 1 }) do
        if axisSide < 0 then
            tractor.rotatedTime = math.min(-tractor.maxRotTime * axisSide, tractor.maxRotTime)
        else
            tractor.rotatedTime = math.max(tractor.minRotTime * axisSide, tractor.minRotTime)
        end
        near(T.getSteer(tractor), axisSide, 1e-9, "steer for axisSide " .. axisSide)
    end

    -- fallback to the steering axis when the vehicle has no rotation limits
    tractor.maxRotTime, tractor.minRotTime = nil, nil
    tractor.spec_drivable.axisSide = 0.75
    local _, v, _, info = lineFor(tractor)
    eq(v.steer, "0.750")
    eq(info.sources.steer, "spec_drivable.axisSide")
    tractor.spec_drivable = nil
    eq(select(2, lineFor(tractor)).steer, nil, "no steering source: key omitted")
end)

test("mass is in kg and includes everything attached, once", function()
    resetGameGlobals()
    local tractor, cultivator = newContractScene()
    eq(select(2, lineFor(tractor)).mass, "8200")

    -- a trailer behind the cultivator (implement on an implement)
    local trailer = newImplement({ mass = 12.5, workArea = false })
    attach(cultivator, trailer, { allowsLowering = false })
    eq(select(2, lineFor(tractor)).mass, "20700", "nested implement")

    -- a hard-attached front weight is already inside the tractor's own mass
    local weight = newImplement({ mass = 0.9, hardAttached = true, workArea = false })
    attach(tractor, weight, { allowsLowering = false })
    near(tractor:getTotalMass(), 7.2 + 0.9 + 0.9 + 1.0 + 12.5, 1e-9, "the game's own sum counts it twice")
    eq(select(2, lineFor(tractor)).mass, "21600", "hard-attached implement counted once")

    tractor.getTotalMass = nil
    eq(select(2, lineFor(tractor)).mass, nil, "no mass source: key omitted")
end)

test("engine rpm and load, zero when the engine is off", function()
    resetGameGlobals()
    local tractor = newTractor()
    local _, v = lineFor(tractor)
    eq(v.rpm, "1450") eq(v.load, "0.62")

    tractor.spec_motorized.motorState = MotorState.OFF
    _, v = lineFor(tractor)
    eq(v.rpm, "0") eq(v.load, "0.00")

    tractor.spec_motorized.motorState = MotorState.ON
    tractor.getMotorRpmReal = nil
    tractor.getMotorLoadPercentage = nil
    local info
    _, v, _, info = lineFor(tractor)
    eq(v.rpm, "1450") eq(v.load, "0.62")
    check(info.sources.engine:find("motor:getLastModulatedMotorRpm", 1, true), info.sources.engine)

    tractor.spec_motorized = nil
    _, v = lineFor(tractor)
    eq(v.rpm, nil, "no engine: rpm omitted") eq(v.load, nil, "no engine: load omitted")
end)

test("slip is the largest of the driven wheels", function()
    resetGameGlobals()
    local tractor = newTractor()
    eq(select(2, lineFor(tractor)).slip, "0.04")

    slipByShape[2] = -0.37 -- sign does not matter, magnitude does
    eq(select(2, lineFor(tractor)).slip, "0.37")
    slipByShape[2] = 5
    eq(select(2, lineFor(tractor)).slip, "1.00", "clamped to 1")
    slipByShape[2] = 0.03

    -- an undriven wheel (not in any differential) is ignored
    tractor.spec_wheels.wheels[5] = newWheel(5)
    slipByShape[5] = 0.9
    eq(select(2, lineFor(tractor)).slip, "0.04", "undriven wheel ignored")

    -- a driven wheel in the air does not count
    slipByShape[3] = 0.8
    tractor.spec_wheels.wheels[3].physics.hasGroundContact = false
    eq(select(2, lineFor(tractor)).slip, "0.03", "airborne wheel ignored")
end)

test("slip falls back to wheel speed against ground speed", function()
    resetGameGlobals()
    local tractor = newTractor()
    getWheelShapeSlip = nil
    -- rear wheels turning for 8 m/s while the tractor does 5.42 m/s
    tractor.spec_wheels.wheels[3].physics.netInfo.xDriveSpeed = 8 / 0.8
    local _, v, _, info = lineFor(tractor)
    near(tonumber(v.slip), (8 - 5.42) / 8, 0.006, "kinematic slip")
    check(info.sources.slip:find("xDriveSpeed", 1, true), info.sources.slip)

    -- stuck: wheels spinning, vehicle not moving
    tractor.lastSignedSpeed = 0
    eq(select(2, lineFor(tractor)).slip, "1.00", "spinning on the spot")

    -- standing still with still wheels
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.netInfo.xDriveSpeed = 0
    end
    eq(select(2, lineFor(tractor)).slip, "0.00", "at rest")

    -- multiplayer client: no physics wheel shapes, even if the function exists
    resetGameGlobals()
    tractor = newTractor()
    tractor.isServer = false
    slipByShape[1] = 0.9
    eq(select(2, lineFor(tractor)).slip, "0.00", "client ignores getWheelShapeSlip")

    tractor.spec_wheels = nil
    eq(select(2, lineFor(tractor)).slip, nil, "no wheels: key omitted")
end)

test("rough and field distinguish road, dirt, grass and field", function()
    resetGameGlobals()
    local tractor = newTractor()
    local function surface(opts)
        setSurface(tractor, opts)
        local _, v = lineFor(tractor)
        return tonumber(v.rough), v.field, v.surface
    end

    local asphaltMesh, f1 = surface({ contact = WheelContactType.OBJECT })
    local asphaltPaint, f2 = surface({ groundDepth = 0 })
    local dirtRoad, f3 = surface({ groundDepth = 0.2 })
    local softGround, f4 = surface({ groundDepth = 1 })
    local meadow, f5 = surface({ densityType = FieldGroundType.GRASS, groundDepth = 1 })
    local sown, f6 = surface({ densityType = FieldGroundType.SOWN, groundDepth = 1 })
    local cultivated, f7 = surface({ densityType = FieldGroundType.CULTIVATED, groundDepth = 1 })
    local plowed, f8 = surface({ densityType = FieldGroundType.PLOWED, groundDepth = 1 })

    eq(asphaltMesh, 0, "road mesh") eq(asphaltPaint, 0, "asphalt terrain")
    near(dirtRoad, 0.32, 0.011, "dirt road")
    eq(softGround, 0.5, "grass / soft terrain") eq(meadow, 0.5, "meadow")
    eq(sown, 0.7, "sown field") eq(cultivated, 0.85, "cultivated field") eq(plowed, 1, "plowed field")
    check(asphaltMesh < dirtRoad and dirtRoad < softGround and softGround < sown and sown < plowed, "ordering")

    eq(f1, "0") eq(f2, "0") eq(f3, "0") eq(f4, "0") eq(f5, "0", "a meadow is not a field")
    eq(f6, "1") eq(f7, "1") eq(f8, "1")

    -- the kind of surface, which is what the bridge picks its rumble pattern by
    local function kind(opts) return (select(3, surface(opts))) end
    eq(kind({ contact = WheelContactType.OBJECT }), "1", "road mesh")
    eq(kind({ groundDepth = 0 }), "1", "asphalt terrain")
    eq(kind({ groundDepth = 0.2 }), "2", "dirt road")
    eq(kind({ groundDepth = 1 }), "3", "soft terrain")
    eq(kind({ densityType = FieldGroundType.GRASS, groundDepth = 1 }), "3", "meadow")
    eq(kind({ contact = WheelContactType.GROUND_HEIGHT, hasSnowContact = true }), "4", "snow")
    eq(kind({ contact = WheelContactType.GROUND_HEIGHT }), "5", "heap")
    eq(kind({ densityType = FieldGroundType.SOWN, groundDepth = 1 }), "6", "sown field")
    eq(kind({ densityType = FieldGroundType.CULTIVATED, groundDepth = 1 }), "7", "cultivated")
    eq(kind({ densityType = FieldGroundType.PLOWED, groundDepth = 1 }), "7", "plowed")
    eq(kind({ densityType = FieldGroundType.STUBBLE_TILLAGE, groundDepth = 1 }), "7", "stubble tillage")
    eq(kind({ densityType = FieldGroundType.RIDGE, groundDepth = 1 }), "8", "ridges")
    eq(kind({ densityType = FieldGroundType.RIDGE_SOWN, groundDepth = 1 }), "8", "sown ridges")
    eq(kind({ densityType = FieldGroundType.ROLLED_SEEDBED, groundDepth = 1 }), "6", "rolled seedbed")

    -- two wheels on the road, two on the field: half way, and still "on the field"
    setSurface(tractor, { densityType = FieldGroundType.SOWN, groundDepth = 1 })
    for shape = 1, 2 do
        local p = tractor.spec_wheels.wheels[shape].physics
        p.contact, p.densityType, p.groundDepth = WheelContactType.OBJECT, FieldGroundType.NONE, 0
    end
    local _, v = lineFor(tractor)
    eq(v.rough, "0.35") eq(v.field, "1")
    eq(v.surface, "6", "an even split goes to the rougher kind, the one that is felt")
    -- three wheels on the road: the road wins, whatever the fourth is on
    do
        local p = tractor.spec_wheels.wheels[3].physics
        p.contact, p.densityType, p.groundDepth = WheelContactType.OBJECT, FieldGroundType.NONE, 0
    end
    eq(select(2, lineFor(tractor)).surface, "1", "most wheels decide")

    -- a field state this script has no entry for still reads as a field
    FieldGroundType.SOMETHING_NEW = 99
    eq((surface({ densityType = 99, groundDepth = 1 })), 0.7, "unknown field state")
    eq(kind({ densityType = 99, groundDepth = 1 }), "6", "and as a plain field")

    -- snow and heaps
    eq((surface({ contact = WheelContactType.GROUND_HEIGHT, hasSnowContact = true })), 0.4, "snow")
    eq((surface({ contact = WheelContactType.GROUND_HEIGHT })), 0.6, "heap")

    -- all wheels in the air: rough omitted, field falls back to the vehicle
    setSurface(tractor, { contact = WheelContactType.NONE })
    _, v = lineFor(tractor)
    eq(v.rough, "0.70", "falls back to getIsOnField") eq(v.field, "1") eq(v.surface, "6")
    tractor.getIsOnField = function() return false end
    _, v = lineFor(tractor)
    eq(v.rough, nil, "unknown surface: rough omitted") eq(v.field, "0")
    eq(v.surface, nil, "and so is the kind")
end)

test("surface works without the FS25 enums (older wheel layout)", function()
    resetGameGlobals()
    local tractor = newTractor()
    WheelContactType = nil
    FieldGroundType = nil
    local _, v = lineFor(tractor)
    -- only the terrain depth is left to go on: soft ground
    eq(v.rough, "0.50") eq(v.field, "0") eq(v.surface, "3")
end)

test("lowered and working follow the implement", function()
    resetGameGlobals()
    local tractor, cultivator, joint = newContractScene()
    local area = cultivator.spec_workArea.workAreas[2]
    local _, v = lineFor(tractor)
    eq(v.lowered, "1") eq(v.working, "1")

    -- raised: not lowered, and the ground stops being worked
    joint.moveDown = false
    area.lastProcessingTime = g_currentMission.time - 5000
    _, v = lineFor(tractor)
    eq(v.lowered, "0") eq(v.working, "0")

    -- lowered into already worked ground while moving: nothing "processed",
    -- but the tool is in the ground
    joint.moveDown = true
    _, v = lineFor(tractor)
    eq(v.lowered, "1") eq(v.working, "1", "ground contact while moving")

    -- same, standing still
    tractor.lastSignedSpeed = 0
    _, v = lineFor(tractor)
    eq(v.lowered, "1") eq(v.working, "0", "standing still")

    -- only the auxiliary area "processed": not work
    tractor.lastSignedSpeed = 0.00542
    joint.moveDown = false
    cultivator.spec_workArea.workAreas[1].lastProcessingTime = g_currentMission.time
    _, v = lineFor(tractor)
    eq(v.working, "0", "auxiliary work areas are ignored")
end)

test("things that are not lowered implements do not read as lowered", function()
    resetGameGlobals()
    local tractor = newTractor()
    local _, v = lineFor(tractor)
    eq(v.lowered, "0", "bare tractor") eq(v.working, "0", "bare tractor")

    -- trailer on a drawbar
    local trailer = newImplement({ workArea = false })
    attach(tractor, trailer, { allowsLowering = false })
    _, v = lineFor(tractor)
    eq(v.lowered, "0", "trailer")

    -- front loader tool carrier: cannot be lowered but counts as always down
    resetGameGlobals()
    tractor = newTractor()
    local bucket = newImplement({ workArea = false })
    attach(tractor, bucket, { allowsLowering = false, isDefaultLowered = true, moveDown = true })
    check(bucket:getIsLowered(false) == true, "the game calls this lowered")
    _, v = lineFor(tractor)
    eq(v.lowered, "0", "front loader tool")

    -- a pickup (baler, forage wagon) has its own lowered state on such a joint
    resetGameGlobals()
    tractor = newTractor()
    local baler = newImplement({ workArea = false })
    baler.spec_pickup = { isLowered = true }
    baler.getIsLowered = function(self) return self.spec_pickup.isLowered end
    attach(tractor, baler, { allowsLowering = false })
    _, v = lineFor(tractor)
    eq(v.lowered, "1", "pickup lowered")

    -- a lowered implement behind a raised one still counts
    resetGameGlobals()
    tractor = newTractor()
    local front = newImplement({ processing = false })
    local rear = newImplement({ processing = false })
    attach(tractor, front, { moveDown = false })
    attach(tractor, rear, { moveDown = true })
    _, v = lineFor(tractor)
    eq(v.lowered, "1", "any implement")
end)

test("a sprayer that is spraying counts as working without being lowered", function()
    resetGameGlobals()
    local tractor = newTractor()
    local sprayer = newImplement({ areaType = WorkAreaType.SPRAYER, requiresGroundContact = false })
    attach(tractor, sprayer, { allowsLowering = false })
    local _, v = lineFor(tractor)
    eq(v.lowered, "0") eq(v.working, "1")
end)

realPrint("engine start and electric drive")

--- A battery tractor: the same machine with a charge consumer and no fuel.
local function newElectricTractor()
    local v = newTractor()
    v.spec_motorized.consumersByFillTypeName = {
        ELECTRICCHARGE = { fillUnitIndex = 1, fillType = 40 },
        AIR = { fillUnitIndex = 2, fillType = 34 },
    }
    return v
end

test("diesel: starting is 1 from the start being triggered until the engine runs", function()
    resetGameGlobals()
    local tractor = newTractor()
    local spec = tractor.spec_motorized
    spec.motor.rpm = 400 -- what the game's motor reports while it is being cranked

    spec.motorState = MotorState.OFF
    local _, v = lineFor(tractor)
    eq(v.starting, "0", "off") eq(v.electric, "0") eq(v.rpm, "0")

    spec.motorState = MotorState.IGNITION -- electrics on, engine not turning
    _, v = lineFor(tractor)
    eq(v.starting, "0", "ignition only")

    spec.motorState = MotorState.STARTING
    local info
    _, v, _, info = lineFor(tractor)
    eq(v.starting, "1", "starting") eq(v.electric, "0")
    eq(v.rpm, "0", "rpm keeps its meaning: 0 until the engine runs") eq(v.load, "0.00")
    eq(info.sources.starting, "getMotorState")
    check(info.sources.engine:find("(engine not running: reported as 0)", 1, true), info.sources.engine)

    spec.motorState = MotorState.ON
    spec.motor.rpm = 851
    _, v = lineFor(tractor)
    eq(v.starting, "0", "running") eq(v.rpm, "851") eq(v.load, "0.62")
end)

test("starting: fallbacks, and the key is left out when the vehicle cannot say", function()
    resetGameGlobals()
    local tractor = newTractor()
    local spec = tractor.spec_motorized
    spec.motorState = MotorState.STARTING

    tractor.getMotorState = nil
    local _, v, _, info = lineFor(tractor)
    eq(v.starting, "1") eq(info.sources.starting, "spec_motorized.motorState")

    tractor.getMotorState = function() error("no state today") end
    _, v, _, info = lineFor(tractor)
    eq(v.starting, "1", "a raising getter falls back to the field") eq(info.errors.starting, nil)

    tractor.getMotorState = function() return nil end
    spec.motorState = MotorState.ON
    _, v = lineFor(tractor)
    eq(v.starting, "0")

    -- no state anywhere
    spec.motorState = nil
    _, v, _, info = lineFor(tractor)
    eq(v.starting, nil, "unknown: omitted") eq(info.errors.starting, nil) eq(v.electric, "0", "electric does not depend on it")

    -- no MotorState enum (FS22): one flag plus the time the start takes
    resetGameGlobals()
    tractor = newTractor()
    spec = tractor.spec_motorized
    MotorState = nil
    tractor.getMotorState, spec.motorState = nil, nil
    tractor.getIsMotorStarted = function(self) return self.spec_motorized.isMotorStarted end
    spec.isMotorStarted = true
    spec.motorStartTime = g_currentMission.time + 800
    _, v, _, info = lineFor(tractor)
    eq(v.starting, "1") eq(info.sources.starting, "isMotorStarted+motorStartTime")
    spec.motorStartTime = g_currentMission.time - 1
    eq(select(2, lineFor(tractor)).starting, "0", "start time passed")
    spec.isMotorStarted = false
    spec.motorStartTime = g_currentMission.time + 800
    eq(select(2, lineFor(tractor)).starting, "0", "switched off again during the start")
    spec.isMotorStarted = nil
    eq(select(2, lineFor(tractor)).starting, nil, "nothing to go on: omitted")

    -- an enum without STARTING, or one that raises on unknown names
    resetGameGlobals()
    tractor = newTractor()
    MotorState = setmetatable({}, { __index = function(_, name) error("no such state " .. tostring(name)) end })
    _, v, _, info = lineFor(tractor)
    eq(v.starting, nil) eq(info.errors.starting, nil)
end)

test("electric vehicle: electric=1, never starting, rpm and load still reported", function()
    resetGameGlobals()
    local tractor = newElectricTractor()
    local spec = tractor.spec_motorized
    local _, v, _, info = lineFor(tractor)
    eq(v.electric, "1") eq(v.starting, "0") eq(v.rpm, "1450") eq(v.load, "0.62")
    eq(info.sources.electric, "consumersByFillTypeName (AIR,ELECTRICCHARGE)")
    eq(info.sources.starting, "electric: never")

    -- the game runs the same start sequence for it; the contract says never 1
    for _, state in ipairs({ MotorState.OFF, MotorState.IGNITION, MotorState.STARTING, MotorState.ON }) do
        spec.motorState = state
        T.resetVehicleCache()
        local values = select(2, lineFor(tractor))
        eq(values.electric, "1") eq(values.starting, "0", "state " .. state)
    end

    -- also when the vehicle cannot report a motor state at all
    tractor.getMotorState, spec.motorState = nil, nil
    _, v = lineFor(tractor)
    eq(v.electric, "1") eq(v.starting, "0")
end)

test("electric only when the motor consumes charge and no fuel", function()
    resetGameGlobals()
    local function electricWith(names)
        local tractor = newTractor()
        local consumers = {}
        for _, name in ipairs(names) do
            consumers[name] = { fillUnitIndex = 1 }
        end
        tractor.spec_motorized.consumersByFillTypeName = consumers
        return (T.isElectric(tractor))
    end
    eq(electricWith({ "ELECTRICCHARGE" }), true, "battery")
    eq(electricWith({ "ELECTRICCHARGE", "AIR" }), true, "battery with air brakes")
    eq(electricWith({ "electricCharge" }), true, "names are compared in upper case")
    eq(electricWith({ "DIESEL" }), false, "diesel")
    eq(electricWith({ "DIESEL", "DEF", "AIR" }), false)
    eq(electricWith({ "METHANE" }), false, "methane")
    eq(electricWith({ "ELECTRICCHARGE", "DIESEL" }), false, "hybrid: there is an engine")
    eq(electricWith({ "ELECTRICCHARGE", "METHANE" }), false)
    eq(electricWith({ "ELECTRICCHARGE", "DEF" }), false, "exhaust fluid means an engine")
    eq(electricWith({ "ELECTRICCHARGE", "HYDROGEN" }), false, "a fuel this script does not know counts as a fuel")
    eq(electricWith({ "HYDROGEN" }), false)
    eq(electricWith({ "AIR" }), false, "nothing but air: not electric")
    eq(electricWith({}), false, "consumes nothing: not electric")

    -- the hybrid still reports starting
    local hybrid = newTractor()
    hybrid.spec_motorized.consumersByFillTypeName = { ELECTRICCHARGE = {}, DIESEL = {} }
    hybrid.spec_motorized.motorState = MotorState.STARTING
    local _, v, _, info = lineFor(hybrid)
    eq(v.electric, "0") eq(v.starting, "1")
    eq(info.sources.electric, "consumersByFillTypeName (DIESEL,ELECTRICCHARGE)")
end)

test("electric: consumer list fallback, and the key is left out when unknown", function()
    resetGameGlobals()
    local names = { [32] = "DIESEL", [34] = "AIR", [40] = "ELECTRICCHARGE" }
    g_fillTypeManager = { getFillTypeNameByIndex = function(self, index) return names[index] end }

    local tractor = newTractor()
    local spec = tractor.spec_motorized
    spec.consumersByFillTypeName = nil
    spec.consumers = { { fillType = 40 }, { fillType = 34 } }
    local _, v, _, info = lineFor(tractor)
    eq(v.electric, "1") eq(info.sources.electric, "consumers+getFillTypeNameByIndex (AIR,ELECTRICCHARGE)")

    T.resetVehicleCache()
    spec.consumers = { { fillType = 40 }, { fillType = 32 } }
    eq(select(2, lineFor(tractor)).electric, "0", "hybrid through the list")

    T.resetVehicleCache()
    spec.consumers = { { fillType = 40 }, { fillType = 99 } } -- one that has no name
    _, v, _, info = lineFor(tractor)
    eq(v.electric, nil, "cannot name a consumer: unknown, omitted") eq(info.errors.electric, nil)

    g_fillTypeManager = nil
    _, v, _, info = lineFor(tractor)
    eq(v.electric, nil, "no fill type manager: omitted") eq(info.errors.electric, nil)
    eq(v.starting, "0", "starting does not depend on it")

    spec.consumers = nil
    _, v = lineFor(tractor)
    eq(v.electric, nil, "no consumer tables: omitted")

    -- junk in the tables
    spec.consumersByFillTypeName = "diesel"
    spec.consumers = 7
    _, v, _, info = lineFor(tractor)
    eq(v.electric, nil) eq(info.errors.electric, nil)
    spec.consumersByFillTypeName = { [1] = {}, [true] = {}, ELECTRICCHARGE = {} }
    eq(select(2, lineFor(tractor)).electric, "1", "keys that are not names are skipped")

    -- no motor at all (something towed, a conveyor): both keys omitted, no error
    local cart = newTractor()
    cart.spec_motorized = nil
    _, v, _, info = lineFor(cart)
    eq(v.starting, nil) eq(v.electric, nil) eq(v.rpm, nil)
    check(next(info.errors) == nil, "no errors")

    -- a manager that raises costs the key, not the line
    T.resetVehicleCache()
    g_fillTypeManager = { getFillTypeNameByIndex = function() error("manager exploded") end }
    tractor = newTractor()
    tractor.spec_motorized.consumersByFillTypeName = nil
    tractor.spec_motorized.consumers = { { fillType = 40 } }
    _, v, _, info = lineFor(tractor)
    eq(v.electric, nil) check(info.errors.electric ~= nil, "reported to the caller")
    eq(v.starting, "0") eq(v.speed, "5.42")
end)

test("the drive is worked out once per driven vehicle", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local calls = 0
    local isElectric = T.isElectric
    T.isElectric = function(vehicle)
        calls = calls + 1
        return isElectric(vehicle)
    end

    local diesel, battery = newTractor(), newElectricTractor()
    for _ = 1, 100 do
        eq(select(2, lineFor(diesel)).electric, "0")
    end
    eq(calls, 1, "one look for a hundred frames")

    for _ = 1, 100 do
        eq(select(2, lineFor(battery)).electric, "1")
    end
    eq(calls, 2, "another vehicle: looked up again")
    local _, _, _, info = lineFor(battery)
    eq(info.sources.electric, "consumersByFillTypeName (AIR,ELECTRICCHARGE)", "the source is remembered with the answer")

    eq(select(2, lineFor(diesel)).electric, "0", "back in the first one: its own answer, not the last one's")
    eq(calls, 3)

    -- on foot in between: nothing is kept
    T.gather(nil)
    lineFor(diesel)
    eq(calls, 4)
    T.resetVehicleCache()
    lineFor(diesel)
    eq(calls, 5)

    -- "unknown" is not remembered: it is asked again until there is an answer
    local unknown = newTractor()
    unknown.spec_motorized.consumersByFillTypeName = nil
    lineFor(unknown) lineFor(unknown) lineFor(unknown)
    eq(calls, 8)
    unknown.spec_motorized.consumersByFillTypeName = { ELECTRICCHARGE = {} }
    eq(select(2, lineFor(unknown)).electric, "1")
    lineFor(unknown)
    eq(calls, 9)

    T.isElectric = isElectric
    T.resetVehicleCache()
end)

test("idle and maximum engine speed: reported whether or not the engine runs, both or neither", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local tractor = newTractor()
    local spec = tractor.spec_motorized
    local _, v, _, info = lineFor(tractor)
    eq(v.rpmmin, "850") eq(v.rpmmax, "2200") eq(v.rpm, "1450")
    eq(info.sources.rpmrange, "motor:getMinRpm/getMaxRpm (850..2200)")

    -- they describe the machine: the same with the engine off or being started
    for _, state in ipairs({ MotorState.OFF, MotorState.IGNITION, MotorState.STARTING }) do
        spec.motorState = state
        local values = select(2, lineFor(tractor))
        eq(values.rpm, "0") eq(values.rpmmin, "850", "state " .. state) eq(values.rpmmax, "2200")
    end
    spec.motorState = MotorState.ON

    -- an electric vehicle has them too
    T.resetVehicleCache()
    local battery = newTractor()
    battery.spec_motorized.consumersByFillTypeName = { ELECTRICCHARGE = {} }
    battery.spec_motorized.motor.minRpm, battery.spec_motorized.motor.maxRpm = 0, 9000
    _, v = lineFor(battery)
    eq(v.electric, "1") eq(v.rpmmin, "0") eq(v.rpmmax, "9000")

    local function range(change)
        T.resetVehicleCache()
        local vehicle = newTractor()
        change(vehicle, vehicle.spec_motorized.motor)
        local _, values, _, details = lineFor(vehicle)
        check(details.errors.rpmrange == nil, "no error")
        check((values.rpmmin == nil) == (values.rpmmax == nil), "both or neither")
        return values.rpmmin, values.rpmmax, details.sources.rpmrange
    end

    -- fallbacks: the motor through getMotor(), the plain fields, getters that raise
    local low, high, source = range(function(vehicle, motor)
        vehicle.spec_motorized.motor = nil
        vehicle.getMotor = function() return motor end
        vehicle.getMotorRpmReal, vehicle.getMotorLoadPercentage = nil, nil
    end)
    eq(low, "850") eq(high, "2200")
    low, high, source = range(function(_, motor) motor.getMinRpm, motor.getMaxRpm = nil, nil end)
    eq(low, "850") eq(high, "2200") eq(source, "motor.minRpm/maxRpm (850..2200)")
    low, high, source = range(function(_, motor) motor.getMaxRpm = function() error("no") end end)
    eq(low, "850") eq(high, "2200") eq(source, "motor.minRpm/maxRpm (850..2200)")
    low, high = range(function(_, motor) motor.getMinRpm = function() return 900.4 end end)
    eq(low, "900", "whole numbers") eq(high, "2200")

    -- nothing to read, or values that make no sense: both left out
    local nonsense = {
        function(_, motor) motor.getMinRpm, motor.getMaxRpm, motor.minRpm, motor.maxRpm = nil, nil, nil, nil end,
        function(_, motor) motor.getMinRpm, motor.minRpm = nil, nil end, -- only one of the two
        function(_, motor) motor.minRpm, motor.maxRpm = 2200, 850 end,   -- the wrong way round
        function(_, motor) motor.minRpm, motor.maxRpm = 1800, 1800 end,  -- no range
        function(_, motor) motor.minRpm = -100 end,
        function(_, motor) motor.maxRpm = 0 end,
        function(_, motor) motor.maxRpm = 250000 end,                    -- absurd
        function(_, motor) motor.maxRpm = math.huge end,
        function(_, motor) motor.minRpm = 0 / 0 end,
        function(_, motor) motor.minRpm, motor.maxRpm = "850", "2200" end,
        function(vehicle) vehicle.spec_motorized.motor = "motor" end,
        function(vehicle) vehicle.spec_motorized.motor = nil vehicle.getMotor = function() error("no motor") end end,
        function(vehicle) vehicle.spec_motorized = nil end,
    }
    for index, change in ipairs(nonsense) do
        low, high, source = range(change)
        eq(low, nil, "case " .. index) eq(high, nil, "case " .. index) eq(source, nil, "case " .. index)
    end
end)

test("idle and maximum engine speed are read once per driven vehicle", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local calls = 0
    local getRpmRange = T.getRpmRange
    T.getRpmRange = function(vehicle)
        calls = calls + 1
        return getRpmRange(vehicle)
    end

    local first, second = newTractor(), newTractor()
    second.spec_motorized.motor.minRpm, second.spec_motorized.motor.maxRpm = 700, 2500
    for _ = 1, 100 do
        eq(select(2, lineFor(first)).rpmmax, "2200")
    end
    eq(calls, 1)
    local _, v = lineFor(second)
    eq(v.rpmmin, "700") eq(v.rpmmax, "2500", "another vehicle: its own values")
    eq(calls, 2)
    eq(select(2, lineFor(first)).rpmmax, "2200")
    eq(calls, 3)
    T.gather(nil) -- on foot
    lineFor(first)
    eq(calls, 4)

    -- "unknown" is not kept: asked again until there is an answer
    local unknown = newTractor()
    unknown.spec_motorized.motor.maxRpm = 0
    lineFor(unknown) lineFor(unknown)
    eq(calls, 6)
    unknown.spec_motorized.motor.maxRpm = 2100
    eq(select(2, lineFor(unknown)).rpmmax, "2100")
    lineFor(unknown)
    eq(calls, 7)
    T.getRpmRange = getRpmRange
    T.resetVehicleCache()
end)

realPrint("bumps")

local FRAME = 1000 / 60

--- A tractor that has been driving on even ground long enough for the bump
-- detection to have settled.
local function settledTractor()
    resetGameGlobals()
    T.resetVehicleCache()
    T.resetBumpNotes()
    local tractor = newTractor()
    for _ = 1, 90 do
        T.updateBumps(tractor, FRAME)
    end
    return tractor, tractor.spec_wheels.wheels
end

--- One frame: set the load on some wheels (by index), update, read back.
local function bumpFrame(tractor, loads, dt)
    local wheels = tractor.spec_wheels.wheels
    for index, wheel in ipairs(wheels) do
        local base = index <= 2 and 1.8 or 2.4
        wheel.physics.load = base * ((loads and loads[index]) or 1)
    end
    local note = T.updateBumps(tractor, dt or FRAME)
    local left, right = T.getBumps(tractor)
    return left, right, note
end

test("which wheels: the steered wheels of the front-most steered axle, by side", function()
    resetGameGlobals()
    local function pick(vehicle)
        local entries, description = T.findBumpWheels(vehicle)
        if entries == nil then
            return nil
        end
        local left, right = {}, {}
        for _, entry in ipairs(entries) do
            if entry.left then left[#left + 1] = entry.physics.wheelShape end
            if entry.right then right[#right + 1] = entry.physics.wheelShape end
        end
        table.sort(left) table.sort(right)
        return table.concat(left, ",") .. "|" .. table.concat(right, ","), description
    end
    local function physics(vehicle, index)
        return vehicle.spec_wheels.wheels[index].physics
    end
    local function steer(vehicle, index, on)
        local p = physics(vehicle, index)
        p.rotMax, p.rotMin, p.rotSpeed = on and 0.5 or 0, on and -0.5 or 0, on and 1 or 0
    end

    -- an ordinary tractor: front wheels steer
    local tractor = newTractor()
    local sides, description = pick(tractor)
    eq(sides, "1|2") eq(description, "steered wheels (1 left, 1 right)")

    -- the side comes from where the wheel is, not from the isLeft flag (which defaults to true)
    tractor.spec_wheels.wheels[2].isLeft = true
    eq((pick(tractor)), "1|2")

    -- any one of the steering values marks a steered wheel
    tractor = newTractor()
    physics(tractor, 1).rotMax, physics(tractor, 1).rotMin = 0, 0 -- rotSpeed alone
    physics(tractor, 2).rotSpeed = 0 -- limits alone (the game zeroes rotSpeed at times)
    eq((pick(tractor)), "1|2")

    -- four wheel steering: the front pair only
    tractor = newTractor()
    steer(tractor, 3, true) steer(tractor, 4, true)
    eq((pick(tractor)), "1|2")

    -- a combine steers with its rear wheels: those are the steered wheels
    tractor = newTractor()
    steer(tractor, 1, false) steer(tractor, 2, false) steer(tractor, 3, true) steer(tractor, 4, true)
    eq((pick(tractor)), "3|4")

    -- two steered axles (a truck): the front-most one; twin wheels on it all count
    tractor = newTractor()
    for shape = 5, 8 do
        tractor.spec_wheels.wheels[shape] = newWheel(shape)
        steer(tractor, shape, true)
    end
    physics(tractor, 5).positionZ, physics(tractor, 6).positionZ = 0.2, 0.2      -- second steered axle
    physics(tractor, 7).positionZ, physics(tractor, 8).positionZ = 1.45, 1.45    -- outer twins on the front axle
    physics(tractor, 7).positionX, physics(tractor, 8).positionX = 1.4, -1.4
    sides, description = pick(tractor)
    eq(sides, "1,7|2,8") eq(description, "steered wheels (2 left, 2 right)")

    -- nothing steers (the frame bends, or it runs on tracks): the front-most axle
    tractor = newTractor()
    steer(tractor, 1, false) steer(tractor, 2, false)
    sides, description = pick(tractor)
    eq(sides, "1|2") eq(description, "front axle (no steered wheels) (1 left, 1 right)")

    -- a single wheel on the centre line counts for both sides
    tractor = newTractor()
    tractor.spec_wheels.wheels[2] = nil
    physics(tractor, 1).positionX = 0.01
    eq((pick(tractor)), "1|1")

    -- positions through the scene graph are preferred when the game offers them
    tractor = newTractor()
    tractor.rootNode = "root"
    local where = { [1] = { -0.9, 1.4 }, [2] = { 0.9, 1.4 }, [3] = { 0.9, -1.2 }, [4] = { -0.9, -1.2 } }
    for index, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.repr = "repr" .. index
    end
    localToLocal = function(node, root, x, y, z)
        eq(root, "root")
        local index = tonumber(node:match("%d+"))
        return where[index][1], 0.5, where[index][2]
    end
    eq((pick(tractor)), "2|1", "the scene graph says wheel 1 is on the right")
    localToLocal = function() error("no scene graph") end
    eq((pick(tractor)), "1|2", "falls back to the wheel's own position")
    localToLocal = nil

    -- no positions at all: steered wheels are still known, sides from isLeft
    tractor = newTractor()
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.positionX, wheel.physics.positionZ = nil, nil
    end
    eq((pick(tractor)), "1|2")
    -- ... but without steered wheels "front" cannot be told
    steer(tractor, 1, false) steer(tractor, 2, false)
    eq((pick(tractor)), nil)

    -- nothing usable
    eq((pick({})), nil)
    eq((pick({ spec_wheels = { wheels = { "wheel", 7, {}, { physics = "p" }, { physics = {} } } } })), nil)
end)

test("even ground: both stay at 0", function()
    local tractor = settledTractor()
    local highest = 0
    for i = 1, 600 do
        -- the load wanders by a tenth, differently on each wheel
        local left, right = bumpFrame(tractor, { 1 + 0.1 * math.sin(i * 0.7), 1 + 0.1 * math.cos(i * 0.31), 1.05, 0.95 })
        highest = math.max(highest, left, right)
    end
    eq(highest, 0)
    local _, v = lineFor(tractor)
    eq(v.bumpl, "0.00") eq(v.bumpr, "0.00")
end)

test("a step under the left wheel: bumpl jumps, is held, and is back at 0 after 150 ms", function()
    local tractor = settledTractor()
    local left, right = bumpFrame(tractor, { 3 }) -- three times its load for one frame
    near(left, 0.6, 1e-6, "peak: (3.0 - 1.5) / (4.0 - 1.5)")
    eq(right, 0, "the right wheel felt nothing")
    local _, v = lineFor(tractor)
    eq(v.bumpl, "0.60") eq(v.bumpr, "0.00")

    -- the hit is over after one frame; a reader polling at 60 Hz still sees it
    local previous, elapsed = left, 0
    for _ = 1, 8 do
        left, right = bumpFrame(tractor)
        elapsed = elapsed + FRAME
        check(left < previous and left > 0, string.format("falling but still there after %.0f ms: %s", elapsed, tostring(left)))
        near(left, 0.6 * (1 - elapsed / 150), 1e-6, "falls in a straight line")
        eq(right, 0)
        previous = left
    end
    left = bumpFrame(tractor) -- 150 ms after the hit
    check(left < 1e-9, "gone after 150 ms: " .. tostring(left))
    eq((bumpFrame(tractor)), 0)
    eq(select(2, lineFor(tractor)).bumpl, "0.00")

    -- a second, smaller hit while the first is still held does not lower it; a bigger one raises it
    bumpFrame(tractor, { 3 })
    left = bumpFrame(tractor, { 2 })
    near(left, 0.6 * (1 - FRAME / 150), 1e-2, "the smaller hit is under the held value")
    left = bumpFrame(tractor, { 3.5 })
    check(left > 0.7, "the bigger hit takes over: " .. tostring(left))
end)

test("the right wheel, both wheels, and how hard", function()
    local tractor = settledTractor()
    local left, right = bumpFrame(tractor, { 1, 3 })
    eq(left, 0) near(right, 0.6, 1e-6)

    tractor = settledTractor()
    left, right = bumpFrame(tractor, { 3, 3 })
    near(left, 0.6, 1e-6) near(right, 0.6, 1e-6, "both together: both rise")

    -- scale: 1.5 times the load is where it starts, 2.25 the "clear bump" 0.3, 4 and more the full 1
    for _, case in ipairs({ { 1.4, 0 }, { 1.5, 0 }, { 2.25, 0.3 }, { 4, 1 }, { 25, 1 } }) do
        tractor = settledTractor()
        left = bumpFrame(tractor, { case[1] })
        near(left, case[2], 1e-6, "load x" .. case[1])
        local _, v = lineFor(tractor)
        eq(v.bumpl, string.format("%.2f", case[2]))
    end

    -- a rear (not steered) wheel being hit is not reported
    tractor = settledTractor()
    left, right = bumpFrame(tractor, { 1, 1, 5, 5 })
    eq(left, 0) eq(right, 0)

    -- with the driving position turned round, the sides swap
    tractor = settledTractor()
    tractor.spec_reverseDriving = { isReverseDriving = true }
    left, right = bumpFrame(tractor, { 3 })
    eq(left, 0) near(right, 0.6, 1e-6, "the vehicle's left is the driver's right")
    tractor.spec_reverseDriving.isReverseDriving = false
    left, right = T.getBumps(tractor)
    near(left, 0.6, 1e-6) eq(right, 0)
end)

test("standing still nothing counts; crawling over a log does", function()
    local tractor = settledTractor()
    tractor.lastSignedSpeed = 0
    for _ = 1, 60 do
        bumpFrame(tractor)
    end
    local left, right = bumpFrame(tractor, { 4, 4 }) -- an implement drops, the chassis rocks
    eq(left, 0) eq(right, 0)
    tractor.lastSignedSpeed = 0.0002 -- 0.2 m/s: still nothing
    eq((bumpFrame(tractor, { 4 })), 0)

    tractor = settledTractor()
    tractor.lastSignedSpeed = 0.0006 -- 0.6 m/s, about 2 km/h
    for _ = 1, 60 do
        bumpFrame(tractor)
    end
    near((bumpFrame(tractor, { 3 })), 0.6, 1e-6, "crawling")

    tractor = settledTractor()
    tractor.lastSignedSpeed = -0.0008 -- reversing over it
    near((bumpFrame(tractor, { 3 })), 0.6, 1e-6, "in reverse")
end)

test("slow changes of load are not bumps", function()
    -- braking: half as much again on both front wheels, building up over a second
    local tractor = settledTractor()
    local highest = 0
    for i = 1, 240 do
        local factor = 1 + 0.5 * math.min(i / 60, 1)
        local left, right = bumpFrame(tractor, { factor, factor })
        highest = math.max(highest, left, right)
    end
    eq(highest, 0, "weight moving forward under braking")

    -- a heavy implement lifts the front: the wheels carry little, then a normal load again
    tractor = settledTractor()
    for _ = 1, 600 do
        bumpFrame(tractor, { 0.1, 0.1 })
    end
    local left, right = bumpFrame(tractor, { 0.3, 0.3 })
    eq(left, 0, "three times next to nothing is not a hit") eq(right, 0)

    -- off the ground and down again with the load it had: no bump; down hard: a bump
    tractor = settledTractor()
    for _ = 1, 30 do
        bumpFrame(tractor, { 0, 0 })
    end
    left, right = bumpFrame(tractor, { 1, 1 })
    eq(left, 0) eq(right, 0)
    tractor = settledTractor()
    for _ = 1, 30 do
        bumpFrame(tractor, { 0, 0 })
    end
    left = bumpFrame(tractor, { 3, 3 })
    near(left, 0.6, 1e-6, "landing hard")
end)

test("no bumps before the running average has settled, or out of a hitch", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local tractor = newTractor()
    for _ = 1, 5 do
        bumpFrame(tractor)
    end
    eq((bumpFrame(tractor, { 4 })), 0, "83 ms after getting in")
    for _ = 1, 40 do
        bumpFrame(tractor)
    end
    check((bumpFrame(tractor, { 4 })) > 0.9, "after the first half second")

    -- a frame that took two seconds (a save): whatever changed in it is not a jolt
    tractor = settledTractor()
    eq((bumpFrame(tractor, { 4, 4 }, 2000)), 0)
    eq((bumpFrame(tractor)), 0)
    -- odd frame times are harmless
    for _, dt in ipairs({ 0, -5, 0 / 0, math.huge }) do
        local left, right = bumpFrame(tractor, nil, dt)
        check(left == 0 and right == 0, "dt " .. tostring(dt))
    end
    local ok = pcall(T.updateBumps, tractor, nil)
    check(ok, "nil dt")
    eq(T.updateBumps(nil, FRAME), nil, "no vehicle")
end)

test("fallbacks: contact force, then suspension speed; keys left out when there is nothing", function()
    -- no getTireLoad on the wheels: the engine's contact force is used the same way
    resetGameGlobals()
    T.resetVehicleCache()
    local tractor = newTractor()
    local force = { 17.6, 17.6, 23.5, 23.5 }
    getWheelShapeContactForce = function(node, shape) return -force[shape] end -- sign and unit do not matter
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.getTireLoad = nil
    end
    for _ = 1, 90 do
        T.updateBumps(tractor, FRAME)
    end
    force[2] = 17.6 * 3
    T.updateBumps(tractor, FRAME)
    local left, right, source = T.getBumps(tractor)
    eq(left, 0) near(right, 0.6, 1e-6)
    eq(source, "tire load against its running average of the steered wheels (1 left, 1 right)")
    force[2] = 17.6
    getWheelShapeContactForce = function(node, shape) return nil end -- nothing touches: no contact, not an error
    T.updateBumps(tractor, FRAME)

    -- a getTireLoad that raises falls through to the contact force as well
    resetGameGlobals()
    T.resetVehicleCache()
    tractor = newTractor()
    getWheelShapeContactForce = function() return 10 end
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.getTireLoad = function() error("no load today") end
    end
    local _, v, _, info = lineFor(tractor)
    eq(v.bumpl, "0.00") eq(info.errors.bumps, nil)

    -- neither: how fast the suspension moves
    resetGameGlobals()
    T.resetVehicleCache()
    tractor = newTractor()
    local wheels = tractor.spec_wheels.wheels
    for _, wheel in ipairs(wheels) do
        wheel.physics.getTireLoad = nil
    end
    for _ = 1, 90 do
        T.updateBumps(tractor, FRAME)
    end
    left, right, source = T.getBumps(tractor)
    eq(left, 0) eq(right, 0)
    eq(source, "suspension speed of the steered wheels (1 left, 1 right)")
    wheels[1].physics.netInfo.suspensionLength = 0.1 - 0.0125 -- 12.5 mm in one frame: 0.75 m/s
    T.updateBumps(tractor, FRAME)
    left, right = T.getBumps(tractor)
    near(left, 0.5, 1e-6, "(0.75 - 0.25) / (1.25 - 0.25)") eq(right, 0)
    wheels[1].physics.netInfo.suspensionLength = 0.1 -- and back out just as fast: also a jolt
    T.updateBumps(tractor, FRAME)
    near((T.getBumps(tractor)), 0.5, 1e-6)
    for i = 1, 300 do -- ordinary movement, 2 mm a frame (0.12 m/s): nothing
        wheels[1].physics.netInfo.suspensionLength = 0.1 + 0.002 * (i % 2)
        T.updateBumps(tractor, FRAME)
    end
    eq((T.getBumps(tractor)), 0)
    wheels[2].physics.netInfo.suspensionLength = 0.02 -- bottoming out in one frame
    T.updateBumps(tractor, FRAME)
    eq(select(2, T.getBumps(tractor)), 1, "never above 1")

    -- nothing to measure with: both keys left out, and no error
    resetGameGlobals()
    T.resetVehicleCache()
    tractor = newTractor()
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.getTireLoad = nil
        wheel.physics.netInfo.suspensionLength = nil
    end
    T.updateBumps(tractor, FRAME)
    _, v, _, info = lineFor(tractor)
    eq(v.bumpl, nil) eq(v.bumpr, nil) eq(info.errors.bumps, nil)
    eq(info.sources.bumps, "none (neither tire load nor suspension length can be read)")

    -- no wheels
    T.resetVehicleCache()
    tractor = newTractor()
    tractor.spec_wheels = nil
    _, v, _, info = lineFor(tractor)
    eq(v.bumpl, nil) eq(v.bumpr, nil) eq(info.sources.bumps, "none (no usable wheels)")

    -- only a left wheel: only bumpl
    T.resetVehicleCache()
    tractor = newTractor()
    local p2 = tractor.spec_wheels.wheels[2].physics
    p2.rotMax, p2.rotMin, p2.rotSpeed = 0, 0, 0
    _, v = lineFor(tractor)
    eq(v.bumpl, "0.00") eq(v.bumpr, nil, "no steered wheel on the right: left out")
end)

test("multiplayer client: the keys are left out", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local tractor = newTractor()
    tractor.isServer = false
    for _ = 1, 90 do
        T.updateBumps(tractor, FRAME)
    end
    tractor.spec_wheels.wheels[1].physics.load = 10
    T.updateBumps(tractor, FRAME)
    local _, v, _, info = lineFor(tractor)
    eq(v.bumpl, nil) eq(v.bumpr, nil)
    eq(info.sources.bumps, "none (multiplayer client: wheel physics is not local)")
    eq(info.errors.bumps, nil)
end)

test("the wheels are picked once per driven vehicle, and nothing carries over to the next", function()
    resetGameGlobals()
    T.resetVehicleCache()
    local calls = 0
    local findBumpWheels = T.findBumpWheels
    T.findBumpWheels = function(vehicle)
        calls = calls + 1
        return findBumpWheels(vehicle)
    end

    local first, second = newTractor(), newTractor()
    for _ = 1, 300 do
        T.updateBumps(first, FRAME)
        lineFor(first)
    end
    eq(calls, 1, "one look for five seconds of driving")
    first.spec_wheels.wheels[1].physics.load = 1.8 * 3
    T.updateBumps(first, FRAME)
    near((T.getBumps(first)), 0.6, 1e-3)

    -- straight into another vehicle: its own wheels, and not the first one's bump
    T.updateBumps(second, FRAME)
    eq(calls, 2)
    eq((T.getBumps(second)), 0)
    eq(select(2, lineFor(second)).bumpl, "0.00")

    -- "nothing to go on" is remembered as well
    local bare = newTractor()
    bare.spec_wheels = nil
    for _ = 1, 50 do
        T.updateBumps(bare, FRAME)
        lineFor(bare)
    end
    eq(calls, 3)

    -- on foot in between, or a reset: looked up again
    T.gather(nil)
    first.spec_wheels.wheels[1].physics.load = 1.8
    T.updateBumps(first, FRAME)
    eq(calls, 4)
    eq((T.getBumps(first)), 0, "and the held bump is gone with the cache")
    T.findBumpWheels = findBumpWheels
    T.resetVehicleCache()
end)

test("the few notes for the game log: first strong bump, and what was measured on and off a field", function()
    local tractor = settledTractor()
    local notes = {}
    local function frames(count, loads, onField)
        for _ = 1, count do
            local wheels = tractor.spec_wheels.wheels
            wheels[1].physics.load = 1.8 * ((loads and loads[1]) or 1)
            wheels[2].physics.load = 1.8 * ((loads and loads[2]) or 1)
            local note = T.updateBumps(tractor, FRAME, onField)
            if note ~= nil then
                notes[#notes + 1] = note
            end
        end
    end

    frames(1, { 2.5 }) -- 0.4: not worth a note
    eq(#notes, 0)
    frames(10)
    frames(1, { 1, 3 })
    eq(notes[1], "first strong bump: right wheel 0.60 at 5.4 m/s, measured 3.00 (tire load as a multiple of its running average; a bump counts from 1.50, full at 4.00)")
    frames(10)
    frames(1, { 4, 4 })
    eq(#notes, 1, "only the first one")

    -- twenty seconds of driving on a field, then off it: one summary each
    tractor = settledTractor()
    notes = {}
    frames(600, nil, true)
    eq(#notes, 0, "ten seconds: not yet")
    frames(1, { 1.3, 1.1 }, true)
    frames(650, nil, true)
    eq(#notes, 1)
    eq(notes[1], "bump check after 20 s of driving on a field: highest left 1.30, right 1.10 (tire load as a multiple of its running average; a bump counts from 1.50, full at 4.00)")
    frames(1300, nil, true)
    eq(#notes, 1, "once")
    frames(1300, nil, false)
    eq(#notes, 2)
    check(notes[2]:find("^bump check after 20 s of driving off the fields: highest left 1%.%d%d, right 1%.%d%d "), notes[2])
    frames(1300, nil, nil)
    eq(#notes, 2)

    -- standing still does not count towards the twenty seconds
    tractor = settledTractor()
    notes = {}
    tractor.lastSignedSpeed = 0
    frames(2000, nil, true)
    eq(#notes, 0)

    -- with the suspension signal the notes say so
    resetGameGlobals()
    T.resetVehicleCache()
    T.resetBumpNotes()
    tractor = newTractor()
    for _, wheel in ipairs(tractor.spec_wheels.wheels) do
        wheel.physics.getTireLoad = nil
    end
    T.updateBumps(tractor, FRAME)
    tractor.spec_wheels.wheels[1].physics.netInfo.suspensionLength = 0.08
    eq(T.updateBumps(tractor, FRAME), "first strong bump: left wheel 0.95 at 5.4 m/s, measured 1.20 (suspension speed in m/s; a bump counts from 0.25, full at 1.25)")
    T.resetBumpNotes()
end)

realPrint("robustness")

test("an empty vehicle still gives a valid line", function()
    resetGameGlobals()
    local line, v, _, info = lineFor({})
    eq(line, "MOZAFS 1 seq=1 t=913400 active=1 lowered=0 working=0 end=1\n")
    check(next(info.errors) == nil, "an empty vehicle is not an error")
end)

test("a vehicle whose every method raises costs keys, not the line", function()
    resetGameGlobals()
    local tractor = newContractScene()
    for key, value in pairs(tractor) do
        if type(value) == "function" then
            tractor[key] = function() error("boom in " .. key) end
        end
    end
    tractor.spec_motorized.motor.getLastModulatedMotorRpm = function() error("boom") end
    getWheelShapeSlip = function() error("boom in physics") end
    local line, v, _, info = lineFor(tractor)
    eq(v.active, "1")
    eq(v.speed, "5.42", "plain fields still work")
    eq(v.steer, "-0.250")
    eq(v.mass, nil) eq(v.rpm, nil) eq(v.load, nil)
    check(info.errors.mass ~= nil and info.errors.engine ~= nil, "errors are reported to the caller")
    check(v.slip ~= nil, "slip fell back to wheel speed")
end)

test("fields of the wrong type are ignored", function()
    resetGameGlobals()
    local junk = {
        lastSignedSpeed = "fast", movingDirection = {}, rotatedTime = "left", maxRotTime = true, minRotTime = false,
        spec_drivable = "x", spec_motorized = 5, spec_wheels = { wheels = { "wheel", 7, {}, { physics = "p" } } },
        spec_attacherJoints = { attachedImplements = { "a", { object = 3 }, { object = {} } } },
        spec_workArea = { workAreas = { 1, "a", {} } },
        getTotalMass = function() return "heavy" end,
        getLastSpeed = function() return nil end,
        getIsLowered = function() return "maybe" end,
        getIsOnField = function() return 1 end,
    }
    local line, v = lineFor(junk)
    eq(v.active, "1")
    eq(v.speed, nil) eq(v.steer, nil) eq(v.mass, nil) eq(v.slip, nil) eq(v.rough, nil)
end)

test("attachment loops and absurd depth end", function()
    resetGameGlobals()
    local a, b = newImplement({ mass = 1 }), newImplement({ mass = 2 })
    attach(a, b)
    attach(b, a) -- a loop that the game would never build
    a.getTotalMass = function(self, only) return self.ownMass end
    b.getTotalMass = function(self, only) return self.ownMass end
    local kg = T.getMassKg(a)
    eq(kg, 3000)
    local lowered, working, count = T.getImplementState(a, 3)
    eq(count, 1)
end)

test("values that are NaN or infinite in the game never reach the line", function()
    resetGameGlobals()
    local tractor = newContractScene()
    tractor.lastSignedSpeed = 0 / 0
    tractor.lastSpeed = math.huge
    tractor.lastSpeedReal = 0 / 0
    tractor.rotatedTime = 0 / 0
    tractor.spec_drivable.axisSide = math.huge
    tractor.spec_motorized.motor.rpm = math.huge
    tractor.spec_motorized.smoothedLoadPercentage = 0 / 0
    tractor.spec_motorized.motor.getSmoothLoadPercentage = function() return 0 / 0 end
    tractor.ownMass = 0 / 0
    slipByShape[1] = 0 / 0
    local line, v = lineFor(tractor)
    check(not line:lower():find("nan") and not line:lower():find("inf"), line)
    eq(v.speed, nil) eq(v.steer, nil) eq(v.rpm, nil) eq(v.load, nil)
end)

test("findVehicle: FS25 player first, then HUD, then the FS22 name", function()
    resetGameGlobals()
    local car, other = { name = "car" }, { name = "other" }
    g_localPlayer = { getCurrentVehicle = function() return car end }
    g_currentMission.hud.controlledVehicle = other
    local v, how = T.findVehicle()
    eq(v, car) eq(how, "g_localPlayer:getCurrentVehicle")

    g_localPlayer = { getCurrentVehicle = function() return nil end }
    eq((T.findVehicle()), nil, "on foot is a real answer; the HUD value is not consulted")

    g_localPlayer = { getCurrentVehicle = function() error("no") end }
    eq((T.findVehicle()), other, "falls back when the player API fails")

    g_localPlayer = nil
    eq((T.findVehicle()), other, "HUD fallback")
    g_currentMission.hud = nil
    g_currentMission.controlledVehicle = car
    eq((T.findVehicle()), car, "FS22 fallback")
    g_currentMission = nil
    eq((T.findVehicle()), nil, "no mission")
end)

realPrint("write rate")

local function countWrites(fps, seconds, active)
    local state = T.newThrottle()
    local dt = 1000 / fps
    local writes, gapMax, sinceLast = 0, 0, 0
    T.throttleStep(state, dt, active) -- the state-change write
    for _ = 1, math.floor(fps * seconds + 0.5) do
        sinceLast = sinceLast + dt
        if T.throttleStep(state, dt, active) then
            writes = writes + 1
            gapMax = math.max(gapMax, sinceLast)
            sinceLast = 0
        end
    end
    return writes, gapMax
end

test("driving: at most 60 writes a second, every frame below that", function()
    local w60 = countWrites(60, 10, true)
    check(w60 >= 598 and w60 <= 600, "60 fps: " .. w60)
    local w144, gap144 = countWrites(144, 10, true)
    check(w144 >= 595 and w144 <= 600, "144 fps: " .. w144)
    check(gap144 <= 1000 / 20, "144 fps gap " .. gap144)
    local w240 = countWrites(240, 10, true)
    check(w240 >= 595 and w240 <= 600, "240 fps: " .. w240)
    eq((countWrites(30, 10, true)), 300, "30 fps writes every frame")
    eq((countWrites(20, 10, true)), 200, "20 fps writes every frame (the contract's minimum)")
end)

test("not driving: about one write a second", function()
    local w, gap = countWrites(60, 30, false)
    check(w >= 29 and w <= 30, "writes in 30 s: " .. w)
    check(gap <= 1020, "longest gap " .. gap)
end)

test("a change of state writes at once; odd dt values are harmless", function()
    local state = T.newThrottle()
    eq(T.throttleStep(state, 16, false), true, "first frame")
    eq(T.throttleStep(state, 16, false), false)
    eq(T.throttleStep(state, 16, true), true, "entered a vehicle")
    eq(T.throttleStep(state, 5, true), false)
    eq(T.throttleStep(state, 5, false), true, "left the vehicle")
    eq(T.throttleStep(state, 0 / 0, false), false, "NaN dt")
    eq(T.throttleStep(state, -50, false), false, "negative dt")
    eq(T.throttleStep(state, nil, false), false, "nil dt")
    eq(T.throttleStep(state, 60000, false), true, "a very long frame")
    eq(T.throttleStep(state, 16, false), false, "does not bank the long frame")
end)

realPrint("bridge presence rules")

test("bridgeBeat accepts only version 1 with a positive whole beat", function()
    eq(T.bridgeBeat(1, 4821), 4821, "the contract's example")
    eq(T.bridgeBeat(1, 1), 1)
    eq(T.bridgeBeat(1, 2147483647), 2147483647)
    eq(T.bridgeBeat(2, 4821), nil, "another format version")
    eq(T.bridgeBeat(0, 4821), nil) eq(T.bridgeBeat(nil, 4821), nil, "no version") eq(T.bridgeBeat("1", 4821), nil, "version as text")
    eq(T.bridgeBeat(1, 0), nil, "beat is positive") eq(T.bridgeBeat(1, -3), nil) eq(T.bridgeBeat(1, 1.5), nil)
    eq(T.bridgeBeat(1, nil), nil, "no beat") eq(T.bridgeBeat(1, "12"), nil) eq(T.bridgeBeat(1, {}), nil)
    eq(T.bridgeBeat(1, 0 / 0), nil, "NaN") eq(T.bridgeBeat(1, math.huge), nil, "inf")
    eq(T.bridgeBeat(nil, nil), nil)
end)

--- Feed a presence tracker one read per second.
local function reads(state, tokens)
    local present
    for _, token in ipairs(tokens) do
        if token == "none" then
            token = nil
        end
        T.presenceTick(state, 1000)
        present = T.presenceObserve(state, token)
    end
    return present
end

test("present from the moment the beat is seen to change", function()
    local state = T.newPresence()
    eq(state.present, false, "starts absent")
    eq(reads(state, { 10 }), false, "one read proves nothing")
    eq(reads(state, { 12 }), true, "changed: present")
    eq(reads(state, { 14, 16, 18 }), true, "keeps changing")
    eq(reads(state, { 3 }), true, "a beat that went down is still a change (the bridge restarted)")

    -- a file left behind by a crash never changes, so it never counts
    state = T.newPresence()
    eq(reads(state, { 500, 500, 500, 500, 500, 500, 500, 500, 500, 500 }), false, "stale file")

    -- anything that is not a beat is not a change
    state = T.newPresence()
    eq(reads(state, { 1, {}, true, 1 }), false, "junk is not a change")
end)

test("absent again after three seconds without a change, or at once when the file is gone", function()
    local state = T.newPresence()
    reads(state, { 10, 12 })
    eq(reads(state, { 12 }), true, "1 s unchanged")
    eq(reads(state, { 12 }), true, "2 s unchanged")
    eq(reads(state, { 12 }), false, "3 s unchanged")
    eq(reads(state, { 12, 12 }), false, "stays absent")
    eq(reads(state, { 13 }), true, "beating again")

    -- reads that return nothing usable count as "not changed"
    eq(reads(state, { "none", "none" }), true, "2 s of unreadable reads")
    eq(reads(state, { 14 }), true, "and then a change: never dropped")
    eq(reads(state, { "none", "none", "none" }), false, "3 s of unreadable reads")

    -- the file is gone
    state = T.newPresence()
    reads(state, { 10, 12 })
    eq(reads(state, { T.FILE_GONE }), false, "gone: absent at once")
    eq(reads(state, { 50 }), false, "back: one read proves nothing, whatever the beat was before")
    eq(reads(state, { 51 }), true)
end)

test("a change measured against an old read does not count", function()
    -- not present: the two differing reads are too far apart to show the bridge is writing now
    local state = T.newPresence()
    eq(reads(state, { 10, "none", "none", "none", "none", 11 }), false, "five seconds apart")
    eq(reads(state, { 12 }), true, "a second change a second later does")

    -- present, then the game stands still for a minute (no reads, time passes)
    state = T.newPresence()
    reads(state, { 10, 12 })
    T.presenceTick(state, 60000, true)
    eq(T.presenceObserve(state, 12), false, "unchanged across the gap: absent at the first read")

    state = T.newPresence()
    reads(state, { 10, 12 })
    T.presenceTick(state, 60000, true)
    eq(T.presenceObserve(state, 130), true, "changed across the gap: one more read to prove itself")
    eq(reads(state, { 130 }), false, "it did not change again: absent after that one read")

    state = T.newPresence()
    reads(state, { 10, 12 })
    T.presenceTick(state, 60000, true)
    T.presenceObserve(state, 130)
    eq(reads(state, { 132, 134, 134, 134 }), true, "it did: carries on as before, with the full three seconds")
    eq(reads(state, { 134 }), false)
end)

test("presenceTick asks for a read about once a second", function()
    for _, fps in ipairs({ 20, 30, 60, 144, 240 }) do
        local state, dt, due = T.newPresence(), 1000 / fps, 0
        for _ = 1, fps * 20 do
            if T.presenceTick(state, dt) then
                due = due + 1
            end
        end
        check(due >= 20 and due <= 21, fps .. " fps: " .. due .. " reads in 20 s")
    end

    local state = T.newPresence()
    eq(T.presenceTick(state, 16), true, "the first frame reads")
    eq(T.presenceTick(state, 16), false)
    eq(T.presenceTick(state, 16, true), true, "forced")
    eq(T.presenceTick(state, 0 / 0), false, "NaN dt")
    eq(T.presenceTick(state, -500), false, "negative dt")
    eq(T.presenceTick(state, nil), false, "nil dt")
    eq(T.presenceTick(state, 999), false, "not quite a second since the last read")
    eq(T.presenceTick(state, 1), true)
    eq(T.presenceTick(state, 600000), true, "a very long frame")
    eq(T.presenceTick(state, 16), false, "is not banked")
end)

---------------------------------------------------------------------------
-- engine layer: MozaYokeLink.lua with mocked engine functions
---------------------------------------------------------------------------

realPrint("engine layer")

local logs = {}
local listeners = {}
local createdFolders = {}
local profileDir = tmpRoot .. "/profile/"
local telemetryPath = profileDir .. "modSettings/FS25_MozaYokeLink/telemetry.txt"
local bridgePath = profileDir .. "modSettings/FS25_MozaYokeLink/bridge.xml"
local oldBridgePath = profileDir .. "modSettings/FS25_MozaYokeLink/bridge.txt" -- the first presence file; no longer used
local xml -- counters of the mocked XML API, reset by loadMod
local mockXmlClass -- defined below
local clockSec = nil -- wall clock behind the mocked getTimeSec, when a test uses one

local function logText()
    return table.concat(logs, "\n")
end

local function countLogs(pattern)
    local n = 0
    for _, line in ipairs(logs) do
        if line:find(pattern) then
            n = n + 1
        end
    end
    return n
end

--- (Re)load both scripts into a fresh mocked game. opts tweak the mocks.
local function loadMod(opts)
    opts = opts or {}
    realOs.execute('rm -rf "' .. tmpRoot .. '"')
    realOs.execute('mkdir -p "' .. profileDir .. '"')
    logs, listeners, createdFolders = {}, {}, {}

    resetGameGlobals()
    io = realIo
    print = function(...)
        local parts = {}
        for i = 1, select("#", ...) do
            parts[#parts + 1] = tostring((select(i, ...)))
        end
        logs[#logs + 1] = table.concat(parts, "\t")
    end
    addModEventListener = function(listener)
        listeners[#listeners + 1] = listener
    end
    getUserProfileAppPath = function()
        return profileDir
    end
    createFolder = function(path)
        createdFolders[#createdFolders + 1] = path
        realOs.execute('mkdir -p "' .. path .. '"')
    end
    createFile, fileWrite, delete, FileAccess = nil, nil, nil, nil
    g_dedicatedServer, g_dedicatedServerInfo = nil, nil
    g_currentModSettingsDirectory = nil
    getNumOfGamepads, getGamepadName, getGamepadCategory, getHasGamepadAxisForceFeedback, Input = nil, nil, nil, nil, nil
    getTimeSec = nil
    clockSec = nil
    loadXMLFile, getXMLInt = nil, nil
    if opts.noXml then
        fileExists, XMLFile = nil, nil
    else
        mockXmlClass()
    end

    -- the game's input binding class, as Drivable uses it
    local yoke = { deviceName = "MOZA AY210 Base", category = 3, calls = {} }
    function yoke:setForceFeedback(axisIndex, force, position)
        self.calls[#self.calls + 1] = { axisIndex, force, position }
    end
    if opts.noInputBinding then
        InputBinding, g_inputBinding = nil, nil
    else
        InputBinding = {}
        InputBinding.getBindingForceFeedbackInfo = function(self, binding)
            if binding == nil then
                return false, nil, 0
            end
            return true, yoke, 0
        end
        g_inputBinding = setmetatable({}, { __index = InputBinding })
        if opts.noForceFeedbackFunction then
            InputBinding.getBindingForceFeedbackInfo = nil
        end
    end

    dofile(modDir .. "scripts/MozaTelemetry.lua")
    dofile(modDir .. "scripts/MozaYokeLink.lua")
    if opts.suppress ~= nil then
        MozaYokeLink.suppressGameForceFeedback = opts.suppress
    end
    return MozaYokeLink, yoke
end

local function unloadMod()
    print = realPrint
    io = realIo
    realOs.execute('rm -rf "' .. tmpRoot .. '"')
end

local function fileValues()
    local text = readFile(telemetryPath)
    check(text ~= nil, "telemetry file does not exist")
    return checkLine(text), text
end

--- One frame of the mocked game.
local function frame(mod, dt)
    if clockSec ~= nil then
        clockSec = clockSec + dt / 1000
    end
    if g_currentMission.paused ~= true then
        g_currentMission.time = g_currentMission.time + dt
    end
    mod:update(dt)
end

local function drive(mod, vehicle, frames, dt)
    g_localPlayer = { getCurrentVehicle = function() return vehicle end }
    for _ = 1, frames do
        frame(mod, dt or 1000 / 60)
    end
end

--- Let the game run for `ms` without changing who drives what.
local function run(mod, ms, dt)
    dt = dt or 1000 / 60
    for _ = 1, math.floor(ms / dt + 0.5) do
        frame(mod, dt)
    end
end

-- the bridge's side of the presence file
local function writeBridge(text)
    local f = realIo.open(bridgePath, "wb")
    check(f ~= nil, "cannot write " .. bridgePath)
    f:write(text)
    f:close()
end

local function bridgeXml(beat, version)
    return '<?xml version="1.0" encoding="utf-8" standalone="no"?>\n<bridge version="' .. (version or 1) .. '" beat="' .. beat .. '"/>\n'
end

local function writeBeat(beat)
    writeBridge(bridgeXml(beat))
end

local function removeBridge()
    realOs.remove(bridgePath)
end

--- Let the game run for `ms` next to a bridge that beats twice a second.
-- @return the last beat written
local function runWithBridge(mod, ms, beat, dt, write)
    dt = dt or 1000 / 60
    write = write or writeBeat
    local sinceBeat = 500
    for _ = 1, math.floor(ms / dt + 0.5) do
        if sinceBeat >= 500 then
            sinceBeat = sinceBeat - 500
            beat = beat + 1
            write(beat)
        end
        frame(mod, dt)
        sinceBeat = sinceBeat + dt
    end
    return beat
end

--- Start a bridge and run until the mod has noticed it. @return the last beat
local function bridgeUp(mod, beat)
    return runWithBridge(mod, 2600, beat or 100)
end

local steeringBinding = { deviceId = "yoke", axisNames = { "AXIS_1" } }

--- What the game's steering code would be told about force feedback right now.
local function ask(binding)
    return g_inputBinding:getBindingForceFeedbackInfo(binding or steeringBinding)
end

--- The force feedback part of the game's Drivable.actionEventSteer.
local function gameSteeringEvent(vehicle, binding, inputValue)
    local state = vehicle.spec_drivable.forceFeedback
    if not state.isActive then
        local isSupported, device, axisIndex = g_inputBinding:getBindingForceFeedbackInfo(binding)
        state.isActive = isSupported
        state.device = device
        state.binding = binding
        state.axisIndex = axisIndex
        if isSupported then
            device:setForceFeedback(axisIndex, 0, inputValue)
        end
    elseif binding ~= state.binding then
        local isSupported = g_inputBinding:getBindingForceFeedbackInfo(binding)
        if not isSupported then
            state.isActive = false
            state.device = nil
            state.binding = nil
            state.axisIndex = 0
        end
    end
end

--- An io library like the game's own (dataS/scripts/io.lua): open, and file
-- objects with write, close and flush. Whatever mode is asked for, the file is
-- opened for writing, and so emptied.
local function gameLikeIo(calls)
    local lib = {}
    function lib.open(filename, options)
        calls[#calls + 1] = { filename, options }
        local f = realIo.open(filename, "w")
        if f == nil then
            return nil
        end
        local object = {}
        function object:write(...) f:write(...) end
        function object:close() f:close() end
        function object:flush() end
        return object
    end
    return lib
end

-- The game's XML reading, mocked over real files -----------------------------

--- Enough of an XML parser for the presence file: an optional declaration and
-- one empty root element with attributes. nil for anything else, as a failed load.
local function parseXml(text)
    if type(text) ~= "string" then
        return nil
    end
    local body = text:gsub("^%s*<%?xml.-%?>%s*", "")
    local name, attributes = body:match("^<([%a_][%w_]*)(.-)/>%s*$")
    if name == nil then
        return nil
    end
    local values = {}
    for key, value in attributes:gmatch('([%a_][%w_]*)="([^"]*)"') do
        values[key] = value
    end
    return { name = name, attributes = values }
end

--- getXMLInt: nil when the element or attribute is missing or not a number; decimals cut off.
local function xmlInt(doc, path)
    local root, attribute = path:match("^([%w_]+)#([%w_]+)$")
    if root == nil or doc.name ~= root then
        return nil
    end
    local n = tonumber(doc.attributes[attribute])
    if n == nil then
        return nil
    end
    return n >= 0 and math.floor(n) or math.ceil(n)
end

local function mockFileExists()
    fileExists = function(path)
        xml.existsCalls = xml.existsCalls + 1
        xml.existsPaths[path] = true
        local f = realIo.open(path, "rb")
        if f == nil then
            return false
        end
        f:close()
        return true
    end
end

local function resetXmlCounters()
    xml = { existsCalls = 0, existsPaths = {}, loads = 0, loadPaths = {}, open = 0, freed = 0, maxOpen = 0 }
end

local function loaded(path)
    xml.loads = xml.loads + 1
    xml.loadPaths[path] = true
    local doc = parseXml(readFile(path))
    if doc ~= nil then
        xml.open = xml.open + 1
        xml.maxOpen = math.max(xml.maxOpen, xml.open)
    end
    return doc
end

--- The XMLFile class, as mods use it: XMLFile.loadIfExists, xmlFile:getInt, xmlFile:delete.
mockXmlClass = function()
    resetXmlCounters()
    mockFileExists()
    XMLFile = {}
    function XMLFile.loadIfExists(objectName, filename)
        check(type(objectName) == "string" and objectName ~= "", "XMLFile.loadIfExists needs an object name")
        local doc = loaded(filename)
        if doc == nil then
            return nil
        end
        local object, deleted = {}, false
        function object:getInt(path, default)
            check(not deleted, "getInt on a deleted XML file")
            if xml.getIntError then
                error(xml.getIntError)
            end
            local value = xmlInt(doc, path)
            if value == nil then
                return default
            end
            return value
        end
        function object:delete()
            check(not deleted, "XML file deleted twice")
            deleted = true
            xml.open = xml.open - 1
            xml.freed = xml.freed + 1
        end
        return object
    end
end

--- Only the engine functions underneath: loadXMLFile, getXMLInt, delete.
local function mockXmlFunctions()
    resetXmlCounters()
    mockFileExists()
    XMLFile = nil
    local docs, nextHandle = {}, 100
    loadXMLFile = function(objectName, filename)
        local doc = loaded(filename)
        if doc == nil then
            return 0
        end
        nextHandle = nextHandle + 1
        docs[nextHandle] = doc
        return nextHandle
    end
    getXMLInt = function(handle, path)
        check(docs[handle] ~= nil, "getXMLInt on a handle that is not open")
        return xmlInt(docs[handle], path)
    end
    delete = function(handle)
        check(docs[handle] ~= nil, "delete of a handle that is not open")
        docs[handle] = nil
        xml.open = xml.open - 1
        xml.freed = xml.freed + 1
    end
end

test("the script registers one listener and logs its version", function()
    local mod = loadMod()
    eq(#listeners, 1) eq(listeners[1], mod)
    check(type(mod.loadMap) == "function" and type(mod.update) == "function" and type(mod.deleteMap) == "function", "listener functions")
    eq(countLogs("^%[MozaYokeLink%] version " .. mod.VERSION:gsub("%.", "%%.") .. " script loaded$"), 1)
    eq(mod.VERSION, "0.1.2.0", "matches modDesc.xml")
    eq(mod.suppressGameForceFeedback, true, "the switch defaults to on")
    unloadMod()
end)

test("version matches modDesc.xml; modDesc lists the scripts and a real icon", function()
    local mod = loadMod()
    local desc = readFile(modDir .. "modDesc.xml")
    check(desc ~= nil, "modDesc.xml missing")
    eq(desc:match("<version>([%d%.]+)</version>"), mod.VERSION, "version")
    check(desc:match('<modDesc descVersion="%d+">'), "descVersion")
    check(desc:find("<title>", 1, true) and desc:find("<description>", 1, true) and desc:find("<author>", 1, true), "title/description/author")
    check(desc:find('<multiplayer supported="true"/>', 1, true), "multiplayer element")
    local count = 0
    for file in desc:gmatch('<sourceFile filename="([^"]+)"') do
        count = count + 1
        check(readFile(modDir .. file) ~= nil, "listed source file missing: " .. file)
    end
    eq(count, 2, "source files")
    local icon = desc:match("<iconFilename>([^<]+)</iconFilename>")
    check(icon ~= nil, "iconFilename")
    local data = readFile(modDir .. icon)
    check(data ~= nil, "icon file missing")
    eq(data:sub(1, 4), "DDS ", "icon magic")
    eq(data:sub(85, 88), "DXT1", "icon compression")
    eq(#data, 128 + 512 * 512 / 2, "icon size (512x512 DXT1, one mip level)")
    unloadMod()
end)

test("loadMap creates the folder and writes active=0 to the contract path", function()
    local mod = loadMod()
    mod:loadMap("map")
    eq(mod.filePath, telemetryPath, "path")
    eq(createdFolders[#createdFolders], profileDir .. "modSettings/FS25_MozaYokeLink/", "folder")
    local v, text = fileValues()
    eq(text, "MOZAFS 1 seq=1 t=913400 active=0 end=1\n")
    eq(countLogs("telemetry file: .*modSettings/FS25_MozaYokeLink/telemetry%.txt$"), 1)
    eq(countLogs("first write ok %(io%)"), 1)
    unloadMod()
end)

test("driving: the file is one valid line, seq goes up by one per write", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()

    drive(mod, tractor, 1)
    local v, text = fileValues()
    eq(v.active, "1") eq(v.seq, "2", "entering the vehicle writes at once")
    eq(v.speed, "5.42") eq(v.steer, "-0.250") eq(v.mass, "8200") eq(v.rpm, "1450") eq(v.load, "0.62")
    eq(v.slip, "0.04") eq(v.rough, "0.70") eq(v.field, "1") eq(v.lowered, "1") eq(v.working, "1")

    local lastSeq = 2
    for _ = 1, 120 do
        drive(mod, tractor, 1)
        local now = tonumber((fileValues()).seq)
        check(now == lastSeq or now == lastSeq + 1, "seq jumped from " .. lastSeq .. " to " .. now)
        lastSeq = now
    end
    check(lastSeq >= 2 + 118 and lastSeq <= 2 + 120, "about 60 writes a second, got seq " .. lastSeq)
    eq(countLogs("driving 'Mock Tractor 7R' %(found via g_localPlayer:getCurrentVehicle%)"), 1, "vehicle logged once")
    check(logText():find("speed<-lastSignedSpeed; steer<-rotatedTime", 1, true), "sources logged")
    unloadMod()
end)

test("starting and electric reach the file and the one-time sources line", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    local spec = tractor.spec_motorized
    spec.motorState = MotorState.STARTING
    drive(mod, tractor, 5)
    local v = fileValues()
    eq(v.starting, "1") eq(v.electric, "0") eq(v.rpm, "0")
    spec.motorState = MotorState.ON
    drive(mod, tractor, 5)
    v = fileValues()
    eq(v.starting, "0") eq(v.electric, "0") eq(v.rpm, "1450")
    check(logText():find("engine<-getMotorRpmReal/getMotorLoadPercentage/(engine not running: reported as 0); rpmrange<-motor:getMinRpm/getMaxRpm (850..2200); starting<-getMotorState; electric<-consumersByFillTypeName (AIR,DEF,DIESEL); slip<-", 1, true),
        "sources line: " .. logText())

    -- another vehicle: its own answer, worked out once, and logged for it
    local calls = 0
    local isElectric = MozaTelemetry.isElectric
    MozaTelemetry.isElectric = function(vehicle)
        calls = calls + 1
        return isElectric(vehicle)
    end
    local battery = newContractScene()
    battery.spec_motorized.consumersByFillTypeName = { ELECTRICCHARGE = {}, AIR = {} }
    battery.spec_motorized.motorState = MotorState.STARTING
    drive(mod, battery, 300)
    v = fileValues()
    eq(v.electric, "1") eq(v.starting, "0", "never starting when electric") eq(v.rpm, "0")
    eq(calls, 1, "one look in five seconds of driving")
    check(logText():find("starting<-electric: never; electric<-consumersByFillTypeName (AIR,ELECTRICCHARGE);", 1, true), "sources line for the electric vehicle")

    drive(mod, tractor, 60)
    eq((fileValues()).electric, "0", "back in the diesel")
    eq(calls, 2)

    -- on foot, then the same vehicle again; and a new savegame: nothing is carried over
    drive(mod, nil, 5)
    eq((fileValues()).electric, nil, "not driving: the keys are not written")
    drive(mod, tractor, 5)
    eq(calls, 3)
    mod:deleteMap()
    mod:loadMap("map")
    drive(mod, tractor, 5)
    eq(calls, 4, "the cache does not outlive the savegame")
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

test("bumps are looked for on every frame, reach the file, and leave their notes in the log", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    local front = tractor.spec_wheels.wheels[1].physics
    drive(mod, tractor, 90)
    local v = fileValues()
    eq(v.bumpl, "0.00") eq(v.bumpr, "0.00")
    check(logText():find("; bumps<-tire load against its running average of the steered wheels (1 left, 1 right)", 1, true), "sources line")

    front.load = 1.8 * 3 -- the left front wheel hits something for one frame
    drive(mod, tractor, 1)
    front.load = 1.8
    v = fileValues()
    eq(v.bumpl, "0.60") eq(v.bumpr, "0.00")
    drive(mod, tractor, 4)
    local held = tonumber((fileValues()).bumpl)
    check(held > 0.2 and held < 0.5, "still held four frames later: " .. held)
    drive(mod, tractor, 6)
    eq((fileValues()).bumpl, "0.00", "gone after 150 ms")
    eq(countLogs("^%[MozaYokeLink%] first strong bump: left wheel 0%.60 at 5%.4 m/s, measured 3%.00 %(tire load as a multiple of its running average; a bump counts from 1%.50, full at 4%.00%)$"), 1)

    -- at 240 fps only every fourth frame writes; a hit on a frame in between is not lost
    local fast = 1000 / 240
    drive(mod, tractor, 240, fast)
    front.load = 1.8 * 3
    drive(mod, tractor, 1, fast)
    front.load = 1.8
    drive(mod, tractor, 4, fast)
    held = tonumber((fileValues()).bumpl)
    check(held > 0.45 and held <= 0.6, "seen by the next write: " .. held)
    drive(mod, tractor, 60, fast)

    -- straight into another vehicle while a bump is held: it does not come along
    front.load = 1.8 * 6
    drive(mod, tractor, 1)
    front.load = 1.8
    eq((fileValues()).bumpl, "1.00")
    local other = newContractScene()
    drive(mod, other, 1)
    eq((fileValues()).bumpl, "0.00")

    -- twenty seconds on the field: one summary line, and only one strong-bump line in all
    drive(mod, other, 1300)
    eq(countLogs("^%[MozaYokeLink%] bump check after 20 s of driving on a field: highest left %d%.%d%d, right %d%.%d%d %("), 1)
    drive(mod, other, 1300)
    eq(countLogs("bump check after 20 s"), 1)
    eq(countLogs("first strong bump"), 1)

    -- paused: nothing is updated, and the hitch afterwards is not a bump
    g_currentMission.paused = true
    other.spec_wheels.wheels[2].physics.load = 1.8 * 4
    drive(mod, other, 30)
    g_currentMission.paused = false
    other.spec_wheels.wheels[2].physics.load = 1.8
    drive(mod, other, 2)
    eq((fileValues()).bumpr, "0.00")

    -- a new savegame logs its notes again; a failure in the detection is contained
    mod:deleteMap()
    mod:loadMap("map")
    drive(mod, tractor, 90)
    front.load = 1.8 * 3
    drive(mod, tractor, 1)
    front.load = 1.8
    eq(countLogs("first strong bump"), 2)
    MozaTelemetry.updateBumps = function() error("bumps exploded") end
    drive(mod, tractor, 120)
    eq(countLogs("problem: bumps: .*bumps exploded"), 1)
    eq((fileValues()).active, "1", "telemetry carries on")
    unloadMod()
end)

test("on foot: active=0 about once a second; t follows game time", function()
    local mod = loadMod()
    mod:loadMap("map")
    drive(mod, nil, 600) -- ten seconds at 60 fps
    local v = fileValues()
    eq(v.active, "0")
    local seq = tonumber(v.seq)
    check(seq >= 10 and seq <= 12, "seq after ten seconds on foot: " .. seq)
    check(tonumber(v.t) > 913400 + 8900, "t advanced: " .. v.t)

    -- leaving the vehicle is reported at once
    local tractor = newContractScene()
    drive(mod, tractor, 30)
    eq((fileValues()).active, "1")
    drive(mod, nil, 1)
    eq((fileValues()).active, "0", "left the vehicle")
    unloadMod()
end)

test("paused: nothing is written, so the reader sees the telemetry go stale", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    drive(mod, tractor, 30)
    local before = (fileValues()).seq
    g_currentMission.paused = true
    drive(mod, tractor, 300)
    eq((fileValues()).seq, before, "seq while paused")
    g_currentMission.paused = false
    drive(mod, tractor, 2)
    check(tonumber((fileValues()).seq) > tonumber(before), "resumes")
    unloadMod()
end)

test("a vehicle that raises everywhere never reaches the game and does not flood the log", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    for key, value in pairs(tractor) do
        if type(value) == "function" then
            tractor[key] = function() error("boom in " .. key) end
        end
    end
    local logsBefore = #logs
    drive(mod, tractor, 600)
    local v = fileValues()
    eq(v.active, "1")
    check(tonumber(v.seq) > 500, "still writing")
    check(#logs - logsBefore <= 16, "log lines for 600 bad frames: " .. (#logs - logsBefore))
    check(countLogs("problem: mass: ") == 1, "each problem is logged once")

    -- even a broken player object or a broken telemetry module is contained
    g_localPlayer = setmetatable({}, { __index = function() error("player exploded") end })
    mod:update(16)
    MozaTelemetry.gather = function() error("gather exploded") end
    g_localPlayer = { getCurrentVehicle = function() return tractor end }
    for _ = 1, 10 do
        mod:update(1000 / 60)
    end
    mod:update("not a number")
    eq(countLogs("problem: update: .*player exploded"), 1)
    eq(countLogs("problem: update: .*gather exploded"), 1)
    unloadMod()
end)

test("without io.open the engine's createFile/fileWrite is used", function()
    local mod = loadMod()
    local written = {}
    io = nil
    FileAccess = { READ = 1, WRITE = 2 }
    createFile = function(path, access)
        eq(access, FileAccess.WRITE, "access")
        return { path = path }
    end
    fileWrite = function(handle, text)
        written[#written + 1] = { handle.path, text }
    end
    local deleted = 0
    delete = function(handle) deleted = deleted + 1 end

    mod:loadMap("map")
    drive(mod, newContractScene(), 10)
    check(#written >= 10, "writes through the engine API: " .. #written)
    eq(deleted, #written, "every handle is released")
    eq(written[1][1], telemetryPath)
    checkLine(written[#written][2])
    eq(countLogs("first write ok %(engine%)"), 1)
    unloadMod()
end)

test("no way to write at all: no error, one log line, keeps retrying quietly", function()
    local mod = loadMod()
    io = { open = function() return nil, "denied" end }
    mod:loadMap("map")
    local logsBefore = #logs
    drive(mod, newContractScene(), 1200)
    eq(countLogs("problem: cannot write "), 1)
    check(#logs - logsBefore <= 8, "log lines: " .. (#logs - logsBefore))
    eq(mod.seq, 0, "seq only counts writes that happened")

    -- and it recovers when writing becomes possible
    io = realIo
    drive(mod, newContractScene(), 120)
    check(mod.seq > 0, "recovered")
    eq((fileValues()).active, "1")
    unloadMod()
end)

test("a write that fails now and then (reader has the file open) is just skipped", function()
    local mod = loadMod()
    mod:loadMap("map")
    local n = 0
    io = { open = function(path, mode)
        n = n + 1
        if n % 3 == 0 then
            return nil, "sharing violation"
        end
        return realIo.open(path, mode)
    end }
    drive(mod, newContractScene(), 300)
    eq(countLogs("problem: cannot write"), 0)
    local v = fileValues()
    check(tonumber(v.seq) > 150, "most writes went through: " .. v.seq)
    unloadMod()
end)

test("without getUserProfileAppPath the mod's own settings folder is used", function()
    loadMod() -- prepares the temp folders and mocks
    getUserProfileAppPath = nil
    g_currentModSettingsDirectory = profileDir .. "modSettings/FS25_MozaYokeLink" -- only set while scripts load
    dofile(modDir .. "scripts/MozaYokeLink.lua")
    local mod = MozaYokeLink
    g_currentModSettingsDirectory = nil
    mod:loadMap("map")
    eq(mod.filePath, telemetryPath)
    eq((fileValues()).active, "0")
    unloadMod()

    -- neither available: nothing to write to, said once, no error
    loadMod()
    getUserProfileAppPath = nil
    dofile(modDir .. "scripts/MozaYokeLink.lua")
    mod = MozaYokeLink
    mod:loadMap("map")
    drive(mod, newContractScene(), 100)
    eq(mod.filePath, nil)
    eq(countLogs("problem: no user profile path"), 1)
    unloadMod()
end)

test("deleteMap leaves the file saying active=0", function()
    local mod = loadMod()
    mod:loadMap("map")
    drive(mod, newContractScene(), 30)
    eq((fileValues()).active, "1")
    mod:deleteMap()
    eq((fileValues()).active, "0")
    -- and nothing is written after the map is gone
    local seq = (fileValues()).seq
    drive(mod, newContractScene(), 30)
    eq((fileValues()).seq, seq)
    unloadMod()
end)

test("dedicated server: disabled, nothing written", function()
    local mod = loadMod()
    g_dedicatedServer = {}
    mod:loadMap("map")
    drive(mod, newContractScene(), 30)
    eq(readFile(telemetryPath), nil, "no file on a dedicated server")
    eq(countLogs("dedicated server, telemetry disabled"), 1)
    unloadMod()
end)

realPrint("bridge presence and the force feedback override")

local DETECTED = "^%[MozaYokeLink%] bridge detected: suppressing the game's steering force feedback$"
local GONE = "^%[MozaYokeLink%] bridge gone: the game's steering force feedback is back in its hands$"

--- Every XML file that was loaded has been freed again.
local function checkHandles()
    eq(xml.open, 0, "XML files left open")
    check(xml.maxOpen <= 1, "more than one XML file open at a time")
end

test("no bridge: every call goes to the game's function and its answer comes back unchanged", function()
    local mod, yoke = loadMod()
    local seen = {}
    local original = function(self, binding, extra)
        seen[#seen + 1] = { self, binding, extra }
        if binding == nil then
            return false, nil, 0
        end
        return true, yoke, 3, "fourth value"
    end
    InputBinding.getBindingForceFeedbackInfo = original

    mod:loadMap("map")
    check(InputBinding.getBindingForceFeedbackInfo ~= original, "wrapper installed")
    eq(countLogs("force feedback override installed on InputBinding class; the game keeps its steering force feedback until the bridge shows up in .*modSettings/FS25_MozaYokeLink/bridge%.xml %(checked with XMLFile%.loadIfExists%)$"), 1)
    eq(countLogs("suppress"), 0, "nothing claims suppression at load")

    drive(mod, newContractScene(), 600) -- ten seconds, no bridge.xml
    local a, b, c, d = g_inputBinding:getBindingForceFeedbackInfo(steeringBinding, "extra argument")
    eq(a, true) eq(b, yoke) eq(c, 3) eq(d, "fourth value")
    eq(select("#", g_inputBinding:getBindingForceFeedbackInfo(steeringBinding)), 4, "every return value is handed back")
    local last = seen[#seen - 1]
    eq(last[1], g_inputBinding, "self") eq(last[2], steeringBinding, "binding") eq(last[3], "extra argument", "extra arguments")
    a, b, c = g_inputBinding:getBindingForceFeedbackInfo(nil)
    eq(a, false) eq(b, nil) eq(c, 0)
    eq(select("#", g_inputBinding:getBindingForceFeedbackInfo(nil)), 3)

    -- the game's function is called exactly once per call, also after the first 200
    local before = #seen
    for _ = 1, 500 do
        ask()
    end
    eq(#seen, before + 500)

    eq(countLogs(DETECTED), 0) eq(countLogs(GONE), 0)
    eq(countLogs("game asked about steering force feedback: device 'MOZA AY210 Base' category=3 axis=3 supported=true %-> left to the game %(no bridge%)$"), 1)
    eq(countLogs("game asked about steering force feedback"), 2, "one line per distinct answer, not per call")

    -- no file: fileExists is asked about once a second and the XML loader never
    check(xml.existsCalls >= 10 and xml.existsCalls <= 11, "fileExists calls in ten seconds: " .. xml.existsCalls)
    eq(xml.loads, 0, "no XML load is attempted for a file that is not there")
    eq(readFile(bridgePath), nil, "the mod does not create bridge.xml")
    eq(countLogs("problem:"), 0)
    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original, "restored")
    unloadMod()
end)

test("bridge beating: suppressed from the second read, logged once", function()
    local mod, yoke = loadMod()
    local original = InputBinding.getBindingForceFeedbackInfo
    mod:loadMap("map")

    writeBeat(100)
    run(mod, 50)
    eq((ask()), true, "one read of the file proves nothing yet")
    eq(countLogs(DETECTED), 0)

    local beat = runWithBridge(mod, 1100, 100)
    local a, b, c = ask()
    eq(a, false, "isSupported") eq(b, nil, "device") eq(c, 0, "axisIndex")
    eq(select("#", ask()), 3, "the shape the game expects: three values")
    eq(countLogs(DETECTED), 1)
    eq(mod.isSuppressing(), true)

    beat = runWithBridge(mod, 20000, beat)
    eq((ask()), false, "still suppressed after twenty seconds")
    eq(countLogs(DETECTED), 1, "one line per transition, nothing per second")
    eq(countLogs(GONE), 0)
    eq(countLogs("game asked about steering force feedback: device 'MOZA AY210 Base' category=3 axis=0 supported=true %-> suppressed %(bridge running%)$"), 1)
    for _ = 1, 500 do
        ask()
    end
    eq(countLogs("game asked about steering force feedback"), 2, "one line while passed through, one while suppressed")
    eq(readFile(bridgePath), bridgeXml(beat), "the mod leaves bridge.xml alone")
    checkHandles()
    check(xml.freed >= 21 and xml.freed <= 23, "one load per second, each freed: " .. xml.freed)

    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original, "original restored on unload, bridge or not")
    eq((ask()), true)
    eq(countLogs("game force feedback override removed"), 1)
    unloadMod()
end)

test("beat frozen for three seconds: back to the game, logged once; a returning bridge is picked up again", function()
    local mod = loadMod()
    mod:loadMap("map")
    local beat = bridgeUp(mod)
    eq((ask()), false)

    run(mod, 2400) -- the bridge has stopped writing; its file is still there
    eq((ask()), false, "2.4 s without a change: still suppressed")
    eq(countLogs(GONE), 0)
    run(mod, 1700)
    eq((ask()), true, "more than three seconds (plus one read): the game's answer again")
    eq(countLogs(GONE), 1)
    eq(mod.isSuppressing(), false)

    run(mod, 30000) -- the stale file stays
    eq((ask()), true)
    eq(countLogs(GONE), 1) eq(countLogs(DETECTED), 1)
    eq(countLogs("bridge%.xml is there but its beat is not changing"), 1, "one hint about the stale file")

    runWithBridge(mod, 2600, beat + 50)
    eq((ask()), false, "the bridge is back")
    eq(countLogs(DETECTED), 2)
    eq(countLogs("problem:"), 0)
    checkHandles()
    unloadMod()
end)

test("file disappears: back to the game at the next read", function()
    local mod = loadMod()
    mod:loadMap("map")
    bridgeUp(mod)
    eq((ask()), false)
    removeBridge() -- the bridge stopped cleanly
    local loads = xml.loads
    run(mod, 1100)
    eq((ask()), true, "no three second wait when the file is gone")
    eq(countLogs(GONE), 1)
    run(mod, 10000)
    eq(countLogs(GONE), 1) eq(countLogs(DETECTED), 1)
    eq(countLogs("is there but"), 0)
    eq(xml.loads, loads, "fileExists says no: the XML loader is not asked")
    checkHandles()
    unloadMod()
end)

test("a file left behind by a crash never counts", function()
    local mod = loadMod()
    mod:loadMap("map")
    writeBeat(4821)
    drive(mod, newContractScene(), 1800) -- thirty seconds
    eq((ask()), true)
    eq(countLogs(DETECTED), 0) eq(countLogs(GONE), 0)
    eq(countLogs("bridge%.xml is there but its beat is not changing %(left over from an earlier run of the bridge%): the bridge counts as not running$"), 1)
    eq(readFile(bridgePath), bridgeXml(4821), "left as it was")
    checkHandles()
    unloadMod()
end)

test("files that are not a version 1 presence file are ignored", function()
    local mod = loadMod()
    mod:loadMap("map")
    local junk = {
        function(n) return bridgeXml(n, 2) end,                                  -- another format version
        function(n) return '<bridge beat="' .. n .. '"/>\n' end,                 -- no version
        function(n) return '<bridge version="1"/>\n' end,                        -- no beat
        function(n) return '<bridge version="1" beat="0"/>\n' end,               -- beat is positive
        function(n) return '<bridge version="1" beat="-' .. n .. '"/>\n' end,
        function(n) return '<bridge version="1" beat="beat' .. n .. '"/>\n' end, -- not a number
        function(n) return '<other version="1" beat="' .. n .. '"/>\n' end,      -- another root element
        function(n) return '<bridge version="1" beat="' .. n .. '"' end,         -- not well formed: the load fails
        function(n) return "MOZAFS-BRIDGE 1 beat=" .. n .. " end=1\n" end,       -- the text format of 0.1.1 drafts
        function(n) return "" end,
    }
    local function writeJunk(n)
        writeBridge(junk[n % #junk + 1](n))
    end

    -- junk that changes all the time is not a beat
    local n = runWithBridge(mod, 12000, 0, nil, writeJunk)
    eq((ask()), true)
    eq(countLogs(DETECTED), 0)
    eq(countLogs("bridge%.xml is there but XMLFile%.loadIfExists gets no beat from it %(not version 1 of the bridge's format, or not readable%): the bridge counts as not running$"), 1)

    -- each kind on its own, long enough for several reads
    for kind = 1, #junk do
        runWithBridge(mod, 3000, 0, nil, function(b)
            writeBridge(junk[kind](b + 1000))
        end)
        eq((ask()), true, "kind " .. kind)
    end
    eq(countLogs(DETECTED), 0)

    -- a real bridge whose file then turns to junk: "no beat" for three seconds, then absent
    local beat = bridgeUp(mod, 500)
    eq((ask()), false)
    n = runWithBridge(mod, 2400, n, nil, writeJunk)
    eq((ask()), false, "unusable for 2.4 s: still present")
    runWithBridge(mod, 1700, n, nil, writeJunk)
    eq((ask()), true, "unusable for more than three seconds")
    eq(countLogs(GONE), 1)

    -- half the reads unusable, the others showing a change: stays present
    beat = bridgeUp(mod, beat)
    local flip = 0
    runWithBridge(mod, 20000, beat, nil, function(b)
        flip = flip + 1
        if flip % 4 < 2 then
            writeBeat(b)
        else
            writeJunk(b)
        end
    end)
    eq((ask()), false)
    eq(countLogs(DETECTED), 2) eq(countLogs(GONE), 1)
    eq(countLogs("problem:"), 0)
    eq(countLogs("is there but"), 1, "the hint is given once")
    checkHandles()
    unloadMod()
end)

test("the XML handle is freed whatever happens while reading it", function()
    local mod = loadMod()
    mod:loadMap("map")
    local beat = bridgeUp(mod)
    eq((ask()), false)
    checkHandles()

    xml.getIntError = "getInt exploded"
    local freed = xml.freed
    beat = runWithBridge(mod, 5000, beat)
    check(xml.freed >= freed + 4, "still loading once a second: " .. (xml.freed - freed))
    checkHandles()
    eq((ask()), true, "no beat for more than three seconds")
    eq(countLogs("problem:"), 0, "a read that fails is not an error")

    xml.getIntError = nil
    runWithBridge(mod, 2600, beat)
    eq((ask()), false, "and reading works again afterwards")
    checkHandles()

    -- the loader itself raising, or fileExists raising: "no beat", nothing left open
    local loadIfExists = XMLFile.loadIfExists
    XMLFile.loadIfExists = function() error("load exploded") end
    run(mod, 4100)
    eq((ask()), true)
    XMLFile.loadIfExists = loadIfExists
    beat = bridgeUp(mod, beat + 10)
    eq((ask()), false)
    fileExists = function() error("fileExists exploded") end
    run(mod, 1500)
    eq((ask()), false, "cannot tell whether the file is there: not the same as gone")
    run(mod, 3000)
    eq((ask()), true, "but no beat for three seconds is")
    eq(countLogs("problem:"), 0)
    checkHandles()
    unloadMod()
end)

test("a file that never yields a beat is given up on after a minute, until the next savegame load", function()
    local mod = loadMod()
    mod:loadMap("map")
    writeBridge(bridgeXml(77, 2))
    run(mod, 300000, 50) -- five minutes
    check(xml.loads >= 59 and xml.loads <= 61, "XML loads in five minutes: " .. xml.loads)
    eq(countLogs("bridge%.xml is there but XMLFile%.loadIfExists gets no beat from it"), 1)
    eq(countLogs("no longer checking bridge%.xml in this session"), 1)
    eq((ask()), true)
    checkHandles()
    mod:deleteMap()

    local loads = xml.loads
    mod:loadMap("map") -- the next savegame load tries again
    runWithBridge(mod, 5000, 100, 50)
    check(xml.loads >= loads + 5, "tried again after reloading")
    eq((ask()), false, "and finds a bridge that has been fixed in the meantime")
    unloadMod()
end)

test("switch off: never suppressed, even with a live bridge, and bridge.xml is not even looked at", function()
    local mod, yoke = loadMod({ suppress = false })
    local original = InputBinding.getBindingForceFeedbackInfo
    mod:loadMap("map")
    eq(InputBinding.getBindingForceFeedbackInfo, original, "the game's function is not even wrapped")
    eq(countLogs("game force feedback left alone %(suppressGameForceFeedback = false%)"), 1)

    -- a vehicle with the game's force feedback active is not touched either
    local tractor = newContractScene()
    tractor.spec_drivable.forceFeedback = { isActive = true, device = yoke, axisIndex = 0, intensity = 1 }
    g_localPlayer = { getCurrentVehicle = function() return tractor end }
    runWithBridge(mod, 10000, 100)
    eq((ask()), true)
    eq(tractor.spec_drivable.forceFeedback.isActive, true)
    eq(#yoke.calls, 0)
    eq(xml.loads, 0, "XML loads") eq(xml.existsCalls, 0, "fileExists calls")
    eq(countLogs(DETECTED), 0)
    eq(mod.isSuppressing(), false)
    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original)
    unloadMod()
end)

test("bridge.xml is read about once a second whatever the frame rate", function()
    for _, fps in ipairs({ 20, 60, 240 }) do
        local mod = loadMod()
        mod:loadMap("map")
        local tractor = newContractScene()
        g_localPlayer = { getCurrentVehicle = function() return tractor end }
        runWithBridge(mod, 10000, 100, 1000 / fps)
        check(xml.loads >= 10 and xml.loads <= 11, fps .. " fps: " .. xml.loads .. " loads in 10 s")
        eq(xml.existsCalls, xml.loads, fps .. " fps: one fileExists per load")
        eq(xml.freed, xml.loads, fps .. " fps: every load freed")
        checkHandles()
        eq((ask()), false, fps .. " fps: bridge detected")
        local lastSeq = tonumber((fileValues()).seq)
        check(lastSeq > 10 * math.min(fps, 60) - 15, fps .. " fps: telemetry still written every frame: " .. lastSeq)
        unloadMod()
    end
end)

test("paused: the mod keeps following the bridge while the game keeps calling update", function()
    local mod = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    g_localPlayer = { getCurrentVehicle = function() return tractor end }
    local beat = bridgeUp(mod)
    local seq = (fileValues()).seq

    g_currentMission.paused = true
    beat = runWithBridge(mod, 10000, beat)
    eq((ask()), false, "bridge beating through the pause: still suppressed")
    eq((fileValues()).seq, seq, "no telemetry while paused")
    eq(countLogs(GONE), 0)

    run(mod, 4100) -- the bridge is closed while the game is paused
    eq((ask()), true, "noticed during the pause")
    eq(countLogs(GONE), 1)

    -- and one that starts during the pause is noticed too
    runWithBridge(mod, 2600, beat + 10)
    eq((ask()), false)
    eq(countLogs(DETECTED), 2)
    g_currentMission.paused = false
    unloadMod()
end)

test("the game stood still (update not called): old knowledge lasts for one read at most", function()
    local function start()
        local mod = loadMod()
        clockSec = 5000
        getTimeSec = function() return clockSec end
        mod:loadMap("map")
        local beat = bridgeUp(mod)
        eq((ask()), false)
        return mod, beat
    end

    -- the bridge was closed during the standstill and took its file with it
    local mod, beat = start()
    removeBridge()
    clockSec = clockSec + 60
    run(mod, 17) -- a single frame
    eq((ask()), true, "file gone: noticed on the first frame")
    eq(countLogs(GONE), 1)
    unloadMod()

    -- the bridge died right when the standstill began: same beat as at the last read
    mod, beat = start()
    run(mod, 600)
    writeBeat(beat) -- unchanged
    clockSec = clockSec + 60
    run(mod, 17)
    eq((ask()), true, "unchanged for a minute of real time: noticed on the first frame, not after three more seconds")
    eq(countLogs(GONE), 1)
    unloadMod()

    -- the bridge wrote a few more beats, then died during the standstill
    mod, beat = start()
    writeBeat(beat + 7)
    clockSec = clockSec + 60
    run(mod, 17)
    eq((ask()), false, "changed since the last read: it gets one more read")
    run(mod, 1100)
    eq((ask()), true, "and did not change again")
    eq(countLogs(GONE), 1)
    unloadMod()

    -- the bridge ran on through the standstill: nothing happens at all
    mod, beat = start()
    clockSec = clockSec + 60
    beat = runWithBridge(mod, 5000, beat + 120)
    eq((ask()), false)
    eq(countLogs(GONE), 0) eq(countLogs(DETECTED), 1)
    unloadMod()

    -- an ordinary long frame (a save, a stutter) is not a standstill
    mod, beat = start()
    for _ = 1, 20 do
        clockSec = clockSec + 0.9 -- 0.9 s hitches the game reports as short frames
        beat = runWithBridge(mod, 500, beat)
    end
    eq((ask()), false)
    eq(countLogs(GONE), 0)
    unloadMod()

    -- without a clock only frame time counts: three seconds of frames after the standstill
    mod = loadMod()
    mod:loadMap("map")
    bridgeUp(mod)
    run(mod, 2400)
    eq((ask()), false)
    run(mod, 1700)
    eq((ask()), true)
    unloadMod()
end)

test("the game's io.open (write only) is never used on the presence file", function()
    local mod, yoke = loadMod()
    local calls = {}
    io = gameLikeIo(calls)
    mod:loadMap("map")
    eq(countLogs("first write ok %(io%)"), 1, "telemetry is still written through the game's io.open")

    local tractor = newContractScene()
    g_localPlayer = { getCurrentVehicle = function() return tractor end }
    run(mod, 3000)
    eq(readFile(bridgePath), nil, "no bridge.xml appears out of nowhere")
    local beat = bridgeUp(mod)
    eq((ask()), false)
    beat = runWithBridge(mod, 10000, beat)
    eq(readFile(bridgePath), bridgeXml(beat), "bridge.xml is intact")
    removeBridge()
    run(mod, 1100)
    eq((ask()), true)

    check(#calls > 500, "telemetry writes went through io.open: " .. #calls)
    for _, call in ipairs(calls) do
        eq(call[2], "w", "io.open mode")
        eq(call[1], telemetryPath, "io.open target")
    end
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

test("the presence file of the first draft (bridge.txt) is not read, created or deleted", function()
    local mod = loadMod()
    mod:loadMap("map")
    local f = realIo.open(oldBridgePath, "wb")
    f:write("MOZAFS-BRIDGE 1 beat=1 end=1\n")
    f:close()
    for beat = 2, 12 do
        f = realIo.open(oldBridgePath, "wb")
        f:write("MOZAFS-BRIDGE 1 beat=" .. beat .. " end=1\n")
        f:close()
        run(mod, 500)
    end
    eq((ask()), true, "a beating bridge.txt means nothing")
    eq(countLogs(DETECTED), 0)
    eq(readFile(oldBridgePath), "MOZAFS-BRIDGE 1 beat=12 end=1\n", "left as it was")
    eq(xml.existsPaths[oldBridgePath], nil, "not even asked about")
    eq(xml.loadPaths[oldBridgePath], nil)
    eq(xml.existsPaths[bridgePath], true, "bridge.xml is what is asked about")
    mod:deleteMap()
    eq(readFile(oldBridgePath), "MOZAFS-BRIDGE 1 beat=12 end=1\n", "still there after unload")
    unloadMod()
end)

test("without the XMLFile class the engine's own XML functions are used", function()
    local mod = loadMod()
    mockXmlFunctions()
    mod:loadMap("map")
    eq(countLogs("force feedback override installed on InputBinding class; .*bridge%.xml %(checked with loadXMLFile%)$"), 1)
    run(mod, 3000)
    eq(xml.loads, 0, "no file: no load")
    local beat = bridgeUp(mod)
    eq((ask()), false)
    eq(countLogs(DETECTED), 1)
    beat = runWithBridge(mod, 10000, beat)
    checkHandles()
    eq(xml.freed, xml.loads, "every handle deleted")

    writeBridge(bridgeXml(beat + 1, 2)) -- another version: ignored
    run(mod, 4100)
    eq((ask()), true)
    writeBridge("<bridge") -- load fails: handle 0
    run(mod, 3000)
    checkHandles()
    bridgeUp(mod, beat + 10)
    eq((ask()), false)
    removeBridge()
    run(mod, 1100)
    eq((ask()), true, "file gone")
    eq(countLogs(GONE), 2)
    eq(countLogs("problem:"), 0)
    checkHandles()
    unloadMod()
end)

test("no way to read XML: said once at load, the game keeps its force feedback", function()
    local mod = loadMod({ noXml = true })
    local original = InputBinding.getBindingForceFeedbackInfo
    mod:loadMap("map")
    eq(countLogs("force feedback override installed on InputBinding class, but there is no way to read .*bridge%.xml here %(no fileExists%): the game keeps its steering force feedback$"), 1)
    runWithBridge(mod, 8000, 100)
    eq((ask()), true)
    eq(countLogs(DETECTED), 0)
    eq(countLogs("problem:"), 0)
    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original)
    unloadMod()

    -- fileExists but neither the class nor the functions
    mod = loadMod()
    XMLFile = nil
    mod:loadMap("map")
    eq(countLogs("but there is no way to read .*bridge%.xml here %(no XML functions%)"), 1)
    runWithBridge(mod, 8000, 100)
    eq((ask()), true)
    eq(xml.existsCalls, 0, "nothing to read with, so the file is not asked about either")
    unloadMod()
end)

test("a failure while checking for the bridge hands force feedback back to the game", function()
    local mod = loadMod()
    mod:loadMap("map")
    bridgeUp(mod)
    eq((ask()), false)
    MozaTelemetry.presenceObserve = function() error("observe exploded") end
    run(mod, 1100)
    eq((ask()), true)
    eq(countLogs(GONE), 1)
    eq(countLogs("problem: bridge presence: .*observe exploded"), 1)
    drive(mod, newContractScene(), 300)
    eq((fileValues()).active, "1", "telemetry carries on")
    eq(countLogs("problem: bridge presence"), 1)
    checkHandles()
    unloadMod()
end)

test("the function or the class missing is harmless and logged", function()
    local mod = loadMod({ noForceFeedbackFunction = true })
    mod:loadMap("map")
    eq(countLogs("game force feedback NOT suppressed: InputBinding.getBindingForceFeedbackInfo not found"), 1)
    g_localPlayer = { getCurrentVehicle = function() return newContractScene() end }
    runWithBridge(mod, 5000, 100)
    eq((fileValues()).active, "1", "telemetry carries on")
    eq(xml.loads, 0, "no override, so no reason to read bridge.xml")
    eq(xml.existsCalls, 0)
    eq(countLogs(DETECTED), 0)
    mod:deleteMap()
    unloadMod()

    mod = loadMod({ noInputBinding = true })
    mod:loadMap("map")
    eq(countLogs("game force feedback NOT suppressed"), 1)
    runWithBridge(mod, 5000, 100)
    mod:deleteMap()
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

test("only the instance is reachable: overridden there and cleanly removed", function()
    local mod, yoke = loadMod()
    local class = InputBinding
    InputBinding = nil -- the class table is not visible, g_inputBinding still inherits from it
    mod:loadMap("map")
    eq(countLogs("force feedback override installed on g_inputBinding instance"), 1)
    eq((ask()), true, "passes through without the bridge")
    bridgeUp(mod)
    eq((ask()), false)
    mod:deleteMap()
    eq(rawget(g_inputBinding, "getBindingForceFeedbackInfo"), nil, "instance field removed")
    eq((ask()), true, "class method answers again")
    eq(class.getBindingForceFeedbackInfo ~= nil, true)
    unloadMod()
end)

test("another script wrapped the override: it is not clobbered and passes through after unload", function()
    local mod, yoke = loadMod()
    local original = InputBinding.getBindingForceFeedbackInfo
    mod:loadMap("map")
    local ours = InputBinding.getBindingForceFeedbackInfo
    local wrapperCalls = 0
    local wrapper = function(self, binding)
        wrapperCalls = wrapperCalls + 1
        return ours(self, binding)
    end
    InputBinding.getBindingForceFeedbackInfo = wrapper
    local beat = bridgeUp(mod)
    eq((ask()), false)

    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, wrapper, "the other script's function is still installed")
    local a, b, c = ask()
    eq(a, true, "passes through to the game after unload") eq(b, yoke) eq(c, 0)
    eq(countLogs("left in place as a pass%-through"), 1)

    -- and it stays a pass-through when the next savegame loads with the bridge still running
    mod:loadMap("map")
    beat = bridgeUp(mod, beat)
    eq((ask()), false, "the new wrapper suppresses")
    mod:deleteMap()
    eq((ask()), true, "both old wrappers pass through")
    check(wrapperCalls > 0, "the other script's wrapper was used")
    unloadMod()
end)

test("the game's force feedback on the vehicle is released when the bridge arrives, and comes back by itself", function()
    local mod, yoke = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    tractor.spec_drivable.forceFeedback = { isActive = false, device = nil, binding = nil, axisIndex = 0, intensity = 1 }

    -- no bridge: the game switches its force feedback on at the first steering event, as without the mod
    drive(mod, tractor, 30)
    gameSteeringEvent(tractor, steeringBinding, 0.3)
    eq(tractor.spec_drivable.forceFeedback.isActive, true)
    eq(tractor.spec_drivable.forceFeedback.device, yoke)
    eq(#yoke.calls, 1)
    drive(mod, tractor, 300)
    eq(tractor.spec_drivable.forceFeedback.isActive, true, "the mod does not interfere")
    eq(#yoke.calls, 1, "and sends the device nothing")

    -- the bridge starts: what the game had going is released, the way the game does on leaving a vehicle
    tractor.spec_drivable.forceFeedback.axisIndex = 2
    local beat = bridgeUp(mod)
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    eq(tractor.spec_drivable.forceFeedback.device, nil)
    eq(#yoke.calls, 2, "released once")
    eq(yoke.calls[2][1], 2, "axis") eq(yoke.calls[2][2], 0, "force") eq(yoke.calls[2][3], 0, "position")
    eq(countLogs("had the game's steering force feedback active; switched it off"), 1)

    -- while suppressed, steering events do not switch it back on
    for _ = 1, 20 do
        beat = runWithBridge(mod, 500, beat)
        gameSteeringEvent(tractor, steeringBinding, 0.3)
    end
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    eq(#yoke.calls, 2, "nothing is sent to the device while suppressed")

    -- the bridge stops: the mod does nothing to the vehicle or the device ...
    removeBridge()
    run(mod, 1100)
    eq(countLogs(GONE), 1)
    eq(#yoke.calls, 2)
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    -- ... and the game's next steering event switches its force feedback back on; no re-entry needed
    gameSteeringEvent(tractor, steeringBinding, -0.1)
    eq(tractor.spec_drivable.forceFeedback.isActive, true)
    eq(tractor.spec_drivable.forceFeedback.device, yoke)
    eq(#yoke.calls, 3)
    eq(yoke.calls[3][3], -0.1, "the game's own call")

    -- a vehicle entered while the bridge runs, with force feedback active from before: released as well
    local other = newContractScene()
    other.spec_drivable.forceFeedback = { isActive = true, device = yoke, binding = steeringBinding, axisIndex = 0, intensity = 1 }
    bridgeUp(mod, beat + 10)
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    drive(mod, other, 2)
    eq(other.spec_drivable.forceFeedback.isActive, false)

    -- a device without the method, or junk in the field, is no problem
    other.spec_drivable.forceFeedback = { isActive = true, device = "junk" }
    drive(mod, other, 1)
    eq(other.spec_drivable.forceFeedback.isActive, false)
    other.spec_drivable.forceFeedback = "junk"
    drive(mod, other, 1)
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

test("input devices and force feedback axes are logged once, on first entering a vehicle", function()
    local mod = loadMod()
    getNumOfGamepads = function() return 2 end
    getGamepadName = function(id) return id == 0 and "MOZA AY210 Base" or "Farm Panel" end
    getGamepadCategory = function(id) return id == 0 and 3 or 6 end
    getHasGamepadAxisForceFeedback = function(id, axis) return id == 0 and (axis == 0 or axis == 1) end
    Input = { MAX_NUM_AXES = 14 }
    mod:loadMap("map")
    eq(countLogs("input device"), 0, "not before a vehicle is entered")

    local tractor = newContractScene()
    drive(mod, tractor, 200)
    eq(countLogs("input device 0: 'MOZA AY210 Base' category=3 forceFeedbackAxes=0,1$"), 1)
    eq(countLogs("input device 1: 'Farm Panel' category=6 forceFeedbackAxes=none$"), 1)
    eq(countLogs("vehicle spec_drivable%.forceFeedback: isActive=false axisIndex=0 intensity=1$"), 1)

    -- another vehicle later: the vehicle line again, the devices not
    local second = newContractScene()
    drive(mod, second, 50)
    eq(countLogs("input device 0"), 1)
    eq(countLogs("driving 'Mock Tractor 7R'"), 2)
    unloadMod()
end)

test("device diagnostics cope with the engine functions missing or raising", function()
    local mod = loadMod()
    mod:loadMap("map")
    drive(mod, newContractScene(), 3)
    eq(countLogs("input devices: none reported"), 1)
    unloadMod()

    mod = loadMod()
    getNumOfGamepads = function() return 1 end
    getGamepadName = function() error("no name") end
    getHasGamepadAxisForceFeedback = function() error("no ffb") end
    mod:loadMap("map")
    local tractor = newContractScene()
    tractor.spec_drivable = nil
    drive(mod, tractor, 3)
    eq(countLogs("input device 0: .* forceFeedbackAxes=none$"), 1)
    eq(countLogs("vehicle spec_drivable%.forceFeedback: not present"), 1)
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

---------------------------------------------------------------------------
-- source lint: the mod has to stay loadable by the game's Lua
---------------------------------------------------------------------------

realPrint("source lint")

--- Blank out comments and string contents so tokens can be searched for.
local function stripCommentsAndStrings(src)
    local out, i, n = {}, 1, #src
    while i <= n do
        local c = src:sub(i, i)
        local two = src:sub(i, i + 1)
        if two == "--" then
            local level = src:match("^%-%-%[(=*)%[", i)
            if level then
                local _, stop = src:find("]" .. level .. "]", i, true)
                i = (stop or n) + 1
            else
                local stop = src:find("\n", i, true) or (n + 1)
                i = stop
            end
        elseif c == '"' or c == "'" then
            local j = i + 1
            while j <= n do
                local d = src:sub(j, j)
                if d == "\\" then
                    j = j + 2
                elseif d == c then
                    break
                else
                    j = j + 1
                end
            end
            out[#out + 1] = c .. c
            i = j + 1
        elseif src:match("^%[=*%[", i) then
            local level = src:match("^%[(=*)%[", i)
            local _, stop = src:find("]" .. level .. "]", i, true)
            out[#out + 1] = '""'
            i = (stop or n) + 1
        else
            out[#out + 1] = c
            i = i + 1
        end
    end
    return table.concat(out)
end

for _, file in ipairs({ "scripts/MozaTelemetry.lua", "scripts/MozaYokeLink.lua" }) do
    test(file .. " uses only Lua 5.1 syntax and no library the game lacks", function()
        local src = readFile(modDir .. file)
        check(src ~= nil, "cannot read " .. file)
        check(not src:find("[^\9\10\32-\126]"), "non-ASCII character in source")
        local code = stripCommentsAndStrings(src)
        local forbidden = {
            { "//", "integer division" }, { "<<", "shift" }, { ">>", "shift" }, { "&", "bitwise and" },
            { "|", "bitwise or" }, { "~[^=]", "bitwise not / xor" }, { "%f[%w_]goto%f[^%w_]", "goto" },
            { "::", "label" }, { "<%s*const%s*>", "<const>" }, { "<%s*close%s*>", "<close>" },
            { "%f[%w_]os%.", "os library" }, { "%f[%w_]require%f[^%w_]", "require" },
            { "%f[%w_]dofile%f[^%w_]", "dofile" }, { "%f[%w_]loadfile%f[^%w_]", "loadfile" },
            { "%f[%w_]debug%.", "debug library" }, { "%f[%w_]io%.read", "io.read" },
            { "%f[%w_]io%.write", "io.write" }, { "%f[%w_]io%.lines", "io.lines" },
            { "table%.unpack", "table.unpack (5.2+)" }, { "math%.tointeger", "math.tointeger (5.3+)" },
            { "utf8%.", "utf8 library (5.3+)" }, { "%f[%w_]continue%f[^%w_]", "continue (Luau only)" },
            { "[%+%-%*/%%]=", "compound assignment (Luau only)" },
        }
        for _, rule in ipairs(forbidden) do
            local at = code:find(rule[1])
            if at then
                local _, lineNo = code:sub(1, at):gsub("\n", "\n")
                error(string.format("%s uses %s near line %d", file, rule[2], lineNo + 1))
            end
        end
    end)
end

---------------------------------------------------------------------------

print = realPrint
io = realIo
realOs.execute('rm -rf "' .. tmpRoot .. '"')

realPrint("")
if failed > 0 then
    for _, text in ipairs(failures) do
        realPrint("FAILED: " .. text .. "\n")
    end
end
realPrint(string.format("%d passed, %d failed", passed, failed))
realOs.exit(failed == 0 and 0 or 1)
