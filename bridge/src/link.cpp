#include "link.hpp"

#include "settings.hpp"
#include "status_board.hpp"
#include "telemetry.hpp"
#include "web_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace bridge {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kKmhPerMs = 3.6;

/** One force-model value as the settings page presents it: the stored field multiplied by `scale`. */
struct Tunable
{
    const char* key;
    const char* group;
    const char* label;
    const char* description;
    const char* unit;
    double      minimum;
    double      maximum;
    double      step;
    double ForceSettings::* field;
    double      scale;
};

const Tunable kTunables[] = {
    {"steering.centering", "Steering", "Centering at speed", "How strongly the wheel returns to center at road speed.", "%", 0, 100, 1,
     &ForceSettings::rollSpringMax, 100},
    {"steering.centering_stopped", "Steering", "Centering when stopped",
     "Centering at a standstill. Zero leaves the wheel where you put it, like a parked tractor.", "%", 0, 100, 1,
     &ForceSettings::rollSpringStopped, 100},
    {"steering.centering_full_speed", "Steering", "Speed for full centering", "Centering builds up from standstill to this speed.", "km/h",
     5, 80, 1, &ForceSettings::rollSpringFullSpeed, kKmhPerMs},
    {"steering.weight", "Steering", "Weight", "Resistance to turning the wheel that is always there while driving.", "%", 0, 100, 1,
     &ForceSettings::rollDamperBase, 100},
    {"steering.weight_load", "Steering", "Extra weight from load", "Added resistance as the vehicle and its implements get heavier.", "%",
     0, 100, 1, &ForceSettings::rollDamperMass, 100},
    {"steering.weight_stopped", "Steering", "Extra weight when stopped", "Added resistance at a standstill, where the tyres scrub.", "%", 0,
     100, 1, &ForceSettings::rollDamperStandstill, 100},
    {"steering.self_align", "Steering", "Self-aligning pull", "Pull toward center that grows with speed and lock, and fades when the wheels slip.",
     "%", 0, 100, 1, &ForceSettings::selfAlign, 100},
    {"steering.implement_pull", "Steering", "Implement pull", "Extra pull toward center while a lowered implement works the ground.", "%", 0,
     100, 1, &ForceSettings::implementDrag, 100},
    {"steering.pull_limit", "Steering", "Pull limit", "Cap on the two pulls above combined.", "%", 0, 100, 1, &ForceSettings::rollForceLimit,
     100},

    {"rumble.surface", "Rumble", "Surface rumble", "Vibration on the roughest ground at speed; smooth roads give none.", "%", 0, 100, 1,
     &ForceSettings::rumbleSurface, 100},
    {"rumble.full_speed", "Rumble", "Speed for full surface rumble", "Surface rumble builds up from standstill to this speed.", "km/h", 2,
     60, 1, &ForceSettings::rumbleFullSpeed, kKmhPerMs},
    {"rumble.working", "Rumble", "Extra while working", "How much stronger the rumble gets while an implement works the ground.", "%", 0, 200,
     5, &ForceSettings::rumbleWorkingBoost, 100},
    {"rumble.engine", "Rumble", "Engine vibration", "Vibration from the running engine, including at a standstill.", "%", 0, 100, 1,
     &ForceSettings::engineRumble, 100},
    {"rumble.limit", "Rumble", "Rumble limit", "Cap on all vibration combined.", "%", 0, 100, 1, &ForceSettings::rumbleMax, 100},
    {"rumble.slow_hz", "Rumble", "Frequency when slow", "Vibration frequency at walking pace.", "Hz", 2, 60, 1, &ForceSettings::rumbleMinHz, 1},
    {"rumble.fast_hz", "Rumble", "Frequency when fast", "Vibration frequency at road speed.", "Hz", 2, 60, 1, &ForceSettings::rumbleMaxHz, 1},

    {"pitch.hold", "Pitch (in/out)", "Hold stiffness", "How firmly the yoke is held at its center in and out while driving.", "%", 0, 100, 1,
     &ForceSettings::pitchSpring, 100},
    {"pitch.damping", "Pitch (in/out)", "Damping", "Resistance to pushing and pulling the yoke while driving.", "%", 0, 100, 1,
     &ForceSettings::pitchDamper, 100},

    {"idle.centering", "Out of the vehicle", "Centering", "Centering on both axes while on foot, in a menu, or with the game closed.", "%", 0,
     100, 1, &ForceSettings::idleSpring, 100},
    {"idle.damping", "Out of the vehicle", "Damping", "Resistance on both axes in the same situations.", "%", 0, 100, 1,
     &ForceSettings::idleDamper, 100},
};

constexpr const char* kStrengthKey     = "general.strength";
constexpr const char* kFlipPullKey     = "general.flip_pull";
constexpr const char* kRumbleOnRollKey = "rumble.on_steering";

void registerSettings(Settings& settings)
{
    settings.addNumber(kStrengthKey, "General", "Overall strength", "Scales every force the bridge applies.", "%", 0, 100, 1, 100);
    settings.addToggle(kFlipPullKey, "General", "Reverse steering pull",
                       "Turn on if the self-aligning pull pushes the wheel away from center instead of toward it.", false);

    const ForceSettings defaults;
    for (const Tunable& tunable : kTunables)
    {
        settings.addNumber(tunable.key, tunable.group, tunable.label, tunable.description, tunable.unit, tunable.minimum, tunable.maximum,
                           tunable.step, defaults.*tunable.field * tunable.scale);
    }

    settings.addToggle(kRumbleOnRollKey, "Rumble", "Rumble through the steering axis",
                       "Off sends rumble through in/out instead, which you feel in the grips without it steering the vehicle.", false);
}

ForceSettings forceSettingsFrom(const Settings& settings)
{
    ForceSettings result;
    for (const Tunable& tunable : kTunables)
    {
        result.*tunable.field = settings.get(tunable.key) / tunable.scale;
    }
    return result;
}

LinkTuning tuningFrom(const Settings& settings, const StatusBoard& status, int fullScale)
{
    LinkTuning tuning;
    tuning.strength     = status.forcesEnabled() ? settings.get(kStrengthKey) / 100.0 : 0.0;
    tuning.flipPull     = settings.enabled(kFlipPullKey);
    tuning.rumbleOnRoll = settings.enabled(kRumbleOnRollKey);
    tuning.fullScale    = fullScale;
    return tuning;
}

void registerStatus(StatusBoard& status)
{
    status.add("game.telemetry", "Game", "Telemetry");
    status.addRanged("game.speed", "Game", "Speed", "km/h", -30, 60, 1);
    status.addRanged("game.steer", "Game", "Steering", "", -1, 1, 2);
    status.add("game.mass", "Game", "Total mass", "t", 1);
    status.addRanged("game.rough", "Game", "Surface roughness", "", 0, 1, 2);
    status.addRanged("game.slip", "Game", "Wheel slip", "", 0, 1, 2);
    status.add("game.implement", "Game", "Implement");

    status.addRanged("yoke.roll", "Yoke", "Roll position", "", -1, 1, 3);
    status.addRanged("yoke.pitch", "Yoke", "Pitch position", "", -1, 1, 3);
    status.addRanged("yoke.roll_torque", "Yoke", "Roll motor", "N\xC2\xB7m", -0.5, 0.5, 3);
    status.addRanged("yoke.pitch_torque", "Yoke", "Pitch motor", "N\xC2\xB7m", -0.5, 0.5, 3);

    status.addRanged("force.centering", "Forces", "Centering", "%", 0, 100, 0);
    status.addRanged("force.weight", "Forces", "Weight", "%", 0, 100, 0);
    status.addRanged("force.pull", "Forces", "Pull", "%", -100, 100, 0);
    status.addRanged("force.rumble", "Forces", "Rumble", "%", 0, 100, 0);
    status.add("force.rumble_hz", "Forces", "Rumble frequency", "Hz", 0);
    status.addRanged("force.pitch_hold", "Forces", "Pitch hold", "%", 0, 100, 0);
    status.add("bridge.send_errors", "Bridge", "Commands that failed to send", "", 0);
}

std::int16_t toRaw(double fraction, int fullScale)
{
    const double raw = std::isfinite(fraction) ? fraction * fullScale : 0.0;
    return static_cast<std::int16_t>(std::clamp(std::lround(raw), -32767L, 32767L));
}

Condition condition(std::int16_t coefficient, int fullScale)
{
    Condition result;
    result.coefficient = coefficient;
    result.saturation  = static_cast<std::uint16_t>(std::clamp(fullScale, 0, 65535));
    return result;
}

/** Latest cached torque reading in N*m, or NaN. The SDK hands these back as doubles already in N*m. */
double cachedTorque(const moza::RawParameter& parameter)
{
    auto [err, value] = parameter.getValue();
    if (err != MOZA_ERR_NO_ERROR)
    {
        return std::nan("");
    }
    return value.isFloat() ? value.asDouble() : value.asInt() / 10000.0;
}

const char* telemetryText(const TelemetryView& view)
{
    switch (view.state)
    {
    case TelemetryState::Fresh: return view.sample.active ? "live, driving" : "live, not in a vehicle";
    case TelemetryState::Stale: return "paused or stopped";
    default: return "waiting for the game";
    }
}

/** The effect blocks the link keeps registered on the base. */
struct Effects
{
    std::uint8_t spring = 0;
    std::uint8_t damper = 0;
    std::uint8_t pull   = 0;
    std::uint8_t rumble = 0;

    bool registerAll(FfbSession& ffb)
    {
        // begin() frees everything on the base first, so this also recovers effects another program wiped.
        return ffb.begin() && (spring = ffb.addEffect(EffectType::Spring)) != 0 && (damper = ffb.addEffect(EffectType::Damper)) != 0 &&
               (pull = ffb.addEffect(EffectType::ConstantForce)) != 0 && (rumble = ffb.addEffect(EffectType::Sine)) != 0;
    }
};

// A sine restarts its waveform when Start is re-sent, so it gets a longer duration than the other
// effects and is restarted half as often as that duration, trading a slower lapse for fewer blips.
constexpr std::uint16_t kRumbleDurationMs = 2000;

/** Sends whatever differs from what the base last received; with @p everything, all of it plus Start. */
bool send(FfbSession& ffb, const Effects& effects, const DeviceForces& now, DeviceForces& sent, bool everything, bool restartRumble,
          int fullScale)
{
    // Half a percent of full scale: below this a change cannot be felt and is not worth a command.
    const int  threshold = std::max(1, fullScale / 200);
    const auto differs   = [&](std::int16_t a, std::int16_t b) { return everything || std::abs(a - b) >= threshold || (a == 0) != (b == 0); };

    bool ok = true;
    if (differs(now.rollSpring, sent.rollSpring))
    {
        ok &= ffb.setCondition(effects.spring, Axis::Roll, condition(now.rollSpring, fullScale));
        sent.rollSpring = now.rollSpring;
    }
    if (differs(now.pitchSpring, sent.pitchSpring))
    {
        ok &= ffb.setCondition(effects.spring, Axis::Pitch, condition(now.pitchSpring, fullScale));
        sent.pitchSpring = now.pitchSpring;
    }
    if (differs(now.rollDamper, sent.rollDamper))
    {
        ok &= ffb.setCondition(effects.damper, Axis::Roll, condition(now.rollDamper, fullScale));
        sent.rollDamper = now.rollDamper;
    }
    if (differs(now.pitchDamper, sent.pitchDamper))
    {
        ok &= ffb.setCondition(effects.damper, Axis::Pitch, condition(now.pitchDamper, fullScale));
        sent.pitchDamper = now.pitchDamper;
    }
    if (differs(now.pull, sent.pull))
    {
        ok &= ffb.setConstant(effects.pull, now.pull, forceAlong(Axis::Roll));
        sent.pull = now.pull;
    }
    if (restartRumble || differs(now.rumble, sent.rumble) || now.rumblePeriodMs != sent.rumblePeriodMs || now.rumbleAxis != sent.rumbleAxis)
    {
        ok &= ffb.setPeriodic(effects.rumble, now.rumble, now.rumblePeriodMs, forceAlong(now.rumbleAxis), kRumbleDurationMs);
        sent.rumble         = now.rumble;
        sent.rumblePeriodMs = now.rumblePeriodMs;
        sent.rumbleAxis     = now.rumbleAxis;
    }

    if (everything)
    {
        ok &= ffb.start(effects.spring);
        ok &= ffb.start(effects.damper);
        ok &= ffb.start(effects.pull);
    }
    if (restartRumble)
    {
        ok &= ffb.start(effects.rumble);
    }
    return ok;
}

} // namespace

DeviceForces toDeviceForces(const ForceOutput& output, const LinkTuning& tuning)
{
    const double strength = std::clamp(std::isfinite(tuning.strength) ? tuning.strength : 0.0, 0.0, 1.0);

    DeviceForces forces;
    forces.rollSpring  = toRaw(std::clamp(output.rollSpring, 0.0, 1.0) * strength, tuning.fullScale);
    forces.pitchSpring = toRaw(std::clamp(output.pitchSpring, 0.0, 1.0) * strength, tuning.fullScale);
    forces.rollDamper  = toRaw(std::clamp(output.rollDamper, 0.0, 1.0) * strength, tuning.fullScale);
    forces.pitchDamper = toRaw(std::clamp(output.pitchDamper, 0.0, 1.0) * strength, tuning.fullScale);

    // The model's positive is "toward steering right"; on the AY210 a positive magnitude moves the
    // reported roll position down, i.e. left. Hence the minus, unless the user flips it.
    const double pull = std::clamp(output.rollForce, -1.0, 1.0) * strength * (tuning.flipPull ? 1.0 : -1.0);
    forces.pull       = toRaw(pull, tuning.fullScale);

    forces.rumble     = toRaw(std::clamp(output.rumble, 0.0, 1.0) * strength, tuning.fullScale);
    forces.rumbleAxis = tuning.rumbleOnRoll ? Axis::Roll : Axis::Pitch;
    if (std::isfinite(output.rumbleHz) && output.rumbleHz >= 1.0)
    {
        forces.rumblePeriodMs = static_cast<std::uint32_t>(std::clamp(std::lround(1000.0 / output.rumbleHz), 5L, 1000L));
    }
    return forces;
}

int runLink(moza::as::Wheelbase& base, const moza::HidDevice& hid, const LinkOptions& options, const std::atomic<bool>& stop)
{
    Settings settings;
    registerSettings(settings);
    const Settings::LoadResult loaded = settings.load(options.settingsPath);
    std::printf("Settings: %s (%s)\n", options.settingsPath.string().c_str(),
                !loaded.ok ? "could not be read, using defaults" : loaded.fileFound ? "loaded" : "not there yet, using defaults");
    for (const std::string& warning : loaded.warnings)
    {
        std::fprintf(stderr, "  %s\n", warning.c_str());
    }

    StatusBoard status;
    registerStatus(status);

    WebUi ui(settings, status, options.settingsPath);
    if (options.uiPort > 0)
    {
        std::string error;
        if (ui.start(static_cast<std::uint16_t>(options.uiPort), &error))
        {
            std::printf("Settings page: http://127.0.0.1:%d/\n", static_cast<int>(ui.port()));
        }
        else
        {
            std::fprintf(stderr, "The settings page could not start (%s). Carrying on without it.\n", error.c_str());
        }
    }

    TelemetryReader reader(options.telemetryPath);
    std::printf("Telemetry file: %s\n", options.telemetryPath.empty() ? "(none: no default location on this system)"
                                                                       : options.telemetryPath.string().c_str());

    ForceModel    model(forceSettingsFrom(settings));
    std::uint64_t settingsRevision = settings.revision();

    FfbSession ffb(base, 1000, options.verbose);
    Effects    effects;
    if (!effects.registerAll(ffb))
    {
        return 4;
    }
    std::puts("Linked. Forces follow the game; with no game they stay at the idle settings. Ctrl+C releases them.");

    const auto modelInterval   = std::chrono::milliseconds(16);
    const auto sendInterval    = std::chrono::milliseconds(50);
    const auto refreshInterval = std::chrono::milliseconds(200);
    const auto rumbleInterval  = std::chrono::milliseconds(kRumbleDurationMs / 2);
    const auto statusInterval  = std::chrono::milliseconds(100);
    const auto torqueInterval  = std::chrono::milliseconds(250);
    const auto printInterval   = std::chrono::seconds(1);

    const moza::RawParameter torqueX = base.steerTorqueOutX().raw();
    const moza::RawParameter torqueY = base.steerTorqueOutY().raw();

    const auto   start       = Clock::now();
    auto         lastModel   = start;
    auto         nextModel   = start;
    auto         nextSend    = start;
    auto         nextRefresh = start;
    auto         nextRumble  = start;
    auto         nextStatus  = start;
    auto         nextTorque  = start;
    auto         nextPrint   = start;
    ForceOutput  output      = model.output();
    DeviceForces sent;
    int          sendErrors    = 0;
    bool         forcesWereOn  = status.forcesEnabled();
    TelemetryView view;

    while (!stop)
    {
        const auto now = Clock::now();
        if (options.seconds > 0 && std::chrono::duration<double>(now - start).count() >= options.seconds)
        {
            break;
        }

        if (now >= nextModel)
        {
            if (settings.revision() != settingsRevision)
            {
                settingsRevision = settings.revision();
                model.setSettings(forceSettingsFrom(settings));
            }
            view      = reader.poll(now);
            output    = model.update(view.fresh(), std::chrono::duration<double>(now - lastModel).count());
            lastModel = now;
            nextModel = now + modelInterval;
        }

        const bool forcesOn = status.forcesEnabled();
        if (forcesOn && !forcesWereOn)
        {
            // Coming back from "off" is also the user's way to recover if something else cleared the base.
            if (!effects.registerAll(ffb))
            {
                ++sendErrors;
            }
            nextRefresh = now;
            nextRumble  = now;
        }
        const bool justSwitchedOff = !forcesOn && forcesWereOn;
        forcesWereOn               = forcesOn;

        if (now >= nextSend || justSwitchedOff)
        {
            const bool everything    = now >= nextRefresh || justSwitchedOff;
            const bool restartRumble = now >= nextRumble;
            if (!send(ffb, effects, toDeviceForces(output, tuningFrom(settings, status, options.fullScale)), sent, everything, restartRumble,
                      options.fullScale))
            {
                ++sendErrors;
            }
            nextSend = now + sendInterval;
            if (everything)
            {
                nextRefresh = now + refreshInterval;
            }
            if (restartRumble)
            {
                nextRumble = now + rumbleInterval;
            }
        }

        if (now >= nextTorque)
        {
            // Asynchronous: the reply lands in the SDK's cache and is picked up by a later status update.
            torqueX.fetch();
            torqueY.fetch();
            nextTorque = now + torqueInterval;
        }

        if (now >= nextStatus)
        {
            const Telemetry& sample = view.sample;
            const bool       live   = view.state == TelemetryState::Fresh && sample.active;
            status.setText("game.telemetry", telemetryText(view));
            status.set("game.speed", live ? sample.speed * kKmhPerMs : 0.0);
            status.set("game.steer", live ? sample.steer : 0.0);
            status.set("game.mass", live ? sample.mass / 1000.0 : 0.0);
            status.set("game.rough", live ? sample.rough : 0.0);
            status.set("game.slip", live ? sample.slip : 0.0);
            status.setText("game.implement", !live ? "-" : sample.working ? "working" : sample.lowered ? "lowered" : "raised or none");

            if (hid)
            {
                auto [positionErr, state] = hid.getInputState();
                if (positionErr == MOZA_ERR_NO_ERROR)
                {
                    status.set("yoke.roll", (state.axes[MOZA_HA_X].normalizedValue - 0.5) * 2.0);
                    status.set("yoke.pitch", (state.axes[MOZA_HA_Y].normalizedValue - 0.5) * 2.0);
                }
            }
            status.set("yoke.roll_torque", cachedTorque(torqueX));
            status.set("yoke.pitch_torque", cachedTorque(torqueY));

            const double scale = 100.0 / std::max(1, options.fullScale);
            status.set("force.centering", sent.rollSpring * scale);
            status.set("force.weight", sent.rollDamper * scale);
            status.set("force.pull", -sent.pull * scale * (settings.enabled(kFlipPullKey) ? -1.0 : 1.0));
            status.set("force.rumble", sent.rumble * scale);
            status.set("force.rumble_hz", 1000.0 / std::max<std::uint32_t>(1, sent.rumblePeriodMs));
            status.set("force.pitch_hold", sent.pitchSpring * scale);
            status.set("bridge.send_errors", sendErrors);
            nextStatus = now + statusInterval;
        }

        if (now >= nextPrint)
        {
            const double scale = 100.0 / std::max(1, options.fullScale);
            std::printf("[%6.0fs] %-22s | %5.1f km/h  steer %+.2f | centering %3.0f%%  weight %3.0f%%  pull %+4.0f%%  rumble %3.0f%% | forces %s\n",
                        std::chrono::duration<double>(now - start).count(), telemetryText(view), view.sample.speed * kKmhPerMs,
                        view.sample.steer, sent.rollSpring * scale, sent.rollDamper * scale,
                        -sent.pull * scale * (settings.enabled(kFlipPullKey) ? -1.0 : 1.0), sent.rumble * scale,
                        forcesOn ? "on" : "OFF");
            nextPrint = now + printInterval;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ui.stop();
    ffb.stop();
    std::printf("Forces released. Updates that failed to send: %d.\n", sendErrors);
    return 0;
}

} // namespace bridge
