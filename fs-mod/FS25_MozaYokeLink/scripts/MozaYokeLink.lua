--
-- MozaYokeLink.lua  --  engine-facing half of FS25_MozaYokeLink.
--
-- Every frame of a running game it finds the vehicle the local player is
-- driving, asks MozaTelemetry.lua for the values, and rewrites
--   <user profile>/modSettings/FS25_MozaYokeLink/telemetry.txt
-- for the bridge that drives the MOZA yoke (contract: docs/telemetry.md).
--
-- While the bridge is running on this machine it also stops Farming
-- Simulator from sending its own steering force feedback to the device, so
-- the game and the bridge do not fight over the yoke's motor. The bridge
-- announces itself through bridge.xml in the same folder (same contract,
-- "Presence file: bridge -> mod"). Without the bridge the game's force
-- feedback is left exactly as it is. See suppressGameForceFeedback below.
--
-- Nothing here may raise into the game: update() runs under pcall and
-- problems are logged a limited number of times.
--

MozaYokeLink = {}

---------------------------------------------------------------------------
-- SWITCH: keep Farming Simulator from driving the steering device's motor.
--
-- true  (default): while the bridge is running on this machine, the game is
--                  told that no steering binding supports force feedback, so
--                  it sends none. Without the bridge nothing is changed.
-- false:           the game's own steering force feedback is always left
--                  alone, bridge or no bridge.
---------------------------------------------------------------------------
MozaYokeLink.suppressGameForceFeedback = true

-- Keep in step with <version> in modDesc.xml (the tests check this).
MozaYokeLink.VERSION = "0.1.1.0"

-- Fixed by the telemetry contract; deliberately not derived from the mod's
-- file name.
MozaYokeLink.SETTINGS_FOLDER = "FS25_MozaYokeLink"
MozaYokeLink.FILE_NAME = "telemetry.txt"
MozaYokeLink.BRIDGE_FILE_NAME = "bridge.xml"

MozaYokeLink.LOG_PREFIX = "[MozaYokeLink] "

local MAX_LOGGED_ERRORS = 10
local MAX_LOGGED_TRANSITIONS = 40
-- reads in a row that find bridge.xml but get no beat from it, before giving up
local MAX_UNUSABLE_READS = 60
local MAX_FF_AXES = 16
-- update() not having run for this long (ms of wall clock beyond the frame's
-- own dt) means the game stood still; see Y.gapSinceLastUpdateMs
local UPDATE_GAP_MS = 1500

local Y = MozaYokeLink

Y.seq = 0
Y.elapsedMs = 0
Y.filePath = nil
Y.writeMethod = nil
Y.writeFailures = 0
Y.retryInMs = 0
Y.loggedErrors = 0
Y.loggedMessages = {}
Y.lastVehicle = nil
Y.devicesLogged = false
Y.throttle = nil
Y.enabled = false
Y.ffb = { installed = false }
Y.bridgePath = nil
Y.bridgeReader = nil
Y.bridgeReaderName = nil
Y.presence = nil
Y.lastClockSec = nil
Y.unchangedReads = 0
Y.unusableReads = 0
Y.loggedTransitions = 0

-- These two globals only exist while the game is loading the mod's scripts.
Y.modSettingsDirAtLoad = g_currentModSettingsDirectory
Y.modNameAtLoad = g_currentModName

---------------------------------------------------------------------------
-- logging
---------------------------------------------------------------------------

function Y.log(text)
    print(Y.LOG_PREFIX .. tostring(text))
end

--- Log an informational line only the first time this exact text comes up.
function Y.logOnce(text)
    text = tostring(text)
    if Y.loggedMessages[text] then
        return false
    end
    Y.loggedMessages[text] = true
    Y.log(text)
    return true
end

--- Log a problem, but each distinct text only once and only a few in total.
function Y.logProblem(text)
    text = tostring(text)
    if Y.loggedMessages[text] or Y.loggedErrors >= MAX_LOGGED_ERRORS then
        return
    end
    Y.loggedMessages[text] = true
    Y.loggedErrors = Y.loggedErrors + 1
    Y.log("problem: " .. text)
    if Y.loggedErrors == MAX_LOGGED_ERRORS then
        Y.log("further problems will not be logged")
    end
end

---------------------------------------------------------------------------
-- file output
---------------------------------------------------------------------------

function Y.resolvePath()
    local base = nil
    if type(getUserProfileAppPath) == "function" then
        local ok, profile = pcall(getUserProfileAppPath)
        if ok and type(profile) == "string" and profile ~= "" then
            local last = string.sub(profile, -1)
            if last ~= "/" and last ~= "\\" then
                profile = profile .. "/"
            end
            base = profile .. "modSettings/"
        end
    end

    local folder
    if base ~= nil then
        folder = base .. Y.SETTINGS_FOLDER .. "/"
    elseif type(Y.modSettingsDirAtLoad) == "string" and Y.modSettingsDirAtLoad ~= "" then
        -- what the game handed this mod as its settings folder; the same
        -- place as long as the mod is installed under its own name
        folder = Y.modSettingsDirAtLoad
        local last = string.sub(folder, -1)
        if last ~= "/" and last ~= "\\" then
            folder = folder .. "/"
        end
    else
        return nil
    end

    if type(createFolder) == "function" then
        if base ~= nil then
            pcall(createFolder, base)
        end
        pcall(createFolder, folder)
    end
    return folder .. Y.FILE_NAME, folder
end

local function writeWithIo(path, text)
    if type(io) ~= "table" or type(io.open) ~= "function" then
        return false
    end
    local okOpen, file = pcall(io.open, path, "w")
    if not okOpen or file == nil then
        return false
    end
    local okWrite, r, err = pcall(function()
        return file:write(text)
    end)
    pcall(function()
        file:close()
    end)
    -- a failed write returns nil plus a message; success returns nothing
    -- (Lua 5.1) or the file (later versions)
    return okWrite and not (r == nil and err ~= nil)
end

local function writeWithEngine(path, text)
    if type(createFile) ~= "function" or type(fileWrite) ~= "function" or FileAccess == nil then
        return false
    end
    local okOpen, handle = pcall(createFile, path, FileAccess.WRITE)
    if not okOpen or handle == nil or handle == 0 then
        return false
    end
    local okWrite = pcall(fileWrite, handle, text)
    if type(delete) == "function" then
        pcall(delete, handle)
    end
    return okWrite
end

--- Replace the telemetry file with one line. @return true when written
function Y.writeFile(path, text)
    if Y.writeMethod ~= "engine" and writeWithIo(path, text) then
        Y.writeMethod = "io"
        return true
    end
    if Y.writeMethod ~= "io" and writeWithEngine(path, text) then
        Y.writeMethod = "engine"
        return true
    end
    return false
end

function Y.currentTimeMs()
    local mission = g_currentMission
    if type(mission) == "table" and type(mission.time) == "number" then
        return mission.time
    end
    if type(g_time) == "number" then
        return g_time
    end
    return Y.elapsedMs
end

function Y.emit(sample)
    if Y.filePath == nil then
        return false
    end
    local line = MozaTelemetry.formatLine(Y.seq + 1, Y.currentTimeMs(), sample)
    if Y.writeFile(Y.filePath, line) then
        Y.seq = Y.seq + 1
        if Y.writeFailures > 0 then
            Y.writeFailures = 0
        end
        if Y.seq == 1 then
            Y.log(string.format("first write ok (%s): %s", tostring(Y.writeMethod), (string.gsub(line, "\n", ""))))
        end
        return true
    end

    -- The reader may hold the file for an instant; that is normal and the
    -- next frame tries again. Only a run of failures is worth reporting.
    Y.writeFailures = Y.writeFailures + 1
    if Y.writeFailures == 120 then
        Y.logProblem("cannot write " .. tostring(Y.filePath) .. " (io.open and createFile both failed 120 times in a row)")
    end
    if Y.writeFailures >= 120 then
        Y.retryInMs = 1000
    end
    return false
end

---------------------------------------------------------------------------
-- the game's own steering force feedback
---------------------------------------------------------------------------
--
-- Drivable.actionEventSteer asks g_inputBinding:getBindingForceFeedbackInfo(binding)
-- for the binding that produced the steering input and gets back
-- (isSupported, device, axisIndex). When supported it stores them in
-- spec_drivable.forceFeedback and from then on calls
-- device:setForceFeedback(axisIndex, force, position) every frame -- even
-- with the in-game force feedback strength at 0. Answering "not supported"
-- (false, nil, 0) is what the game itself returns for a binding without
-- force feedback, and makes Drivable skip all of that.
--
-- The function is wrapped for as long as a savegame is loaded, but the
-- wrapper only answers "not supported" while the bridge is present. At all
-- other times it hands the call to the game's function and returns whatever
-- that returns.
--
-- Handing back needs no help: while force feedback is not active on a
-- vehicle, actionEventSteer asks again on every steering event, so the first
-- one after the wrapper stops answering for the game switches it back on.

local function describeDevice(device)
    if type(device) ~= "table" then
        return tostring(device)
    end
    return string.format("'%s' category=%s", tostring(device.deviceName), tostring(device.category))
end

--- Is the game's steering force feedback being suppressed right now?
function Y.isSuppressing()
    return Y.ffb.installed == true and Y.ffb.active == true
end

function Y.installForceFeedbackOverride()
    if Y.ffb.installed then
        return true
    end
    if not Y.suppressGameForceFeedback then
        Y.log("game force feedback left alone (suppressGameForceFeedback = false)")
        return false
    end

    local owner, where = nil, nil
    if type(InputBinding) == "table" and type(InputBinding.getBindingForceFeedbackInfo) == "function" then
        owner, where = InputBinding, "InputBinding class"
    elseif g_inputBinding ~= nil and type(g_inputBinding.getBindingForceFeedbackInfo) == "function" then
        owner, where = g_inputBinding, "g_inputBinding instance"
    end
    if owner == nil then
        Y.log("game force feedback NOT suppressed: InputBinding.getBindingForceFeedbackInfo not found in this game version")
        return false
    end

    -- A fresh state table per installation: the wrapper below keeps it, so a
    -- wrapper left behind by an earlier map stays a pass-through for good.
    local ffb = { installed = false, active = false }
    Y.ffb = ffb

    local original = owner.getBindingForceFeedbackInfo
    ffb.owner = owner
    ffb.ownerHadOwnField = rawget(owner, "getBindingForceFeedbackInfo") ~= nil
    ffb.original = original
    ffb.asked = 0

    -- Log, for the first calls only and each distinct answer once, what the
    -- game's own function says about the steering binding.
    local function note(outcome, isSupported, device, axisIndex)
        if ffb.asked >= 200 then
            return
        end
        ffb.asked = ffb.asked + 1
        pcall(function()
            Y.logOnce(string.format("game asked about steering force feedback: device %s axis=%s supported=%s -> %s",
                describeDevice(device), tostring(axisIndex), tostring(isSupported), outcome))
        end)
    end

    local function passThrough(...)
        note("left to the game (no bridge)", ...)
        return ...
    end

    ffb.replacement = function(inputBinding, binding, ...)
        if ffb.active ~= true then
            if ffb.asked >= 200 then
                return original(inputBinding, binding, ...)
            end
            return passThrough(original(inputBinding, binding, ...))
        end
        if ffb.asked < 200 then
            local ok, isSupported, device, axisIndex = pcall(original, inputBinding, binding, ...)
            if ok then
                note("suppressed (bridge running)", isSupported, device, axisIndex)
            end
        end
        return false, nil, 0
    end

    owner.getBindingForceFeedbackInfo = ffb.replacement
    ffb.installed = true
    if Y.bridgeReader ~= nil then
        Y.log(string.format("force feedback override installed on %s; the game keeps its steering force feedback until the bridge shows up in %s (checked with %s)",
            where, tostring(Y.bridgePath), tostring(Y.bridgeReaderName)))
    else
        Y.log(string.format("force feedback override installed on %s, but there is no way to read %s here (%s): the game keeps its steering force feedback",
            where, tostring(Y.bridgePath), tostring(Y.bridgeReaderName)))
    end
    return true
end

function Y.removeForceFeedbackOverride()
    local ffb = Y.ffb
    if not ffb.installed then
        return
    end
    -- From here on the wrapper passes straight through, whatever happens below.
    ffb.active = false
    ffb.asked = 200
    local owner = ffb.owner
    if owner ~= nil and rawget(owner, "getBindingForceFeedbackInfo") == ffb.replacement then
        if ffb.ownerHadOwnField then
            owner.getBindingForceFeedbackInfo = ffb.original
        else
            owner.getBindingForceFeedbackInfo = nil -- fall back to the class method
        end
        Y.log("game force feedback override removed")
    else
        Y.log("game force feedback override left in place as a pass-through (another script wrapped it)")
    end
    ffb.installed = false
    ffb.owner = nil
    ffb.original = nil
    ffb.replacement = nil
end

--- A vehicle on which the game already switched force feedback on keeps
-- getting it; switch it off the way Drivable.onLeaveVehicle does. Only while
-- suppressing.
function Y.clearVehicleForceFeedback(vehicle)
    if not Y.isSuppressing() or type(vehicle) ~= "table" then
        return false
    end
    local spec = vehicle.spec_drivable
    local state = type(spec) == "table" and spec.forceFeedback or nil
    if type(state) ~= "table" or state.isActive ~= true then
        return false
    end
    local device = state.device
    if type(device) == "table" and type(device.setForceFeedback) == "function" then
        pcall(device.setForceFeedback, device, state.axisIndex or 0, 0, 0)
    end
    state.isActive = false
    state.device = nil
    Y.logOnce("a vehicle had the game's steering force feedback active; switched it off")
    return true
end

--- Start or stop suppressing, following the bridge. Logs each change.
function Y.setBridgePresent(present)
    local ffb = Y.ffb
    present = present == true and ffb.installed == true
    if (ffb.active == true) == present then
        return
    end
    ffb.active = present

    if Y.loggedTransitions < MAX_LOGGED_TRANSITIONS then
        Y.loggedTransitions = Y.loggedTransitions + 1
        if present then
            Y.log("bridge detected: suppressing the game's steering force feedback")
        else
            Y.log("bridge gone: the game's steering force feedback is back in its hands")
        end
        if Y.loggedTransitions == MAX_LOGGED_TRANSITIONS then
            Y.log("the bridge keeps coming and going; further changes will not be logged")
        end
    end

    if present then
        -- release what the game has going on the vehicle being driven
        local ok, vehicle = pcall(MozaTelemetry.findVehicle)
        if ok then
            Y.clearVehicleForceFeedback(vehicle)
        end
    end
    -- Nothing to do when the bridge goes: the mod never commanded the device,
    -- and the game asks about force feedback again on the next steering event.
end

---------------------------------------------------------------------------
-- reading the bridge's presence file
---------------------------------------------------------------------------
--
-- bridge.xml is read with the game's XML functions, the only way a mod can
-- read a file back: the game's io.open only writes (asked to read, it opens
-- the file for writing all the same, which would empty it).
--
-- Every read loads the file anew and frees the handle again. fileExists is
-- asked first, so a missing file never reaches the XML loader (which would
-- log an error for it) and counts as "gone" rather than "unreadable".

local XML_OBJECT_NAME = "MozaYokeLinkBridge"

--- @return version, beat as read (either may be nil), or nothing at all
local function loadWithXmlClass(path)
    local xmlFile = XMLFile.loadIfExists(XML_OBJECT_NAME, path)
    if xmlFile == nil then
        return nil, nil
    end
    local ok, version, beat = pcall(function()
        return xmlFile:getInt("bridge#version"), xmlFile:getInt("bridge#beat")
    end)
    pcall(function()
        xmlFile:delete()
    end)
    if not ok then
        return nil, nil
    end
    return version, beat
end

--- The engine functions the XMLFile class is built on.
local function loadWithXmlFunctions(path)
    local handle = loadXMLFile(XML_OBJECT_NAME, path)
    if handle == nil or handle == 0 then
        return nil, nil
    end
    local ok, version, beat = pcall(function()
        return getXMLInt(handle, "bridge#version"), getXMLInt(handle, "bridge#beat")
    end)
    pcall(delete, handle)
    if not ok then
        return nil, nil
    end
    return version, beat
end

--- One look at the presence file.
-- @return the beat, nil for "nothing usable this time", or FILE_GONE
function Y.readBridgeBeat(path, load)
    if type(fileExists) ~= "function" then
        return nil
    end
    local okExists, exists = pcall(fileExists, path)
    if not okExists then
        return nil
    end
    if exists ~= true then
        return MozaTelemetry.FILE_GONE
    end
    local ok, version, beat = pcall(load, path)
    if not ok then
        return nil
    end
    return MozaTelemetry.bridgeBeat(version, beat)
end

--- Pick the way to read the presence file. @return load function or nil, name
function Y.chooseBridgeReader()
    if type(fileExists) ~= "function" then
        return nil, "no fileExists"
    end
    if type(XMLFile) == "table" and type(XMLFile.loadIfExists) == "function" then
        return loadWithXmlClass, "XMLFile.loadIfExists"
    end
    if type(loadXMLFile) == "function" and type(getXMLInt) == "function" and type(delete) == "function" then
        return loadWithXmlFunctions, "loadXMLFile"
    end
    return nil, "no XML functions"
end

--- Wall-clock time that passed without update() being called, in ms.
-- dt only covers frames that ran. If the game stood still (it does not call
-- update while loading, and may not while paused), what the mod knows about
-- the bridge is older than the frame times add up to.
function Y.gapSinceLastUpdateMs(dt)
    if type(getTimeSec) ~= "function" then
        return 0
    end
    local ok, now = pcall(getTimeSec)
    if not ok or type(now) ~= "number" or now ~= now then
        return 0
    end
    local last = Y.lastClockSec
    Y.lastClockSec = now
    if last == nil then
        return 0
    end
    local gap = (now - last) * 1000 - dt
    if gap > UPDATE_GAP_MS and gap < math.huge then
        return gap
    end
    return 0
end

--- Once a second: look at the presence file and follow what it says.
function Y.updateBridgePresence(dt)
    if not Y.ffb.installed or Y.bridgeReader == nil or Y.bridgePath == nil then
        return
    end
    if Y.presence == nil then
        Y.presence = MozaTelemetry.newPresence()
    end

    local gap = Y.gapSinceLastUpdateMs(dt)
    if not MozaTelemetry.presenceTick(Y.presence, dt + gap, gap > 0) then
        return
    end

    local token = Y.readBridgeBeat(Y.bridgePath, Y.bridgeReader)
    local present = MozaTelemetry.presenceObserve(Y.presence, token)

    -- One hint each, once, when the file is there but its beat never changes
    -- or cannot be had. Both leave the game's force feedback alone.
    if present or token == MozaTelemetry.FILE_GONE then
        Y.unchangedReads, Y.unusableReads = 0, 0
    elseif token == nil then
        Y.unusableReads = Y.unusableReads + 1
        if Y.unusableReads == 6 then
            Y.logOnce(string.format("%s is there but %s gets no beat from it (not version 1 of the bridge's format, or not readable): the bridge counts as not running",
                Y.BRIDGE_FILE_NAME, tostring(Y.bridgeReaderName)))
        end
        if Y.unusableReads >= MAX_UNUSABLE_READS then
            -- a load that fails every time may put a line in the game log
            -- every time; stop asking until the next savegame load
            Y.bridgeReader = nil
            Y.logOnce("no longer checking " .. Y.BRIDGE_FILE_NAME .. " in this session")
        end
    else
        Y.unusableReads = 0
        Y.unchangedReads = Y.unchangedReads + 1
        if Y.unchangedReads == 6 then
            Y.logOnce(string.format("%s is there but its beat is not changing (left over from an earlier run of the bridge): the bridge counts as not running",
                Y.BRIDGE_FILE_NAME))
        end
    end

    Y.setBridgePresent(present)
end

--- One-time list of input devices and which axes the game reports as force
-- feedback capable.
function Y.logInputDevices(vehicle)
    local lines = 0
    if type(getNumOfGamepads) == "function" and type(getGamepadName) == "function" then
        local okCount, count = pcall(getNumOfGamepads)
        if okCount and type(count) == "number" then
            for id = 0, math.min(count, 8) - 1 do
                local okName, name = pcall(getGamepadName, id)
                if not okName then
                    name = "?"
                end
                local category = "?"
                if type(getGamepadCategory) == "function" then
                    local ok, value = pcall(getGamepadCategory, id)
                    if ok then category = tostring(value) end
                end
                local axes = "unknown"
                if type(getHasGamepadAxisForceFeedback) == "function" then
                    local found = {}
                    local maxAxes = (type(Input) == "table" and type(Input.MAX_NUM_AXES) == "number") and Input.MAX_NUM_AXES or MAX_FF_AXES
                    for axis = 0, math.min(maxAxes, MAX_FF_AXES) - 1 do
                        local ok, has = pcall(getHasGamepadAxisForceFeedback, id, axis)
                        if ok and has == true then
                            found[#found + 1] = tostring(axis)
                        end
                    end
                    axes = (#found > 0) and table.concat(found, ",") or "none"
                end
                Y.log(string.format("input device %d: '%s' category=%s forceFeedbackAxes=%s", id, tostring(name), category, axes))
                lines = lines + 1
            end
        end
    end
    if lines == 0 then
        Y.log("input devices: none reported (or getNumOfGamepads/getGamepadName not available)")
    end

    local spec = type(vehicle) == "table" and vehicle.spec_drivable or nil
    local state = type(spec) == "table" and spec.forceFeedback or nil
    if type(state) == "table" then
        Y.log(string.format("vehicle spec_drivable.forceFeedback: isActive=%s axisIndex=%s intensity=%s",
            tostring(state.isActive), tostring(state.axisIndex), tostring(state.intensity)))
    else
        Y.log("vehicle spec_drivable.forceFeedback: not present")
    end
end

---------------------------------------------------------------------------
-- per vehicle diagnostics
---------------------------------------------------------------------------

local function vehicleName(vehicle)
    local name = nil
    if type(vehicle.getFullName) == "function" then
        local ok, value = pcall(vehicle.getFullName, vehicle)
        if ok then name = value end
    end
    if name == nil and type(vehicle.getName) == "function" then
        local ok, value = pcall(vehicle.getName, vehicle)
        if ok then name = value end
    end
    return tostring(name or vehicle.typeName or "?")
end

local SOURCE_ORDER = { "speed", "steer", "mass", "engine", "slip", "surface", "implements" }

function Y.logVehicle(vehicle, vehicleSource, sample, info)
    local parts = {}
    for i = 1, #SOURCE_ORDER do
        local key = SOURCE_ORDER[i]
        parts[#parts + 1] = key .. "<-" .. tostring(info.sources[key] or "none")
    end
    Y.log(string.format("driving '%s' (found via %s); sources: %s",
        vehicleName(vehicle), tostring(vehicleSource), table.concat(parts, "; ")))
    Y.log("sample: " .. (string.gsub(MozaTelemetry.formatLine(Y.seq + 1, Y.currentTimeMs(), sample), "\n", "")))
end

---------------------------------------------------------------------------
-- mod event listener
---------------------------------------------------------------------------

function Y:loadMap(name)
    local ok, err = pcall(function()
        Y.seq = 0
        Y.elapsedMs = 0
        Y.writeFailures = 0
        Y.retryInMs = 0
        Y.lastVehicle = nil
        Y.devicesLogged = false
        Y.loggedErrors = 0
        Y.loggedMessages = {}
        Y.loggedTransitions = 0
        Y.unchangedReads = 0
        Y.unusableReads = 0
        Y.lastClockSec = nil
        Y.presence = MozaTelemetry.newPresence()
        Y.bridgePath, Y.bridgeReader, Y.bridgeReaderName = nil, nil, nil
        Y.throttle = MozaTelemetry.newThrottle()

        -- A dedicated server has no local player and nobody to feel the yoke.
        if g_dedicatedServer ~= nil or g_dedicatedServerInfo ~= nil then
            Y.enabled = false
            Y.log("version " .. Y.VERSION .. ": dedicated server, telemetry disabled")
            return
        end
        Y.enabled = true

        local folder
        Y.filePath, folder = Y.resolvePath()
        if folder ~= nil then
            Y.bridgePath = folder .. Y.BRIDGE_FILE_NAME
            Y.bridgeReader, Y.bridgeReaderName = Y.chooseBridgeReader()
        else
            Y.bridgeReaderName = "no settings folder"
        end
        Y.log(string.format("version %s, telemetry file: %s", Y.VERSION, tostring(Y.filePath)))
        if Y.filePath == nil then
            Y.logProblem("no user profile path (getUserProfileAppPath missing); nothing will be written")
        elseif not Y.emit({ active = false }) then
            Y.logProblem("could not write the telemetry file yet; will keep trying")
        end

        Y.installForceFeedbackOverride()
    end)
    if not ok then
        Y.logProblem("loadMap: " .. tostring(err))
    end
end

function Y:deleteMap()
    local ok, err = pcall(function()
        if Y.enabled and Y.filePath ~= nil then
            Y.emit({ active = false }) -- leave the file saying "not driving"
        end
        Y.removeForceFeedbackOverride()
        Y.enabled = false
        Y.lastVehicle = nil
    end)
    if not ok then
        Y.logProblem("deleteMap: " .. tostring(err))
    end
end

function Y.onUpdate(dt)
    if not Y.enabled then
        return
    end
    if type(dt) ~= "number" or dt ~= dt or dt < 0 then
        dt = 0
    end
    Y.elapsedMs = Y.elapsedMs + dt

    -- Follow the bridge. Before the pause check on purpose: the bridge keeps
    -- beating while the game is paused, so this keeps looking for as long as
    -- the game keeps calling update. If anything goes wrong here the game
    -- gets its force feedback back.
    local okPresence, presenceError = pcall(Y.updateBridgePresence, dt)
    if not okPresence then
        Y.setBridgePresent(false)
        Y.logProblem("bridge presence: " .. tostring(presenceError))
    end

    -- Paused (single-player menu): stop writing, so seq stops and the bridge
    -- sees the telemetry go stale, as the contract describes.
    local mission = g_currentMission
    if type(mission) == "table" and mission.paused == true then
        return
    end

    local vehicle, vehicleSource = MozaTelemetry.findVehicle()
    if type(vehicle) ~= "table" then
        vehicle = nil
    end

    if vehicle ~= nil then
        Y.clearVehicleForceFeedback(vehicle)
    end

    if Y.throttle == nil then
        Y.throttle = MozaTelemetry.newThrottle()
    end
    local due = MozaTelemetry.throttleStep(Y.throttle, dt, vehicle ~= nil)

    if Y.retryInMs > 0 then
        Y.retryInMs = Y.retryInMs - dt
        if Y.retryInMs > 0 then
            return
        end
    end

    local isNewVehicle = vehicle ~= Y.lastVehicle
    if not due and not isNewVehicle then
        return
    end

    local sample, info = MozaTelemetry.gather(vehicle)

    if isNewVehicle then
        Y.lastVehicle = vehicle
        if vehicle ~= nil then
            Y.logVehicle(vehicle, vehicleSource, sample, info)
            if not Y.devicesLogged then
                Y.devicesLogged = true
                Y.logInputDevices(vehicle)
            end
        end
    end
    for key, message in pairs(info.errors) do
        Y.logProblem(key .. ": " .. message)
    end

    Y.emit(sample)
end

function Y:update(dt)
    local ok, err = pcall(Y.onUpdate, dt)
    if not ok then
        Y.logProblem("update: " .. tostring(err))
    end
end

---------------------------------------------------------------------------
-- registration
---------------------------------------------------------------------------

if type(addModEventListener) == "function" then
    addModEventListener(MozaYokeLink)
    Y.log("version " .. Y.VERSION .. " script loaded")
else
    print(Y.LOG_PREFIX .. "addModEventListener is not available; the mod is inactive")
end
