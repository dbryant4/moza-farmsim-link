// Checks the exact command stream the bridge sends, against the in-process fake SDK.

#include "fake_moza_sdk.hpp"
#include "ffb_session.hpp"
#include "link.hpp"
#include "presence.hpp"
#include "stroke.hpp"
#include "yoke_lights.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fake_moza::Command;

int g_failures = 0;

#define CHECK(condition)                                                           \
    do                                                                             \
    {                                                                              \
        if (!(condition))                                                          \
        {                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (false)

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

std::string describe(const Command& command)
{
    std::string text = std::to_string(command.id) + " [";
    for (std::size_t i = 0; i < command.args.size(); ++i)
    {
        text += (i ? ", " : "") + std::to_string(command.args[i]);
    }
    return text + "]";
}

void checkCommands(const std::vector<Command>& expected, int line)
{
    const auto& actual = fake_moza::state().commands;
    if (actual == expected)
    {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: command stream mismatch\n", __FILE__, line);
    for (std::size_t i = 0; i < expected.size() || i < actual.size(); ++i)
    {
        std::fprintf(stderr, "  %-2zu expected %-60s actual %s\n", i, i < expected.size() ? describe(expected[i]).c_str() : "-",
                     i < actual.size() ? describe(actual[i]).c_str() : "-");
    }
}

/** Initializes the fake SDK and opens its flight base for the lifetime of a test. */
struct Fixture
{
    Fixture()
    {
        fake_moza::reset();
        CHECK(moza::initialize() == MOZA_ERR_NO_ERROR);
    }
    ~Fixture() { moza::shutdown(); }

    moza::as::Wheelbase open()
    {
        const auto found = bridge::findFlightBase(0);
        CHECK(found.has_value());
        MOZA_DEVICE_HANDLE handle = MOZA_INVALID_DEVICE_HANDLE;
        CHECK(Moza_DeviceOpen(found ? found->id : 0, &handle) == MOZA_ERR_NO_ERROR);
        return moza::as::Wheelbase(handle);
    }
};

void testSpringAndDamperCommandStream()
{
    Fixture fixture;
    auto    base = fixture.open();
    {
        bridge::FfbSession ffb(base, 1000);
        CHECK(ffb.begin());
        const auto spring = ffb.addEffect(bridge::EffectType::Spring);
        const auto damper = ffb.addEffect(bridge::EffectType::Damper);
        CHECK(spring == 1);
        CHECK(damper == 2);

        CHECK(ffb.setCondition(spring, bridge::Axis::Roll, bridge::conditionFromPercent(30, 10000)));
        CHECK(ffb.setCondition(spring, bridge::Axis::Pitch, {}));
        CHECK(ffb.setCondition(damper, bridge::Axis::Roll, bridge::conditionFromPercent(20, 10000)));
        CHECK(ffb.start(spring));
        CHECK(ffb.start(damper));
    }

    // Field order per sdk/docs/Flight_Base_Force_Feedback_Command_Guide.md:
    // 607 condition = index, blockOffset, center, +coef, -coef, +sat, -sat, deadBand, duration, enableAxis, dirX, dirY
    checkCommands(
        {
            {610, {2}},                                               // reset
            {610, {1}},                                               // start calculation
            {603, {8, 1}},                                            // spring -> index 1
            {603, {9, 2}},                                            // damper -> index 2
            {607, {1, 0, 0, 3000, 3000, 10000, 10000, 0, 1000, 3, 0, 0}},
            {607, {1, 1, 0, 0, 0, 0, 0, 0, 1000, 3, 0, 0}},
            {607, {2, 0, 0, 2000, 2000, 10000, 10000, 0, 1000, 3, 0, 0}},
            {609, {1, 1}},                                            // start spring
            {609, {2, 1}},                                            // start damper
            {609, {1, 0}},                                            // leaving scope: stop + free each
            {609, {1, 2}},
            {609, {2, 0}},
            {609, {2, 2}},
            {610, {3}},                                               // stop all
        },
        __LINE__);
}

void testConstantForceCommand()
{
    Fixture fixture;
    auto    base = fixture.open();

    bridge::FfbSession ffb(base, 1000);
    CHECK(ffb.begin());
    const auto force = ffb.addEffect(bridge::EffectType::ConstantForce);
    CHECK(force == 1);

    // 604 constant = index, magnitude, duration, enableAxis {X, Y, DirectionEnable}, dirX, dirY
    CHECK(ffb.setConstant(force, -1200, bridge::forceAlong(bridge::Axis::Roll)));
    CHECK(fake_moza::state().commands.back() == (Command{604, {1, -1200, 1000, 1, 0, 1, 9000, 0}}));
    CHECK(ffb.setConstant(force, 800, bridge::forceAlong(bridge::Axis::Pitch)));
    CHECK(fake_moza::state().commands.back() == (Command{604, {1, 800, 1000, 0, 1, 1, 0, 0}}));
    CHECK(fake_moza::state().commands[2] == (Command{603, {1, 1}}));
}

void testPeriodicCommand()
{
    Fixture fixture;
    auto    base = fixture.open();

    bridge::FfbSession ffb(base, 1000);
    CHECK(ffb.begin());
    const auto sine = ffb.addEffect(bridge::EffectType::Sine);
    CHECK(sine == 1);
    CHECK(fake_moza::state().commands.back() == (Command{603, {4, 1}}));

    // 606 periodic = index, magnitude, offset, phase, period, duration, enableAxis, dirX, dirY
    CHECK(ffb.setPeriodic(sine, 3000, 40, bridge::forceAlong(bridge::Axis::Roll)));
    CHECK(fake_moza::state().commands.back() == (Command{606, {1, 3000, 0, 0, 40, 1000, 5, 9000, 0}}));
    CHECK(ffb.setPeriodic(sine, 3000, 40, bridge::forceAlong(bridge::Axis::Pitch), 60000));
    CHECK(fake_moza::state().commands.back() == (Command{606, {1, 3000, 0, 0, 40, 60000, 6, 0, 0}}));
}

void testStopIsIdempotent()
{
    Fixture fixture;
    auto    base = fixture.open();

    bridge::FfbSession ffb(base, 500);
    ffb.stop();
    CHECK(fake_moza::state().commands.empty());

    CHECK(ffb.begin());
    CHECK(ffb.addEffect(bridge::EffectType::Spring) == 1);
    ffb.stop();
    const auto afterFirstStop = fake_moza::state().commands.size();
    ffb.stop();
    CHECK(fake_moza::state().commands.size() == afterFirstStop);
    CHECK(fake_moza::state().commands.back() == (Command{610, {3}}));
}

void testControlTimeoutIsTolerated()
{
    Fixture fixture;
    auto    base = fixture.open();

    fake_moza::state().controlSyncResult = MOZA_ERR_TIMEOUT;
    bridge::FfbSession ffb(base, 500);
    CHECK(ffb.begin());
    CHECK(ffb.addEffect(bridge::EffectType::Spring) == 1);
}

void testControlErrorFailsBegin()
{
    Fixture fixture;
    auto    base = fixture.open();

    fake_moza::state().controlSyncResult = MOZA_ERR_NOT_SUPPORTED;
    bridge::FfbSession ffb(base, 500);
    CHECK(!ffb.begin());
}

void testRefusedEffectReturnsZero()
{
    Fixture fixture;
    auto    base = fixture.open();

    fake_moza::state().acceptEffects = false;
    bridge::FfbSession ffb(base, 500);
    CHECK(ffb.begin());
    CHECK(ffb.addEffect(bridge::EffectType::Spring) == 0);
    ffb.stop();
    // Nothing was registered, so there is nothing to stop or free individually.
    for (const auto& command : fake_moza::state().commands)
    {
        CHECK(command.id != 609);
    }
}

void testFindFlightBaseWaitsForEnumeration()
{
    Fixture fixture;

    fake_moza::state().emptyDeviceListCalls = 2;
    CHECK(!bridge::findFlightBase(0).has_value());
    CHECK(bridge::findFlightBase(2000).has_value());

    CHECK(bridge::findFlightBase(0, "Yoke").has_value());
    CHECK(!bridge::findFlightBase(0, "Throttle").has_value());
}

void testConditionFromPercent()
{
    CHECK(bridge::conditionFromPercent(0, 10000).coefficient == 0);
    CHECK(bridge::conditionFromPercent(50, 10000).coefficient == 5000);
    CHECK(bridge::conditionFromPercent(250, 10000).coefficient == 10000);
    CHECK(bridge::conditionFromPercent(-5, 10000).coefficient == 0);
    // The coefficient is an int16 field while saturation is uint16.
    CHECK(bridge::conditionFromPercent(100, 65535).coefficient == 32767);
    CHECK(bridge::conditionFromPercent(100, 65535).saturation == 65535);
    CHECK(bridge::conditionFromPercent(100, 10000).saturation == 10000);
}

/** Result of running a StrokeController against a simple friction-limited axis model. */
struct StrokeRun
{
    double minPosition = 1.0;
    double maxPosition = 0.0;
    double endPosition = 0.0;
    double peakForce   = 0.0;
    double peakRise    = 0.0;
    double seconds     = 0.0;
    int    stalls      = 0;
    bool   finished    = false;
};

StrokeRun runStroke(const bridge::StrokeSettings& settings, double start, double stopLow, double stopHigh)
{
    const double dt       = 0.05;
    const double friction = 0.4 * settings.force;

    bridge::StrokeController stroke(settings);
    StrokeRun                run;
    double                   position = start;
    double                   previous = 0.0;
    while (!stroke.finished() && run.seconds < 300.0)
    {
        const double force = stroke.update(position, dt);
        // A positive magnitude lowers the position, as on the real base; friction eats the first 40%.
        if (std::abs(force) > friction)
        {
            const double drive = (std::abs(force) - friction) / settings.force;
            position           = std::clamp(position - (force > 0 ? 1 : -1) * drive * 0.6 * dt, stopLow, stopHigh);
        }
        run.minPosition = std::min(run.minPosition, position);
        run.maxPosition = std::max(run.maxPosition, position);
        run.peakForce   = std::max(run.peakForce, std::abs(force));
        run.peakRise    = std::max(run.peakRise, std::abs(force) - std::abs(previous));
        previous        = force;
        run.seconds += dt;
    }
    run.endPosition = position;
    run.stalls      = stroke.stalls();
    run.finished    = stroke.finished();
    return run;
}

void testStrokeReachesBothEndsAndReturnsToCenter()
{
    bridge::StrokeSettings settings;
    settings.force  = 20000;
    settings.cycles = 2;

    const StrokeRun run = runStroke(settings, 0.55, 0.0, 1.0);
    CHECK(run.finished);
    CHECK(run.stalls == 0);
    CHECK(run.minPosition <= settings.low);
    CHECK(run.maxPosition >= settings.high);
    // Friction eats 40% of peak force in this model, so the fading return stops within 40% of the taper.
    CHECK(std::abs(run.endPosition - 0.5) <= 0.4 * settings.centerTaper + 0.01);
    CHECK(run.peakForce <= settings.force + 1);
    // Force builds no faster than the ramp allows: full force over 0.5 s, in 50 ms steps.
    CHECK(run.peakRise <= settings.force * 0.05 / settings.rampSeconds + 1);
}

void testStrokeGivesUpOnAnEndItCannotReach()
{
    bridge::StrokeSettings settings;
    settings.force  = 20000;
    settings.cycles = 1;

    // Hard stops inside the target range, like an axis that binds before its calibrated ends.
    const StrokeRun run = runStroke(settings, 0.5, 0.2, 0.8);
    CHECK(run.finished);
    CHECK(run.stalls == 2);
    CHECK(run.minPosition == 0.2);
    CHECK(run.maxPosition == 0.8);
    CHECK(std::abs(run.endPosition - 0.5) <= 0.4 * settings.centerTaper + 0.01);
}

void testStrokeEndsEvenIfTheAxisNeverMoves()
{
    bridge::StrokeSettings settings;
    settings.force  = 20000;
    settings.cycles = 2;

    const StrokeRun run = runStroke(settings, 0.5, 0.5, 0.5);
    CHECK(run.finished);
    CHECK(run.seconds < 30.0);
}

void testDeviceForcesScaleAndRoute()
{
    bridge::ForceOutput output;
    output.rollSpring  = 0.5;
    output.pitchSpring = 0.8;
    output.rollDamper  = 0.25;
    output.pitchDamper = 0.1;
    output.rollForce   = 0.2; // toward steering right
    output.rumble      = 0.1;
    output.rumbleHz    = 20.0;

    bridge::LinkTuning tuning;
    tuning.fullScale = 10000;

    bridge::DeviceForces forces = bridge::toDeviceForces(output, tuning);
    CHECK(forces.rollSpring == 5000);
    CHECK(forces.pitchSpring == 8000);
    CHECK(forces.rollDamper == 2500);
    CHECK(forces.pitchDamper == 1000);
    // On the AY210 a positive magnitude moves roll left, so a pull to the right is negative.
    CHECK(forces.pull == -2000);
    CHECK(forces.rumble == 1000);
    CHECK(forces.rumblePeriodMs == 50);
    CHECK(forces.rumbleAxis == bridge::Axis::Pitch);

    tuning.flipPull     = true;
    tuning.rumbleOnRoll = true;
    forces              = bridge::toDeviceForces(output, tuning);
    CHECK(forces.pull == 2000);
    CHECK(forces.rumbleAxis == bridge::Axis::Roll);

    tuning.strength = 0.5;
    forces          = bridge::toDeviceForces(output, tuning);
    CHECK(forces.rollSpring == 2500);
    CHECK(forces.pull == 1000);
    CHECK(forces.rumble == 500);

    // The ground's texture scales the rumble and nothing else; a value that is not a number leaves it alone.
    output.rumbleTexture = 0.4;
    forces               = bridge::toDeviceForces(output, tuning);
    CHECK(forces.rumble == 200);
    CHECK(forces.rollSpring == 2500);
    CHECK(forces.pull == 1000);
    output.rumbleTexture = std::nan("");
    CHECK(bridge::toDeviceForces(output, tuning).rumble == 500);
    output.rumbleTexture = 7.0;
    CHECK(bridge::toDeviceForces(output, tuning).rumble == 500);
    output.rumbleTexture = 1.0;

    // A kick rides on top of the steady pull, in the same direction convention, within the device's range.
    tuning          = bridge::LinkTuning{};
    tuning.fullScale = 10000;
    output.rollKick = 0.5;
    CHECK(bridge::toDeviceForces(output, tuning).pull == -7000);
    output.rollKick = -0.5;
    CHECK(bridge::toDeviceForces(output, tuning).pull == 3000);
    output.rollKick = 5.0;
    CHECK(bridge::toDeviceForces(output, tuning).pull == -10000);
    output.rollKick = 0.0;

    // A bump on both wheels becomes two slow sines, one per axis, scaled like everything else.
    output.pitchThump = 0.6;
    output.rollWiggle = 0.2;
    output.bumpHz     = 8.0;
    forces            = bridge::toDeviceForces(output, tuning);
    CHECK(forces.thump == 6000 && forces.wiggle == 2000 && forces.bumpPeriodMs == 125);
    tuning.strength = 0.0;
    forces          = bridge::toDeviceForces(output, tuning);
    CHECK(forces.thump == 0 && forces.wiggle == 0);
    output.bumpHz = 0.0;
    CHECK(bridge::toDeviceForces(output, tuning).bumpPeriodMs >= 5);

    // The acceleration sway: the pitch spring is re-centered on the target (a position, so strength does
    // not scale it), and the controller's push goes out with the base's sign, where positive lowers the position.
    tuning             = bridge::LinkTuning{};
    tuning.fullScale   = 10000;
    output.pitchTarget = 0.12;
    forces             = bridge::toDeviceForces(output, tuning, 0.5);
    CHECK(forces.pitchCenter == 1200);
    CHECK(forces.pitchPush == -5000);
    CHECK(near(bridge::pitchTargetFor(output, tuning), 0.12));
    tuning.flipSway = true;
    CHECK(bridge::toDeviceForces(output, tuning, 0.0).pitchCenter == -1200);
    CHECK(near(bridge::pitchTargetFor(output, tuning), -0.12));
    tuning.flipSway = false;
    tuning.strength = 0.5;
    forces          = bridge::toDeviceForces(output, tuning, -0.5);
    CHECK(forces.pitchCenter == 1200 && forces.pitchPush == 2500);
    tuning.strength = 0.0;
    CHECK(bridge::toDeviceForces(output, tuning, 1.0).pitchPush == 0);
    CHECK(bridge::toDeviceForces(output, tuning, std::nan("")).pitchPush == 0);
    output.pitchTarget = 0.0;
}

void testDeviceForcesAreZeroWhenForcesAreOff()
{
    bridge::ForceOutput output;
    output.rollSpring = output.pitchSpring = output.rollDamper = output.pitchDamper = output.rumble = 1.0;
    output.rollForce = -1.0;
    output.rumbleHz  = 15.0;

    bridge::LinkTuning tuning;
    tuning.strength = 0.0;

    const bridge::DeviceForces forces = bridge::toDeviceForces(output, tuning);
    CHECK(forces.rollSpring == 0 && forces.pitchSpring == 0 && forces.rollDamper == 0 && forces.pitchDamper == 0);
    CHECK(forces.pull == 0 && forces.rumble == 0);
}

void testDeviceForcesSurviveHostileInput()
{
    bridge::ForceOutput output;
    output.rollSpring  = 7.0;
    output.pitchSpring = -3.0;
    output.rollDamper  = std::nan("");
    output.rollForce   = -50.0;
    output.rumble      = 2.0;
    output.rumbleHz    = 0.0;

    bridge::LinkTuning tuning;
    tuning.strength = 9.0;

    const bridge::DeviceForces forces = bridge::toDeviceForces(output, tuning);
    CHECK(forces.rollSpring == 32767);
    CHECK(forces.pitchSpring == 0);
    CHECK(forces.rollDamper == 0);
    CHECK(forces.pull == 32767);
    CHECK(forces.rumble == 32767);
    // A frequency of zero must not turn into a divide by zero or a zero-length period.
    CHECK(forces.rumblePeriodMs >= 5);

    output.rumbleHz = 100000.0;
    CHECK(bridge::toDeviceForces(output, tuning).rumblePeriodMs == 5);
    output.rumbleHz = 1.0;
    CHECK(bridge::toDeviceForces(output, tuning).rumblePeriodMs == 1000);
}

std::string readWholeFile(const std::filesystem::path& path)
{
    std::ifstream     file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string presenceXml(int beat)
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"no\"?>\n<bridge version=\"1\" beat=\"" + std::to_string(beat) + "\"/>\n";
}

void testPresenceFileBeatsWhileAliveAndIsRemovedAfterwards()
{
    using Clock = bridge::PresenceFile::Clock;
    const auto folder = std::filesystem::temp_directory_path() / "moza_fs_bridge_presence_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto file = folder / "bridge.xml";

    const Clock::time_point start = Clock::now();
    {
        bridge::PresenceFile presence(folder / "telemetry.txt");
        CHECK(presence.path() == file);
        CHECK(!std::filesystem::exists(file));

        presence.tick(start);
        CHECK(readWholeFile(file) == presenceXml(1));

        // Not again until half a second has passed.
        presence.tick(start + std::chrono::milliseconds(400));
        CHECK(readWholeFile(file) == presenceXml(1));
        presence.tick(start + std::chrono::milliseconds(500));
        CHECK(readWholeFile(file) == presenceXml(2));
        presence.tick(start + std::chrono::milliseconds(1700));
        CHECK(readWholeFile(file) == presenceXml(3));
    }
    // Gone with the object, so the mod does not have to wait out its timeout.
    CHECK(!std::filesystem::exists(file));
    // Replaced by rename each time, with no temporary file left behind.
    CHECK(std::filesystem::is_empty(folder));
    std::filesystem::remove_all(folder);
}

void testPresenceFileStaysQuietWithoutTheModsFolder()
{
    using Clock = bridge::PresenceFile::Clock;
    const auto folder = std::filesystem::temp_directory_path() / "moza_fs_bridge_presence_missing";
    std::filesystem::remove_all(folder);

    {
        bridge::PresenceFile presence(folder / "telemetry.txt");
        presence.tick(Clock::now());
        // The mod creates this folder; the bridge must not, e.g. on a PC where the game is not installed.
        CHECK(!std::filesystem::exists(folder));

        // Once the mod has made it, the next beat lands.
        std::filesystem::create_directories(folder);
        presence.tick(Clock::now() + std::chrono::seconds(1));
        CHECK(std::filesystem::exists(folder / "bridge.xml"));
    }
    CHECK(!std::filesystem::exists(folder / "bridge.xml"));
    std::filesystem::remove_all(folder);

    // No telemetry path at all: nothing to write next to, and nothing to remove.
    bridge::PresenceFile nowhere{std::filesystem::path()};
    nowhere.tick(Clock::now());
    CHECK(nowhere.path().empty());
}

void testLightsFollowEachMachinesOwnEngineRange()
{
    bridge::LightTuning tuning;
    tuning.followEngine = true;
    tuning.low          = 3.0;
    tuning.idleRpm      = 900.0;
    tuning.fullRpm      = 2200.0;
    tuning.colorShift   = true;
    tuning.colorFrom    = 0.6;

    bridge::Telemetry driving;
    driving.seq    = 1;
    driving.active = true;

    // Engine off: dark. Starting and idling: the low setting, in the user's own colour.
    driving.rpm = 0.0;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 0.0);
    driving.starting = true;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 3.0 && bridge::lightTargetFor(&driving, tuning).heat == 0.0);
    driving.starting = false;

    // A big tractor: idles at 850, flat out at 1900.
    driving.rpmMin = 850.0;
    driving.rpmMax = 1900.0;
    driving.rpm    = 850.0;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 3.0);
    driving.rpm = 1375.0; // half way
    CHECK(near(bridge::lightTargetFor(&driving, tuning).level, 9.0, 1e-9));
    CHECK(bridge::lightTargetFor(&driving, tuning).heat == 0.0);
    driving.rpm = 1900.0;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 15.0);
    CHECK(near(bridge::lightTargetFor(&driving, tuning).heat, 1.0, 1e-9));

    // A small loader that revs to 3000: at 1900 it is not yet at full brightness, nor into the colour shift.
    driving.rpmMin = 1000.0;
    driving.rpmMax = 3000.0;
    driving.rpm    = 1900.0;
    const bridge::LightTarget loader = bridge::lightTargetFor(&driving, tuning);
    CHECK(near(loader.level, 3.0 + 12.0 * 0.45, 1e-9));
    CHECK(loader.heat == 0.0);
    driving.rpm = 3000.0;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 15.0);

    // The colour only moves over the last part of the range: nothing at 60%, half way at 80%, all at 100%.
    driving.rpm = 1000.0 + 0.6 * 2000.0;
    CHECK(near(bridge::lightTargetFor(&driving, tuning).heat, 0.0, 1e-9));
    driving.rpm = 1000.0 + 0.8 * 2000.0;
    CHECK(near(bridge::lightTargetFor(&driving, tuning).heat, 0.5, 1e-9));
    driving.rpm = 99999.0;
    CHECK(near(bridge::lightTargetFor(&driving, tuning).heat, 1.0, 1e-9));
    tuning.colorShift = false;
    CHECK(bridge::lightTargetFor(&driving, tuning).heat == 0.0);
    tuning.colorShift = true;

    // An older mod, or a vehicle the game gives no range for: the fallback settings apply.
    driving.rpmMin = 0.0;
    driving.rpmMax = 0.0;
    driving.rpm    = 1550.0;
    CHECK(near(bridge::lightTargetFor(&driving, tuning).level, 9.0, 1e-9));
    driving.rpmMin = 2000.0; // nonsense: idle above maximum
    driving.rpmMax = 1000.0;
    CHECK(near(bridge::lightTargetFor(&driving, tuning).level, 9.0, 1e-9));

    // An electric motor counts as an engine for this: it is about whether the machine is on.
    driving.electric = true;
    driving.rpm      = 2200.0;
    CHECK(bridge::lightTargetFor(&driving, tuning).level == 15.0);

    // Not driving, no telemetry, or the feature switched off: leave the lights as the user had them.
    bridge::Telemetry onFoot;
    onFoot.seq = 2;
    CHECK(bridge::lightTargetFor(&onFoot, tuning).level < 0.0 && bridge::lightTargetFor(&onFoot, tuning).heat == 0.0);
    CHECK(bridge::lightTargetFor(nullptr, tuning).level < 0.0);
    tuning.followEngine = false;
    CHECK(bridge::lightTargetFor(&driving, tuning).level < 0.0);

    // Odd settings stay inside the handle's range.
    tuning.followEngine = true;
    tuning.low          = 40.0;
    tuning.fullRpm      = 100.0;
    tuning.colorFrom    = 7.0;
    const bridge::LightTarget odd = bridge::lightTargetFor(&driving, tuning);
    CHECK(odd.level == 15.0 && odd.heat >= 0.0 && odd.heat <= 1.0);
    tuning.low       = std::nan("");
    tuning.colorFrom = std::nan("");
    driving.rpm      = 500.0;
    const bridge::LightTarget nan = bridge::lightTargetFor(&driving, tuning);
    CHECK(nan.level >= 0.0 && nan.level <= 15.0 && nan.heat >= 0.0 && nan.heat <= 1.0);
}

void testYokeLightColourRunsFromTheUsersOwnThroughAmberToRed()
{
    const bridge::Rgb green{0, 255, 0};
    CHECK(bridge::YokeLights::colorAt(green, 0.0) == green);
    CHECK(bridge::YokeLights::colorAt(green, 0.5) == (bridge::Rgb{255, 150, 0}));
    CHECK(bridge::YokeLights::colorAt(green, 1.0) == (bridge::Rgb{255, 0, 0}));
    // In between it blends: a quarter of the way is half way from green to amber.
    CHECK(bridge::YokeLights::colorAt(green, 0.25) == (bridge::Rgb{128, 203, 0}));
    // Any starting colour works, and nonsense is held to the ends.
    const bridge::Rgb blue{0, 0, 255};
    CHECK(bridge::YokeLights::colorAt(blue, 0.0) == blue);
    CHECK(bridge::YokeLights::colorAt(blue, 1.0) == (bridge::Rgb{255, 0, 0}));
    CHECK(bridge::YokeLights::colorAt(blue, -3.0) == blue);
    CHECK(bridge::YokeLights::colorAt(blue, 9.0) == (bridge::Rgb{255, 0, 0}));
    CHECK(bridge::YokeLights::colorAt(blue, std::nan("")) == blue);
}

void testYokeLightsChangeAndRestoreTheHandlesSettings()
{
    using Clock = bridge::YokeLights::Clock;
    constexpr int kBrightnessParameter = 4011;
    constexpr int kColorParameter      = 4012;

    Fixture fixture;
    const auto writes     = [] { return fake_moza::state().parameterWrites; };
    const auto colorWrites = [] { return fake_moza::state().listWrites; };
    const auto rgb         = [](long long r, long long g, long long b) { return std::vector<long long>{r, g, b}; };

    // No handle connected: nothing to attach to, and showing a level writes nothing.
    {
        bridge::YokeLights lights;
        CHECK(!lights.attach());
        lights.show(5.0, 0.0, Clock::now());
        CHECK(writes().empty());
    }

    fake_moza::state().handlePresent                    = true;
    fake_moza::state().parameters[kBrightnessParameter] = 12;
    fake_moza::state().listParameters[kColorParameter]  = rgb(0, 255, 0);
    Clock::time_point when = Clock::now();
    const auto        later = [&when] { return when += std::chrono::seconds(1); };
    {
        bridge::YokeLights lights;
        CHECK(lights.attach());
        CHECK(lights.original() == 12 && lights.current() == 12);
        CHECK(lights.color() == (bridge::Rgb{0, 255, 0}));
        CHECK(writes().empty()); // attaching only reads

        // The first change goes out at once.
        lights.show(3.0, 0.0, when);
        CHECK(writes().size() == 1 && writes().back() == std::make_pair(kBrightnessParameter, 3LL));

        // A request hovering around the same step writes nothing more.
        lights.show(3.4, 0.0, later());
        lights.show(2.7, 0.02, later());
        CHECK(writes().size() == 1);

        // A real change is followed, but not faster than a few times a second.
        const Clock::time_point burst = later();
        lights.show(8.0, 0.0, burst);
        CHECK(writes().size() == 2 && writes().back().second == 8);
        lights.show(15.0, 0.0, burst + std::chrono::milliseconds(100));
        CHECK(writes().size() == 2);
        lights.show(15.0, 0.0, burst + std::chrono::milliseconds(500));
        CHECK(writes().size() == 3 && writes().back().second == 15);
        when = burst + std::chrono::milliseconds(500);

        // The colour shifts in steps: half heat is step 4 of 8, which is amber.
        CHECK(colorWrites().empty());
        lights.show(15.0, 0.5, later());
        CHECK(colorWrites().size() == 1 && colorWrites().back() == std::make_pair(kColorParameter, rgb(255, 150, 0)));
        CHECK(lights.color() == (bridge::Rgb{255, 150, 0}));
        // Wandering within a step writes nothing; a full step up does; flat out is exactly red.
        lights.show(15.0, 0.55, later());
        lights.show(15.0, 0.47, later());
        CHECK(colorWrites().size() == 1);
        lights.show(15.0, 0.75, later());
        CHECK(colorWrites().size() == 2);
        lights.show(15.0, 1.0, later());
        CHECK(colorWrites().back().second == rgb(255, 0, 0));

        // Brightness and colour both due: one write per call, brightness first.
        const auto levelWritesBefore = writes().size() - colorWrites().size();
        lights.show(6.0, 0.0, later());
        CHECK(writes().size() - colorWrites().size() == levelWritesBefore + 1);
        CHECK(colorWrites().back().second == rgb(255, 0, 0));
        lights.show(6.0, 0.0, later());
        CHECK(colorWrites().back().second == rgb(0, 255, 0));

        // Fully off is reached exactly, and out-of-range requests are held to the handle's range.
        lights.show(0.2, 0.0, later());
        CHECK(lights.current() == 0);
        lights.show(99.0, 0.0, later());
        CHECK(lights.current() == 15);
        lights.show(std::nan(""), 0.0, later());
        CHECK(lights.current() == 15);

        // "As they were found" puts back the brightness, and the colour whatever heat is asked for.
        lights.show(15.0, 1.0, later());
        CHECK(lights.color() == (bridge::Rgb{255, 0, 0}));
        lights.show(-1.0, 1.0, later());
        CHECK(lights.current() == 12);
        lights.show(-1.0, 1.0, later());
        CHECK(lights.color() == (bridge::Rgb{0, 255, 0}));

        lights.show(4.0, 0.0, later());
        lights.show(4.0, 1.0, later());
        CHECK(lights.current() == 4 && lights.color() == (bridge::Rgb{255, 0, 0}));
    }
    // Destroyed while changed: both originals are written back.
    CHECK(fake_moza::state().parameters[kBrightnessParameter] == 12);
    CHECK(fake_moza::state().listParameters[kColorParameter] == rgb(0, 255, 0));

    // Left unchanged, nothing is written on the way out.
    const auto before = writes().size();
    {
        bridge::YokeLights untouched;
        CHECK(untouched.attach());
    }
    CHECK(writes().size() == before);

    // A handle whose colour cannot be read still gets brightness, and its colour is never written.
    fake_moza::state().listParameters.erase(kColorParameter);
    const auto colorsBefore = colorWrites().size();
    {
        bridge::YokeLights noColor;
        CHECK(noColor.attach());
        noColor.show(5.0, 1.0, later());
        noColor.show(5.0, 1.0, later());
        CHECK(noColor.current() == 5);
    }
    CHECK(colorWrites().size() == colorsBefore);
    CHECK(fake_moza::state().parameters[kBrightnessParameter] == 12);

    // A handle whose brightness cannot be read is left alone entirely: there would be nothing to put back.
    fake_moza::state().parameters.erase(kBrightnessParameter);
    const auto untouchedWrites = writes().size();
    {
        bridge::YokeLights unreadable;
        CHECK(!unreadable.attach());
        unreadable.show(5.0, 1.0, later());
    }
    CHECK(writes().size() == untouchedWrites);
}

void testFormatValue()
{
    Fixture fixture;
    CHECK(bridge::formatValue(moza::Value(7)) == "7");
    CHECK(bridge::formatValue(moza::Value(std::vector<moza::Value>{moza::Value(1), moza::Value(50u)})) == "[1, 50]");
    CHECK(bridge::formatValue(moza::Value()) == "<none>");
}

} // namespace

int main()
{
    testSpringAndDamperCommandStream();
    testConstantForceCommand();
    testPeriodicCommand();
    testStopIsIdempotent();
    testControlTimeoutIsTolerated();
    testControlErrorFailsBegin();
    testRefusedEffectReturnsZero();
    testFindFlightBaseWaitsForEnumeration();
    testConditionFromPercent();
    testFormatValue();
    testLightsFollowEachMachinesOwnEngineRange();
    testYokeLightColourRunsFromTheUsersOwnThroughAmberToRed();
    testYokeLightsChangeAndRestoreTheHandlesSettings();
    testPresenceFileBeatsWhileAliveAndIsRemovedAfterwards();
    testPresenceFileStaysQuietWithoutTheModsFolder();
    testDeviceForcesScaleAndRoute();
    testDeviceForcesAreZeroWhenForcesAreOff();
    testDeviceForcesSurviveHostileInput();
    testStrokeReachesBothEndsAndReturnsToCenter();
    testStrokeGivesUpOnAnEndItCannotReach();
    testStrokeEndsEvenIfTheAxisNeverMoves();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
