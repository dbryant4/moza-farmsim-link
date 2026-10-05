--
-- Tests for FS25_MozaYokeLink, run under plain Lua with mocked game globals:
--
--     lua fs-mod/tests/run_tests.lua
--
-- Covers the telemetry line (docs/telemetry.md), the gathering code against a
-- mocked tractor with implements, the write-rate limiter, the engine layer
-- (file output, pause, failure handling) and the force feedback override.
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

local INT_KEYS = { seq = true, t = true, mass = true, rpm = true }
local FLAG_KEYS = { active = true, field = true, lowered = true, working = true, ["end"] = true }
local RANGES = {
    steer = { -1, 1 }, load = { 0, 1 }, slip = { 0, 1 }, rough = { 0, 1 },
    speed = { -200, 200 },
}
local KNOWN_ORDER = { "seq", "t", "active", "speed", "steer", "mass", "rpm", "load", "slip", "rough", "field", "lowered", "working", "end" }

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
    slipByShape = {}
    getWheelShapeSlip = function(node, shape)
        return slipByShape[shape] or 0, 0
    end
end

local function newWheel(shape, opts)
    opts = opts or {}
    return {
        node = 1000 + shape,
        physics = {
            wheelShape = shape,
            wheelShapeCreated = true,
            hasGroundContact = opts.hasGroundContact ~= false,
            contact = opts.contact or WheelContactType.GROUND,
            densityType = opts.densityType or FieldGroundType.NONE,
            groundDepth = opts.groundDepth or 0,
            hasSnowContact = opts.hasSnowContact or false,
            radius = opts.radius or 0.8,
            netInfo = { xDriveSpeed = opts.xDriveSpeed or (5.42 / 0.8), slip = opts.netSlip or 0 },
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
        motorStarted = true,
        spec_drivable = { axisSide = -0.25, steeringDirection = 1, forceFeedback = { isActive = false, axisIndex = 0, intensity = 1 } },
        spec_motorized = {
            motor = {
                rpm = 1450,
                getLastModulatedMotorRpm = function(self) return self.rpm end,
                getLastMotorRpm = function(self) return self.rpm end,
                getSmoothLoadPercentage = function() return 0.62 end,
            },
            smoothedLoadPercentage = 0.62,
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
    function v:getIsMotorStarted() return self.motorStarted end
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

local contractExample = "MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 slip=0.04 rough=0.70 field=1 lowered=1 working=1 end=1"

test("the example in docs/telemetry.md is the one tested here", function()
    local doc = readFile(contractFile)
    if doc == nil then
        realPrint("        (docs/telemetry.md not found, using the embedded copy)")
        return
    end
    check(doc:find(contractExample, 1, true), "docs/telemetry.md no longer contains the example line this test expects")
end)

test("formatLine reproduces the contract example byte for byte", function()
    local line = T.formatLine(4821, 913400, {
        active = true, speed = 5.42, steer = -0.25, mass = 8200, rpm = 1450, load = 0.62,
        slip = 0.04, rough = 0.7, field = true, lowered = true, working = true,
    })
    eq(line, contractExample .. "\n")
    checkLine(line)
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
    eq(line, contractExample .. "\n")
    check(next(info.errors) == nil, "unexpected gather errors")
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

    tractor.motorStarted = false
    _, v = lineFor(tractor)
    eq(v.rpm, "0") eq(v.load, "0.00")

    tractor.motorStarted = true
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
        return tonumber(v.rough), v.field
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

    -- two wheels on the road, two on the field: half way, and still "on the field"
    setSurface(tractor, { densityType = FieldGroundType.SOWN, groundDepth = 1 })
    for shape = 1, 2 do
        local p = tractor.spec_wheels.wheels[shape].physics
        p.contact, p.densityType, p.groundDepth = WheelContactType.OBJECT, FieldGroundType.NONE, 0
    end
    local _, v = lineFor(tractor)
    eq(v.rough, "0.35") eq(v.field, "1")

    -- a field state this script has no entry for still reads as a field
    FieldGroundType.SOMETHING_NEW = 99
    eq((surface({ densityType = 99, groundDepth = 1 })), 0.7, "unknown field state")

    -- snow and heaps
    eq((surface({ contact = WheelContactType.GROUND_HEIGHT, hasSnowContact = true })), 0.4, "snow")
    eq((surface({ contact = WheelContactType.GROUND_HEIGHT })), 0.6, "heap")

    -- all wheels in the air: rough omitted, field falls back to the vehicle
    setSurface(tractor, { contact = WheelContactType.NONE })
    _, v = lineFor(tractor)
    eq(v.rough, "0.70", "falls back to getIsOnField") eq(v.field, "1")
    tractor.getIsOnField = function() return false end
    _, v = lineFor(tractor)
    eq(v.rough, nil, "unknown surface: rough omitted") eq(v.field, "0")
end)

test("surface works without the FS25 enums (older wheel layout)", function()
    resetGameGlobals()
    local tractor = newTractor()
    WheelContactType = nil
    FieldGroundType = nil
    local _, v = lineFor(tractor)
    -- only the terrain depth is left to go on: soft ground
    eq(v.rough, "0.50") eq(v.field, "0")
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

---------------------------------------------------------------------------
-- engine layer: MozaYokeLink.lua with mocked engine functions
---------------------------------------------------------------------------

realPrint("engine layer")

local logs = {}
local listeners = {}
local createdFolders = {}
local profileDir = tmpRoot .. "/profile/"
local telemetryPath = profileDir .. "modSettings/FS25_MozaYokeLink/telemetry.txt"

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

local function drive(mod, vehicle, frames, dt)
    g_localPlayer = { getCurrentVehicle = function() return vehicle end }
    for _ = 1, frames do
        g_currentMission.time = g_currentMission.time + (dt or 1000 / 60)
        mod:update(dt or 1000 / 60)
    end
end

local originalFfbFunction

test("the script registers one listener and logs its version", function()
    local mod = loadMod()
    eq(#listeners, 1) eq(listeners[1], mod)
    check(type(mod.loadMap) == "function" and type(mod.update) == "function" and type(mod.deleteMap) == "function", "listener functions")
    eq(countLogs("^%[MozaYokeLink%] version " .. mod.VERSION:gsub("%.", "%%.") .. " script loaded$"), 1)
    eq(mod.VERSION, "0.1.0.0", "matches modDesc.xml")
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

realPrint("force feedback override")

test("switch on: the game is told that steering has no force feedback", function()
    local mod, yoke = loadMod()
    local original = InputBinding.getBindingForceFeedbackInfo
    local binding = { deviceId = "yoke", axisNames = { "AXIS_1" } }
    local a, b, c = g_inputBinding:getBindingForceFeedbackInfo(binding)
    eq(a, true, "before loadMap the game's own answer stands") eq(b, yoke)

    mod:loadMap("map")
    check(InputBinding.getBindingForceFeedbackInfo ~= original, "override installed")
    a, b, c = g_inputBinding:getBindingForceFeedbackInfo(binding)
    eq(a, false, "isSupported") eq(b, nil, "device") eq(c, 0, "axisIndex")
    eq(select("#", g_inputBinding:getBindingForceFeedbackInfo(binding)), 3, "same shape as the original: three values")
    eq(countLogs("game force feedback suppressed: getBindingForceFeedbackInfo overridden on InputBinding class"), 1)

    -- the first call also logs what the game itself would have answered
    eq(countLogs("game asked about steering force feedback: device 'MOZA AY210 Base' category=3 axis=0 supported=true %-> suppressed"), 1)
    for _ = 1, 500 do
        g_inputBinding:getBindingForceFeedbackInfo(binding)
    end
    eq(countLogs("game asked about steering force feedback"), 1, "logged once, not per call")

    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original, "original restored on unload")
    a, b = g_inputBinding:getBindingForceFeedbackInfo(binding)
    eq(a, true) eq(b, yoke)
    eq(countLogs("game force feedback override removed"), 1)
    unloadMod()
end)

test("switch off: the game's function is left alone", function()
    local mod, yoke = loadMod({ suppress = false })
    local original = InputBinding.getBindingForceFeedbackInfo
    mod:loadMap("map")
    eq(InputBinding.getBindingForceFeedbackInfo, original)
    eq((g_inputBinding:getBindingForceFeedbackInfo({})), true)
    eq(countLogs("game force feedback left alone %(suppressGameForceFeedback = false%)"), 1)

    -- and a vehicle with active game force feedback is not touched
    local tractor = newContractScene()
    tractor.spec_drivable.forceFeedback = { isActive = true, device = yoke, axisIndex = 0, intensity = 1 }
    drive(mod, tractor, 5)
    eq(tractor.spec_drivable.forceFeedback.isActive, true)
    eq(#yoke.calls, 0)
    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, original)
    unloadMod()
end)

test("the function or the class missing is harmless and logged", function()
    local mod = loadMod({ noForceFeedbackFunction = true })
    mod:loadMap("map")
    eq(countLogs("game force feedback NOT suppressed: InputBinding.getBindingForceFeedbackInfo not found"), 1)
    drive(mod, newContractScene(), 5)
    eq((fileValues()).active, "1", "telemetry carries on")
    mod:deleteMap()
    unloadMod()

    mod = loadMod({ noInputBinding = true })
    mod:loadMap("map")
    eq(countLogs("game force feedback NOT suppressed"), 1)
    drive(mod, newContractScene(), 5)
    mod:deleteMap()
    eq(countLogs("problem:"), 0)
    unloadMod()
end)

test("only the instance is reachable: overridden there and cleanly removed", function()
    local mod, yoke = loadMod()
    local class = InputBinding
    InputBinding = nil -- the class table is not visible, g_inputBinding still inherits from it
    mod:loadMap("map")
    eq(countLogs("overridden on g_inputBinding instance"), 1)
    eq((g_inputBinding:getBindingForceFeedbackInfo({})), false)
    mod:deleteMap()
    eq(rawget(g_inputBinding, "getBindingForceFeedbackInfo"), nil, "instance field removed")
    eq((g_inputBinding:getBindingForceFeedbackInfo({})), true, "class method answers again")
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
    eq((g_inputBinding:getBindingForceFeedbackInfo({})), false)

    mod:deleteMap()
    eq(InputBinding.getBindingForceFeedbackInfo, wrapper, "the other script's function is still installed")
    local a, b, c = g_inputBinding:getBindingForceFeedbackInfo({})
    eq(a, true, "passes through to the game after unload") eq(b, yoke) eq(c, 0)
    eq(countLogs("left in place as a pass%-through"), 1)
    unloadMod()
end)

test("a vehicle with the game's force feedback already active is switched off", function()
    local mod, yoke = loadMod()
    mod:loadMap("map")
    local tractor = newContractScene()
    tractor.spec_drivable.forceFeedback = { isActive = true, device = yoke, binding = {}, axisIndex = 2, intensity = 1 }
    drive(mod, tractor, 3)
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    eq(tractor.spec_drivable.forceFeedback.device, nil)
    eq(#yoke.calls, 1, "the device was released once")
    eq(yoke.calls[1][1], 2, "axis") eq(yoke.calls[1][2], 0, "force") eq(yoke.calls[1][3], 0, "position")
    eq(countLogs("had the game's steering force feedback active; switched it off"), 1)

    -- a device without the method, or junk in the field, is no problem
    tractor.spec_drivable.forceFeedback = { isActive = true, device = "junk" }
    drive(mod, tractor, 1)
    eq(tractor.spec_drivable.forceFeedback.isActive, false)
    tractor.spec_drivable.forceFeedback = "junk"
    drive(mod, tractor, 1)
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
