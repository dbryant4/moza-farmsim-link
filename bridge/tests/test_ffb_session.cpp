// Checks the exact command stream the bridge sends, against the in-process fake SDK.

#include "fake_moza_sdk.hpp"
#include "ffb_session.hpp"
#include "link.hpp"
#include "presence.hpp"
#include "stroke.hpp"

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
