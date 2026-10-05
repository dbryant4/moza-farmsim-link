#include "force_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bridge {

namespace {

// Bounds on what is accepted from telemetry, far beyond anything the game produces. They exist so
// that a corrupt value cannot carry an infinity into the arithmetic below.
constexpr double kMaxSpeed = 1000.0; // m/s
constexpr double kMaxMass  = 1.0e7;  // kg
constexpr double kMaxRpm   = 1.0e5;  // 1/min

/** Clamps into [low, high]; NaN, which std::clamp would pass through, becomes the bound nearest zero. */
double sanitize(double value, double low, double high)
{
    if (std::isnan(value))
    {
        value = 0.0;
    }
    return std::clamp(value, low, high);
}

double unit(double value)
{
    return sanitize(value, 0.0, 1.0);
}

/**
 * 0 at standstill rising to 1 at @p fullSpeed: steepest at the start, so there is already something
 * to feel at field speeds, and flat on arrival, so reaching the maximum is not a kink.
 */
double easeToFull(double speed, double fullSpeed)
{
    if (!(fullSpeed > 0.0))
    {
        return speed > 0.0 ? 1.0 : 0.0;
    }
    const double x = std::clamp(speed / fullSpeed, 0.0, 1.0);
    return x * (2.0 - x);
}

/** 1 up to @p start falling linearly to 0 at @p end. */
double fadeOut(double value, double start, double end)
{
    if (!(end > start))
    {
        return value < start ? 1.0 : 0.0;
    }
    return 1.0 - std::clamp((value - start) / (end - start), 0.0, 1.0);
}

/**
 * Moves @p current toward @p target for @p dt seconds, as a low-pass whose rate of change is capped
 * at the slew limit. This is the exact solution for a target held over the step (a straight line
 * at the limit until the low-pass alone would be slower, then an exponential), not an Euler step,
 * which is what makes the result the same whether the time arrives as one 50 ms update or five of
 * 10 ms. It never overshoots and never moves further than perSecond * dt.
 */
double approach(double current, double target, const Smoothing& smoothing, double dt)
{
    if (!std::isfinite(target))
    {
        return current;
    }
    const double gap      = target - current;
    double       distance = std::abs(gap);
    const bool   filtered = smoothing.seconds > 0.0;
    const bool   limited  = smoothing.perSecond > 0.0;
    if (!filtered && !limited)
    {
        return target;
    }

    // Closer to the target than this, the low-pass is the slower of the two and takes over.
    const double knee = !filtered ? 0.0 : limited ? smoothing.perSecond * smoothing.seconds : std::numeric_limits<double>::infinity();
    if (distance > knee)
    {
        const double timeToKnee = (distance - knee) / smoothing.perSecond;
        if (dt <= timeToKnee)
        {
            return target - std::copysign(distance - smoothing.perSecond * dt, gap);
        }
        distance = knee;
        dt -= timeToKnee;
    }
    if (filtered)
    {
        distance *= std::exp(-dt / smoothing.seconds);
    }
    return target - std::copysign(distance, gap);
}

} // namespace

ForceModel::ForceModel(const ForceSettings& settings) : m_settings(settings)
{
    // Zero is outside the band; start the frequency where a rumble fading in from standstill begins.
    m_output = clamped(m_output);
}

ForceOutput ForceModel::update(const Telemetry* telemetry, double dtSeconds)
{
    // Written so that NaN fails it too.
    if (!(dtSeconds > 0.0))
    {
        return m_output;
    }
    const ForceSettings& s  = m_settings;
    const double         dt = s.maxStepSeconds > 0.0 ? std::min(dtSeconds, s.maxStepSeconds) : dtSeconds;

    const ForceOutput goal = target(telemetry);
    ForceOutput       next;
    next.rollSpring  = approach(m_output.rollSpring, goal.rollSpring, s.springSmoothing, dt);
    next.rollDamper  = approach(m_output.rollDamper, goal.rollDamper, s.damperSmoothing, dt);
    next.rollForce   = approach(m_output.rollForce, goal.rollForce, s.forceSmoothing, dt);
    next.pitchSpring = approach(m_output.pitchSpring, goal.pitchSpring, s.springSmoothing, dt);
    next.pitchDamper = approach(m_output.pitchDamper, goal.pitchDamper, s.damperSmoothing, dt);
    next.rumble      = approach(m_output.rumble, goal.rumble, s.rumbleSmoothing, dt);
    next.rumbleHz    = approach(m_output.rumbleHz, goal.rumbleHz, s.rumbleHzSmoothing, dt);

    m_output = clamped(next);
    return m_output;
}

ForceOutput ForceModel::target(const Telemetry* telemetry) const
{
    const ForceSettings& s = m_settings;
    ForceOutput          out;

    if (telemetry == nullptr || !telemetry->active)
    {
        out.rollSpring  = s.idleSpring;
        out.rollDamper  = s.idleDamper;
        out.pitchSpring = s.idleSpring;
        out.pitchDamper = s.idleDamper;
        out.rumbleHz    = m_output.rumbleHz;
        return clamped(out);
    }

    // Reversing counts like driving forward: the tyres load the steering the same either way.
    const double speed = sanitize(std::abs(telemetry->speed), 0.0, kMaxSpeed);
    const double steer = sanitize(telemetry->steer, -1.0, 1.0);
    const double mass  = sanitize(telemetry->mass, 0.0, kMaxMass);
    const double rpm   = sanitize(telemetry->rpm, 0.0, kMaxRpm);
    const double load  = unit(telemetry->load);
    const double slip  = unit(telemetry->slip);
    const double rough = unit(telemetry->rough);

    out.rollSpring = s.rollSpringStopped + (s.rollSpringMax - s.rollSpringStopped) * easeToFull(speed, s.rollSpringFullSpeed);

    const double massShare = s.rollDamperMassHalf > 0.0 ? mass / (mass + s.rollDamperMassHalf) : (mass > 0.0 ? 1.0 : 0.0);
    out.rollDamper = s.rollDamperBase + s.rollDamperMass * massShare +
                     s.rollDamperStandstill * fadeOut(speed, 0.0, s.rollDamperStandstillSpeed);

    // Slipping tyres generate no aligning torque, so the wheel goes light. The implement keeps
    // dragging the rig straight whether or not the tyres grip, so its share stays.
    const double grip      = fadeOut(slip, s.selfAlignSlipStart, s.selfAlignSlipEnd);
    const double selfAlign = s.selfAlign * easeToFull(speed, s.selfAlignFullSpeed) * grip;
    const bool   dragging  = telemetry->lowered && telemetry->working;
    const double implement = dragging ? s.implementDrag * easeToFull(speed, s.implementDragFullSpeed) : 0.0;
    // Steering right is positive and so is a push to the right, hence the minus: toward center.
    out.rollForce = -steer * (selfAlign + implement);

    const double bandLow  = std::min(s.rumbleMinHz, s.rumbleMaxHz);
    const double bandHigh = std::max(s.rumbleMinHz, s.rumbleMaxHz);

    const double workingBoost = telemetry->working ? 1.0 + s.rumbleWorkingBoost : 1.0;
    const double surface      = s.rumbleSurface * rough * easeToFull(speed, s.rumbleFullSpeed) * workingBoost;
    const double hzShare      = s.rumbleHzFullSpeed > 0.0 ? std::min(speed / s.rumbleHzFullSpeed, 1.0) : 1.0;
    const double surfaceHz    = s.rumbleMinHz + (s.rumbleMaxHz - s.rumbleMinHz) * hzShare;
    const double engineShare  = s.engineRumbleIdleShare + (1.0 - s.engineRumbleIdleShare) * load;
    const double engine       = rpm > 0.0 ? s.engineRumble * engineShare : 0.0;
    const double engineHz     = sanitize(rpm / 60.0, bandLow, bandHigh);

    // One vibration has to carry both sources, so its frequency is their mean weighted by strength:
    // whichever is stronger sets the pitch.
    const double total = surface + engine;
    out.rumble         = std::min(total, s.rumbleMax);
    out.rumbleHz       = total > 0.0 ? (surface * surfaceHz + engine * engineHz) / total : surfaceHz;

    out.pitchSpring = s.pitchSpring;
    out.pitchDamper = s.pitchDamper;
    return clamped(out);
}

ForceOutput ForceModel::clamped(ForceOutput output) const
{
    const double forceLimit = unit(m_settings.rollForceLimit);

    output.rollSpring  = unit(output.rollSpring);
    output.rollDamper  = unit(output.rollDamper);
    output.rollForce   = sanitize(output.rollForce, -forceLimit, forceLimit);
    output.pitchSpring = unit(output.pitchSpring);
    output.pitchDamper = unit(output.pitchDamper);
    output.rumble      = unit(output.rumble);
    output.rumbleHz    = sanitize(output.rumbleHz, std::min(m_settings.rumbleMinHz, m_settings.rumbleMaxHz),
                                  std::max(m_settings.rumbleMinHz, m_settings.rumbleMaxHz));
    return output;
}

} // namespace bridge
