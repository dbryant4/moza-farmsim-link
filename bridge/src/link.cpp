#include "link.hpp"

#include "pitch_servo.hpp"
#include "presence.hpp"
#include "settings.hpp"
#include "status_board.hpp"
#include "telemetry.hpp"
#include "web_ui.hpp"
#include "yoke_lights.hpp"

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
    {"rumble.engine", "Rumble", "Engine vibration",
     "Vibration from a running combustion engine: gentle and slow at idle, stronger and faster as the revs, the load and the "
     "speed rise. This is its strength flat out. Electric vehicles have none.",
     "%", 0, 100, 1, &ForceSettings::engineRumble, 100},
    {"rumble.engine_idle", "Rumble", "Engine vibration at idle",
     "How much of the engine vibration is left with the engine idling at a standstill.", "%", 0, 100, 5,
     &ForceSettings::engineRumbleIdleShare, 100},
    {"rumble.engine_start", "Rumble", "Engine start shake",
     "Extra shake while a combustion engine is being started, fading once it runs. Electric vehicles have none.", "%", 0, 100, 1,
     &ForceSettings::engineStartShake, 100},
    {"rumble.engine_start_share", "Rumble", "Engine start shake length",
     "How much of the machine's own start the shake lasts for. Each machine takes its own time to start; 100 shakes all the way "
     "through it.",
     "% of the start", 10, 100, 5, &ForceSettings::engineStartShare, 100},
    {"rumble.limit", "Rumble", "Rumble limit", "Cap on surface and engine vibration combined; the start shake comes on top.", "%", 0, 100,
     1, &ForceSettings::rumbleMax, 100},
    {"rumble.slow_hz", "Rumble", "Frequency when slow", "Vibration frequency at walking pace.", "Hz", 2, 60, 1, &ForceSettings::rumbleMinHz, 1},
    {"rumble.fast_hz", "Rumble", "Frequency when fast", "Vibration frequency at road speed.", "Hz", 2, 60, 1, &ForceSettings::rumbleMaxHz, 1},

    {"surfaces.road", "Surfaces", "Asphalt and concrete",
     "Paved ground is smooth, so this starts at nothing. Raising it gives a faint, fast buzz at speed.", "%", 0, 200, 5,
     &ForceSettings::surfaceRoad, 100},
    {"surfaces.gravel", "Surfaces", "Gravel and dirt tracks", "A fast, gritty buzz that rises with speed.", "%", 0, 200, 5,
     &ForceSettings::surfaceGravel, 100},
    {"surfaces.grass", "Surfaces", "Grass", "Low and soft, in long swells.", "%", 0, 200, 5, &ForceSettings::surfaceGrass, 100},
    {"surfaces.snow", "Surfaces", "Snow", "Lower and more muffled than grass.", "%", 0, 200, 5, &ForceSettings::surfaceSnow, 100},
    {"surfaces.field", "Surfaces", "Fields: sown, rolled or stubble", "A medium, steady rumble.", "%", 0, 200, 5,
     &ForceSettings::surfaceField, 100},
    {"surfaces.tilled", "Surfaces", "Fields: ploughed or cultivated", "Slow and heavy, coming and going clod by clod.", "%", 0, 200, 5,
     &ForceSettings::surfaceTilled, 100},
    {"surfaces.ridge", "Surfaces", "Fields: ridges", "A pulse for each ridge crossed, faster as you speed up.", "%", 0, 200, 5,
     &ForceSettings::surfaceRidge, 100},
    {"surfaces.heap", "Surfaces", "Heaps of tipped material", "A slow lurching while driving over a heap.", "%", 0, 200, 5,
     &ForceSettings::surfaceHeap, 100},
    {"surfaces.tug", "Surfaces", "Tug on broken ground",
     "Small irregular pulls on the wheel on ploughed, cultivated and ridged fields. Heavier machines get less. 0 turns it off.", "%", 0,
     50, 1, &ForceSettings::fieldTug, 100},

    {"bumps.kick", "Bumps", "Obstacle kick",
     "Jerk of the wheel toward the side whose front wheel hits something like a log or a kerb. Heavier machines get less.", "%", 0, 100,
     1, &ForceSettings::bumpKick, 100},
    {"bumps.thump", "Bumps", "Thump (in and out)",
     "In-and-out shake when both front wheels are jolted together, as on rail tracks.", "%", 0, 100, 1, &ForceSettings::bumpThump, 100},
    {"bumps.wiggle", "Bumps", "Wiggle (left and right)",
     "Left-right shake of the wheel for the same kind of bump. Heavier machines get less.", "%", 0, 100, 1, &ForceSettings::bumpWiggle,
     100},
    {"bumps.shake_hz", "Bumps", "Shake frequency", "How fast the thump and the wiggle go back and forth.", "Hz", 2, 30, 0.5,
     &ForceSettings::bumpShakeHz, 1},
    {"bumps.mass_half", "Bumps", "Mass that halves kick and wiggle",
     "Total mass, with implements, at which the kick and the wiggle are down to half. Lower makes big machines calmer.", "t", 1, 50, 0.5,
     &ForceSettings::bumpKickMassHalf, 0.001},

    {"movement.rock", "Machine movement", "Rocking (wheel left and right)",
     "The wheel is pushed left and right as the machine itself rocks from side to side, the way the cab sways off road. 0 turns it off.",
     "%", 0, 100, 1, &ForceSettings::bodyRock, 100},
    {"movement.bounce", "Machine movement", "Pitching and bouncing (yoke in and out)",
     "The yoke is pushed in and out as the machine pitches and bounces. It takes a fair amount before the yoke moves. 0 turns it off.",
     "%", 0, 100, 1, &ForceSettings::bodyBounce, 100},
    {"movement.rock_full", "Machine movement", "Rocking that gives the full push",
     "How fast the machine has to rock for the whole of the push above. Lower makes gentle ground count for more.", "deg/s", 5, 90, 1,
     &ForceSettings::bodyRockFullRate, 57.29577951308232},

    {"pitch.hold", "Pitch (in/out)", "Hold stiffness", "How firmly the yoke is held at its center in and out while driving.", "%", 0, 100, 1,
     &ForceSettings::pitchSpring, 100},
    {"pitch.damping", "Pitch (in/out)", "Damping", "Resistance to pushing and pulling the yoke while driving.", "%", 0, 100, 1,
     &ForceSettings::pitchDamper, 100},
    {"pitch.accel_sway", "Pitch (in/out)", "Acceleration sway",
     "How far the yoke leans out and in while pulling away and braking, as a share of its travel from center. 0 turns it off.", "%", 0,
     50, 1, &ForceSettings::accelSway, 100},

    {"idle.centering", "Out of the vehicle", "Centering", "Centering on both axes while on foot, in a menu, or with the game closed.", "%", 0,
     100, 1, &ForceSettings::idleSpring, 100},
    {"idle.damping", "Out of the vehicle", "Damping", "Resistance on both axes in the same situations.", "%", 0, 100, 1,
     &ForceSettings::idleDamper, 100},
};

constexpr const char* kStrengthKey     = "general.strength";
constexpr const char* kFlipPullKey     = "general.flip_pull";
constexpr const char* kRumbleOnRollKey = "rumble.on_steering";
constexpr const char* kFlipSwayKey     = "pitch.sway_reverse";
constexpr const char* kLightsKey       = "lights.follow_engine";
constexpr const char* kLightsLowKey    = "lights.low";
constexpr const char* kLightsRpmKey    = "lights.full_rpm";
constexpr const char* kLightsColorKey  = "lights.color_shift";
constexpr const char* kLightsFromKey   = "lights.color_from";
constexpr const char* kSwayKey         = "pitch.accel_sway";
constexpr const char* kUpdatesKey      = "updates.check";
constexpr const char* kPrereleasesKey  = "updates.prereleases";
constexpr const char* kPatternsKey     = "surfaces.patterns";

void registerSettings(Settings& settings)
{
    settings.addNumber(kStrengthKey, "General", "Overall strength", "Scales every force the bridge applies.", "%", 0, 100, 1, 100);
    settings.addToggle(kFlipPullKey, "General", "Reverse steering pull",
                       "Turn on if the self-aligning pull pushes the wheel away from center instead of toward it.", false);

    const ForceSettings defaults;
    for (const Tunable& tunable : kTunables)
    {
        // The switch for the surface patterns heads their group, wherever the table puts it.
        if (std::string_view(tunable.group) == "Surfaces" && !settings.contains(kPatternsKey))
        {
            settings.addToggle(kPatternsKey, "Surfaces", "A different feel for each surface",
                               "Off gives every surface the same plain rumble, stronger or weaker with how rough it is. The "
                               "strengths below multiply what Surface rumble gives.",
                               defaults.surfacePatterns);
        }
        settings.addNumber(tunable.key, tunable.group, tunable.label, tunable.description, tunable.unit, tunable.minimum, tunable.maximum,
                           tunable.step, defaults.*tunable.field * tunable.scale);
    }

    settings.addToggle(kFlipSwayKey, "Pitch (in/out)", "Reverse acceleration sway",
                       "Turn on if the yoke leans the wrong way when you pull away.", false);
    settings.addToggle(kLightsKey, "Yoke lights", "Lights follow the engine",
                       "Off with the engine off, dim at idle, brighter with engine speed. Changes the handle's stored brightness "
                       "while the link runs and puts it back when it stops. With MOZA's SDK 1.0.0.4 this has made the SDK lock up "
                       "now and then; the link then restarts itself, with forces gone for about fifteen seconds.",
                       false);
    settings.addNumber(kLightsLowKey, "Yoke lights", "Brightness at idle", "How bright the lights are with the engine idling.", "of 15", 1,
                       15, 1, LightTuning{}.low);
    settings.addToggle(kLightsColorKey, "Yoke lights", "Colour shifts at high revs",
                       "Near the top of the engine's range the lights move from your colour through amber to red.", true);
    settings.addNumber(kLightsFromKey, "Yoke lights", "Colour starts to shift at",
                       "How far through the engine's range, from idle to flat out, the colour begins to change.", "%", 0, 95, 5,
                       LightTuning{}.colorFrom * 100.0);
    settings.addNumber(kLightsRpmKey, "Yoke lights", "Full engine speed, if the game does not say",
                       "Each machine's own maximum engine speed is used when the game reports it; this is the fallback.", "rpm", 1000,
                       4000, 50, LightTuning{}.fullRpm);
    settings.addToggle(kRumbleOnRollKey, "Rumble", "Rumble through the steering axis",
                       "Off sends rumble through in/out instead, which you feel in the grips without it steering the vehicle.", false);
    settings.addToggle(kUpdatesKey, "Updates", "Look for new versions",
                       "Once a day the program by the clock asks GitHub whether a newer release exists and tells you. Nothing is "
                       "downloaded or installed until you pick it from the icon's menu.",
                       true);
    settings.addToggle(kPrereleasesKey, "Updates", "Include pre-releases",
                       "Also offer versions published as pre-releases: newer, and less tried. Turning this off again does not go "
                       "back to an older version; you stay on what you have until a newer normal release is out.",
                       false);
}

ForceSettings forceSettingsFrom(const Settings& settings)
{
    ForceSettings result;
    for (const Tunable& tunable : kTunables)
    {
        result.*tunable.field = settings.get(tunable.key) / tunable.scale;
    }
    result.surfacePatterns = settings.enabled(kPatternsKey);
    return result;
}

LinkTuning tuningFrom(const Settings& settings, const StatusBoard& status, int fullScale)
{
    LinkTuning tuning;
    tuning.strength     = status.forcesEnabled() ? settings.get(kStrengthKey) / 100.0 : 0.0;
    tuning.flipPull     = settings.enabled(kFlipPullKey);
    tuning.rumbleOnRoll = settings.enabled(kRumbleOnRollKey);
    tuning.flipSway     = settings.enabled(kFlipSwayKey);
    tuning.fullScale    = fullScale;
    return tuning;
}

LightTuning lightTuningFrom(const Settings& settings)
{
    LightTuning tuning;
    tuning.followEngine = settings.enabled(kLightsKey);
    tuning.low          = settings.get(kLightsLowKey);
    tuning.fullRpm      = settings.get(kLightsRpmKey);
    tuning.colorShift   = settings.enabled(kLightsColorKey);
    tuning.colorFrom    = settings.get(kLightsFromKey) / 100.0;
    return tuning;
}

void registerStatus(StatusBoard& status)
{
    status.add("game.telemetry", "Game", "Telemetry");
    status.addRanged("game.speed", "Game", "Speed", "km/h", -30, 60, 1);
    status.addRanged("game.steer", "Game", "Steering", "", -1, 1, 2);
    status.add("game.mass", "Game", "Total mass", "t", 1);
    status.add("game.surface", "Game", "Surface");
    status.addRanged("game.rough", "Game", "Surface roughness", "", 0, 1, 2);
    status.addRanged("game.slip", "Game", "Wheel slip", "", 0, 1, 2);
    status.addRanged("game.bump", "Game", "Wheel bump (left - / right +)", "", -1, 1, 2);
    status.addRanged("game.rock", "Game", "Rocking (right +)", "deg/s", -40, 40, 1);
    status.addRanged("game.pitching", "Game", "Pitching (nose up +)", "deg/s", -40, 40, 1);
    status.addRanged("game.heave", "Game", "Bouncing (up +)", "m/s\xC2\xB2", -8, 8, 1);
    status.add("game.implement", "Game", "Implement");
    status.add("game.engine", "Game", "Engine");
    status.add("game.start_time", "Game", "Engine takes to start", "s", 1);

    status.addRanged("yoke.roll", "Yoke", "Roll position", "", -1, 1, 3);
    status.addRanged("yoke.pitch", "Yoke", "Pitch position", "", -1, 1, 3);

    status.addRanged("force.centering", "Forces", "Centering", "%", 0, 100, 0);
    status.addRanged("force.weight", "Forces", "Weight", "%", 0, 100, 0);
    status.addRanged("force.pull", "Forces", "Pull", "%", -100, 100, 0);
    status.addRanged("force.rumble", "Forces", "Rumble", "%", 0, 100, 0);
    status.add("force.rumble_hz", "Forces", "Rumble frequency", "Hz", 0);
    status.addRanged("force.pitch_hold", "Forces", "Pitch hold", "%", 0, 100, 0);
    status.addRanged("game.accel", "Game", "Acceleration", "m/s\xC2\xB2", -5, 5, 2);
    status.addRanged("force.sway", "Forces", "Pitch sway target", "", -0.5, 0.5, 3);
    status.addRanged("force.pitch_push", "Forces", "Pitch push", "%", -100, 100, 0);
    status.addRanged("yoke.roll_torque", "Yoke", "Roll motor", "N\xC2\xB7m", -0.5, 0.5, 3);
    status.addRanged("yoke.pitch_torque", "Yoke", "Pitch motor", "N\xC2\xB7m", -0.5, 0.5, 3);
    status.add("yoke.lights", "Yoke", "Lights");
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

const char* surfaceLabel(Surface surface)
{
    switch (surface)
    {
    case Surface::Road: return "asphalt or concrete";
    case Surface::Gravel: return "gravel or dirt track";
    case Surface::Grass: return "grass";
    case Surface::Snow: return "snow";
    case Surface::Heap: return "heap";
    case Surface::Field: return "field";
    case Surface::Tilled: return "field, ploughed or cultivated";
    case Surface::Ridge: return "field, ridges";
    default: return "not reported";
    }
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
    std::uint8_t thump  = 0;
    std::uint8_t wiggle = 0;
    std::uint8_t push   = 0;

    bool registerAll(FfbSession& ffb)
    {
        // begin() frees everything on the base first, so this also recovers effects another program wiped.
        return ffb.begin() && (spring = ffb.addEffect(EffectType::Spring)) != 0 && (damper = ffb.addEffect(EffectType::Damper)) != 0 &&
               (pull = ffb.addEffect(EffectType::ConstantForce)) != 0 && (rumble = ffb.addEffect(EffectType::Sine)) != 0 &&
               (thump = ffb.addEffect(EffectType::Sine)) != 0 && (wiggle = ffb.addEffect(EffectType::Sine)) != 0 &&
               (push = ffb.addEffect(EffectType::ConstantForce)) != 0;
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
    if (differs(now.pitchSpring, sent.pitchSpring) || differs(now.pitchCenter, sent.pitchCenter))
    {
        Condition pitch = condition(now.pitchSpring, fullScale);
        pitch.center    = now.pitchCenter;
        ok &= ffb.setCondition(effects.spring, Axis::Pitch, pitch);
        sent.pitchSpring = now.pitchSpring;
        sent.pitchCenter = now.pitchCenter;
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
    if (differs(now.pitchPush, sent.pitchPush))
    {
        ok &= ffb.setConstant(effects.push, now.pitchPush, forceAlong(Axis::Pitch));
        sent.pitchPush = now.pitchPush;
    }
    if (restartRumble || differs(now.rumble, sent.rumble) || now.rumblePeriodMs != sent.rumblePeriodMs || now.rumbleAxis != sent.rumbleAxis)
    {
        ok &= ffb.setPeriodic(effects.rumble, now.rumble, now.rumblePeriodMs, forceAlong(now.rumbleAxis), kRumbleDurationMs);
        sent.rumble         = now.rumble;
        sent.rumblePeriodMs = now.rumblePeriodMs;
        sent.rumbleAxis     = now.rumbleAxis;
    }

    // The two bump shakes are separate sines so that they keep their own slow frequency whatever
    // the rumble is doing. They sit at zero almost all the time.
    const bool bumpPeriodChanged = now.bumpPeriodMs != sent.bumpPeriodMs;
    if (restartRumble || bumpPeriodChanged || differs(now.thump, sent.thump))
    {
        ok &= ffb.setPeriodic(effects.thump, now.thump, now.bumpPeriodMs, forceAlong(Axis::Pitch), kRumbleDurationMs);
        sent.thump = now.thump;
    }
    if (restartRumble || bumpPeriodChanged || differs(now.wiggle, sent.wiggle))
    {
        ok &= ffb.setPeriodic(effects.wiggle, now.wiggle, now.bumpPeriodMs, forceAlong(Axis::Roll), kRumbleDurationMs);
        sent.wiggle = now.wiggle;
    }
    sent.bumpPeriodMs = now.bumpPeriodMs;

    if (everything)
    {
        ok &= ffb.start(effects.spring);
        ok &= ffb.start(effects.damper);
        ok &= ffb.start(effects.pull);
        ok &= ffb.start(effects.push);
    }
    if (restartRumble)
    {
        ok &= ffb.start(effects.rumble);
        ok &= ffb.start(effects.thump);
        ok &= ffb.start(effects.wiggle);
    }
    return ok;
}

} // namespace

LightTarget lightTargetFor(const Telemetry* telemetry, const LightTuning& tuning)
{
    LightTarget target;
    if (!tuning.followEngine || telemetry == nullptr || !telemetry->active)
    {
        return target;
    }
    const double brightest = YokeLights::kBrightest;
    const double low       = std::clamp(std::isfinite(tuning.low) ? tuning.low : 0.0, 0.0, brightest);
    if (telemetry->starting)
    {
        target.level = low;
        return target;
    }
    if (!(telemetry->rpm > 0.0))
    {
        target.level = 0.0;
        return target;
    }

    // Each machine's own range when the game reports it: a small loader and a big tractor both
    // reach full brightness flat out, though one does it at 3000 rpm and the other at 1900.
    const bool   known = telemetry->rpmMax > telemetry->rpmMin && telemetry->rpmMin >= 0.0 && std::isfinite(telemetry->rpmMax);
    const double idle  = known ? telemetry->rpmMin : tuning.idleRpm;
    const double full  = known ? telemetry->rpmMax : tuning.fullRpm;
    const double share = full > idle ? std::clamp((telemetry->rpm - idle) / (full - idle), 0.0, 1.0) : 1.0;

    target.level = low + (brightest - low) * share;
    if (tuning.colorShift)
    {
        const double from = std::clamp(std::isfinite(tuning.colorFrom) ? tuning.colorFrom : 0.0, 0.0, 0.99);
        target.heat       = std::clamp((share - from) / (1.0 - from), 0.0, 1.0);
    }
    return target;
}

double pitchTargetFor(const ForceOutput& output, const LinkTuning& tuning)
{
    const double target = std::isfinite(output.pitchTarget) ? std::clamp(output.pitchTarget, -1.0, 1.0) : 0.0;
    return tuning.flipSway ? -target : target;
}

DeviceForces toDeviceForces(const ForceOutput& output, const LinkTuning& tuning, double pitchPush)
{
    const double strength = std::clamp(std::isfinite(tuning.strength) ? tuning.strength : 0.0, 0.0, 1.0);

    DeviceForces forces;
    forces.rollSpring  = toRaw(std::clamp(output.rollSpring, 0.0, 1.0) * strength, tuning.fullScale);
    forces.pitchSpring = toRaw(std::clamp(output.pitchSpring, 0.0, 1.0) * strength, tuning.fullScale);
    forces.rollDamper  = toRaw(std::clamp(output.rollDamper, 0.0, 1.0) * strength, tuning.fullScale);
    forces.pitchDamper = toRaw(std::clamp(output.pitchDamper, 0.0, 1.0) * strength, tuning.fullScale);

    // The model's positive is "toward steering right"; on the AY210 a positive magnitude moves the
    // reported roll position down, i.e. left. Hence the minus, unless the user flips it.
    const double kick = std::isfinite(output.rollKick) ? output.rollKick : 0.0;
    const double pull = std::clamp(output.rollForce + kick, -1.0, 1.0) * strength * (tuning.flipPull ? 1.0 : -1.0);
    forces.pull       = toRaw(pull, tuning.fullScale);

    const double texture = std::isfinite(output.rumbleTexture) ? std::clamp(output.rumbleTexture, 0.0, 1.0) : 1.0;
    forces.rumble        = toRaw(std::clamp(output.rumble, 0.0, 1.0) * texture * strength, tuning.fullScale);
    forces.rumbleAxis = tuning.rumbleOnRoll ? Axis::Roll : Axis::Pitch;
    if (std::isfinite(output.rumbleHz) && output.rumbleHz >= 1.0)
    {
        forces.rumblePeriodMs = static_cast<std::uint32_t>(std::clamp(std::lround(1000.0 / output.rumbleHz), 5L, 1000L));
    }

    // A spring center is a position, so it is not scaled by strength; measured on the AY210, a positive
    // center sits at a higher reported position while a positive constant force pushes toward a lower one.
    forces.pitchCenter = toRaw(pitchTargetFor(output, tuning), tuning.fullScale);
    // The machine's own pitching and bouncing comes on top of whatever is holding the yoke in place,
    // and turns round with the sway if the user has reversed that.
    const double bounce = (std::isfinite(output.pitchBounce) ? output.pitchBounce : 0.0) * (tuning.flipSway ? -1.0 : 1.0);
    const double push   = std::clamp((std::isfinite(pitchPush) ? pitchPush : 0.0) + bounce, -1.0, 1.0);
    forces.pitchPush    = toRaw(-push * strength, tuning.fullScale);

    forces.thump  = toRaw(std::clamp(output.pitchThump, 0.0, 1.0) * strength, tuning.fullScale);
    forces.wiggle = toRaw(std::clamp(output.rollWiggle, 0.0, 1.0) * strength, tuning.fullScale);
    if (std::isfinite(output.bumpHz) && output.bumpHz >= 1.0)
    {
        forces.bumpPeriodMs = static_cast<std::uint32_t>(std::clamp(std::lround(1000.0 / output.bumpHz), 5L, 1000L));
    }
    return forces;
}

void registerLinkSettings(Settings& settings)
{
    registerSettings(settings);
}

void registerLinkStatus(StatusBoard& status)
{
    registerStatus(status);
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
    if (options.versionText)
    {
        status.add("bridge.version", "Bridge", "Version");
        status.setText("bridge.version", options.versionText());
    }
    bool checkUpdates = settings.enabled(kUpdatesKey);
    bool prereleases  = settings.enabled(kPrereleasesKey);
    if (options.onUpdateSettings)
    {
        options.onUpdateSettings(checkUpdates, prereleases);
    }

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

    // From here until this function returns, the mod knows the bridge is driving the yoke.
    PresenceFile presence(options.telemetryPath);

    ForceModel    model(forceSettingsFrom(settings));
    std::uint64_t settingsRevision = settings.revision();

    FfbSession ffb(base, 1000, options.verbose);
    Effects    effects;
    if (!effects.registerAll(ffb))
    {
        return 4;
    }
    std::puts("Linked. Forces follow the game; with no game they stay at the idle settings.");
    const LinkPanel panel{&settings, &status, options.settingsPath};
    if (options.onPanel)
    {
        options.onPanel(&panel);
    }
    if (options.onStarted)
    {
        options.onStarted(ui.running() ? static_cast<int>(ui.port()) : 0);
    }
    std::string lastState;

    const auto modelInterval   = std::chrono::milliseconds(16);
    const auto sendInterval    = std::chrono::milliseconds(30);
    const auto refreshInterval = std::chrono::milliseconds(200);
    const auto rumbleInterval  = std::chrono::milliseconds(kRumbleDurationMs / 2);
    const auto statusInterval  = std::chrono::milliseconds(100);
    const auto printInterval   = std::chrono::seconds(1);

    // The motor torque of both axes is asked for four times a second. It is shown in the status, but
    // that is not why it is here: 0.1.3-pre3 stopped asking, and from then on no force reached the
    // yoke at all, although every command was still being sent. Something in the SDK or the base
    // needs these requests to keep coming for effects to play; what, exactly, is not known. They
    // are also half of the replies behind the SDK's lock-up (see the README), which is the price.
    const auto torqueInterval = std::chrono::milliseconds(250);
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
    PitchServo   servo;
    double       pitchPush   = 0.0;
    // Beside the settings file: what the handle's lights were before this program changed them.
    YokeLights   lights(options.settingsPath.empty() ? std::filesystem::path() : options.settingsPath.parent_path() / "yoke_lights_original.txt");
    auto         nextLightsAttach = start;
    int          sendErrors    = 0;
    bool         forcesWereOn  = status.forcesEnabled();
    TelemetryView view;

    const auto alive = [&] {
        if (options.onLoop)
        {
            options.onLoop();
        }
    };
    while (!stop)
    {
        alive();
        const auto now = Clock::now();
        if (options.seconds > 0 && std::chrono::duration<double>(now - start).count() >= options.seconds)
        {
            break;
        }

        presence.tick(now);

        if (now >= nextModel)
        {
            if (settings.revision() != settingsRevision)
            {
                settingsRevision = settings.revision();
                model.setSettings(forceSettingsFrom(settings));
            }
            const double dt = std::chrono::duration<double>(now - lastModel).count();
            view            = reader.poll(now);
            output          = model.update(view.fresh(), dt);
            lastModel       = now;
            nextModel       = now + modelInterval;

            // The sway needs to know where the yoke is; without that, or with it switched off, pitch
            // is left to its spring exactly as before.
            pitchPush = 0.0;
            bool steering = false;
            if (hid && status.forcesEnabled() && settings.get(kSwayKey) > 0.0)
            {
                auto [positionErr, state] = hid.getInputState();
                if (positionErr == MOZA_ERR_NO_ERROR)
                {
                    const double position = (state.axes[MOZA_HA_Y].normalizedValue - 0.5) * 2.0;
                    pitchPush = servo.update(pitchTargetFor(output, tuningFrom(settings, status, options.fullScale)), position, dt);
                    steering  = true;
                }
            }
            if (!steering)
            {
                servo.reset();
            }
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
            if (!send(ffb, effects, toDeviceForces(output, tuningFrom(settings, status, options.fullScale), pitchPush), sent, everything,
                      restartRumble, options.fullScale))
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

        // The lights are only touched once the user has asked for them to follow the engine.
        const LightTuning lightTuning = lightTuningFrom(settings);
        // ... or once an earlier run has left them changed, to put them back.
        if (!lights.attached() && now >= nextLightsAttach && (lightTuning.followEngine || lights.restorePending()))
        {
            lights.attach();
            nextLightsAttach = now + std::chrono::seconds(3);
        }
        const LightTarget lightTarget = lightTargetFor(view.fresh(), lightTuning);
        lights.show(lightTarget.level, lightTarget.heat, now);

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
            status.setText("game.surface", !live ? "-" : surfaceLabel(sample.surface));
            status.set("game.rough", live ? sample.rough : 0.0);
            status.set("game.slip", live ? sample.slip : 0.0);
            status.set("game.bump", live ? sample.bumpRight - sample.bumpLeft : 0.0);
            if (live && sample.startMs > 0.0)
            {
                status.set("game.start_time", sample.startMs / 1000.0);
            }
            else
            {
                status.setText("game.start_time", live ? "not given by the game" : "-");
            }
            status.set("game.rock", live ? sample.rollRate * 57.29577951308232 : 0.0);
            status.set("game.pitching", live ? sample.pitchRate * 57.29577951308232 : 0.0);
            status.set("game.heave", live ? sample.heave : 0.0);
            status.setText("game.implement", !live ? "-" : sample.working ? "working" : sample.lowered ? "lowered" : "raised or none");
            status.setText("game.engine", !live              ? "-"
                                          : sample.electric  ? (sample.rpm > 0.0 ? "electric, on" : "electric, off")
                                          : sample.starting  ? "starting"
                                          : sample.rpm > 0.0 ? "running"
                                                             : "off");

            if (hid)
            {
                auto [positionErr, state] = hid.getInputState();
                if (positionErr == MOZA_ERR_NO_ERROR)
                {
                    status.set("yoke.roll", (state.axes[MOZA_HA_X].normalizedValue - 0.5) * 2.0);
                    status.set("yoke.pitch", (state.axes[MOZA_HA_Y].normalizedValue - 0.5) * 2.0);
                }
            }

            const double scale = 100.0 / std::max(1, options.fullScale);
            status.set("force.centering", sent.rollSpring * scale);
            status.set("force.weight", sent.rollDamper * scale);
            status.set("force.pull", -sent.pull * scale * (settings.enabled(kFlipPullKey) ? -1.0 : 1.0));
            status.set("force.rumble", sent.rumble * scale);
            status.set("force.rumble_hz", 1000.0 / std::max<std::uint32_t>(1, sent.rumblePeriodMs));
            status.set("force.pitch_hold", sent.pitchSpring * scale);
            status.set("game.accel", live ? model.acceleration() : 0.0);
            status.set("force.sway", sent.pitchCenter / static_cast<double>(std::max(1, options.fullScale)));
            status.set("force.pitch_push", -sent.pitchPush * scale);
            status.set("yoke.roll_torque", cachedTorque(torqueX));
            status.set("yoke.pitch_torque", cachedTorque(torqueY));
            status.setText("yoke.lights", !lightTuning.followEngine ? "not following the engine"
                                          : !lights.attached()      ? "no yoke handle found"
                                                                    : "brightness " + std::to_string(lights.current()) + " of 15, colour " +
                                                                          std::to_string(lights.color().r) + "," +
                                                                          std::to_string(lights.color().g) + "," +
                                                                          std::to_string(lights.color().b));
            status.set("bridge.send_errors", sendErrors);
            if (options.versionText)
            {
                status.setText("bridge.version", options.versionText());
            }
            if (options.onUpdateSettings &&
                (settings.enabled(kUpdatesKey) != checkUpdates || settings.enabled(kPrereleasesKey) != prereleases))
            {
                checkUpdates = settings.enabled(kUpdatesKey);
                prereleases  = settings.enabled(kPrereleasesKey);
                options.onUpdateSettings(checkUpdates, prereleases);
            }
            nextStatus = now + statusInterval;
        }

        if (now >= nextPrint && options.onStatus)
        {
            const std::string state = std::string(telemetryText(view)) + (forcesOn ? "" : " (forces off)");
            if (state != lastState)
            {
                std::printf("Game: %s\n", state.c_str());
                lastState = state;
            }
            options.onStatus(state);
            nextPrint = now + printInterval;
        }
        else if (now >= nextPrint)
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

    if (options.onPanel)
    {
        options.onPanel(nullptr);
    }
    // Each of these talks to something that can fail to answer; the guard hears about every step.
    alive();
    ui.stop();
    presence.remove();
    alive();
    lights.restore();
    alive();
    ffb.stop();
    alive();
    std::printf("Forces released. Updates that failed to send: %d.\n", sendErrors);
    return 0;
}

} // namespace bridge
