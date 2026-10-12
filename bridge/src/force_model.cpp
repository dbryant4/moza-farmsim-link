#include "force_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
 * What one kind of ground feels like. Frequencies are positions in the user's rumble band (0 its
 * slow end, 1 its fast end), so the band setting still means what it says.
 */
struct SurfacePattern
{
    /** Roughness assumed when the game reports less; only paved ground uses it, to have something to scale. */
    double minRough;
    /** Where in the band the rumble sits at standstill, and at full speed. */
    double hzFrom;
    double hzTo;
    /** How much of the rumble comes and goes irregularly, 0..1, and the distance between its lumps, m. */
    double grain;
    double grainMetres;
    /** How much of it pulses regularly, 0..1, and the distance between pulses, m. */
    double pulse;
    double pulseMetres;
    /** Share of ForceSettings::fieldTug this ground gives. */
    double tug;
};

// In the order of Surface. Unknown is the plain rumble the bridge had before there were patterns.
constexpr SurfacePattern kPatterns[kSurfaceCount] = {
    /* Unknown */ {0.00, 0.00, 1.00, 0.00, 0.0, 0.0, 0.00, 0.0},
    /* Road    */ {0.15, 0.70, 1.00, 0.00, 0.0, 0.0, 0.00, 0.0},  // a fine, fast buzz, if any
    /* Gravel  */ {0.00, 0.55, 1.00, 0.35, 0.4, 0.0, 0.00, 0.0},  // fast and gritty
    /* Grass   */ {0.00, 0.10, 0.45, 0.25, 2.0, 0.0, 0.00, 0.0},  // low and soft, in long swells
    /* Snow    */ {0.00, 0.00, 0.25, 0.15, 3.0, 0.0, 0.00, 0.0},  // lower still, nearly even
    /* Heap    */ {0.00, 0.00, 0.30, 0.60, 1.0, 0.0, 0.00, 0.5},  // lurching
    /* Field   */ {0.00, 0.30, 0.70, 0.20, 1.5, 0.0, 0.00, 0.3},  // medium and steady
    /* Tilled  */ {0.00, 0.00, 0.35, 0.70, 1.2, 0.0, 0.00, 1.0},  // slow and heavy, clod by clod
    /* Ridge   */ {0.00, 0.10, 0.40, 0.20, 1.5, 0.8, 0.75, 0.6},  // a pulse per ridge
};

// Fastest the grain, the ridges and the tug may come and go, per second. The base is updated about
// thirty times a second, and beyond a few hertz a pattern is no longer felt as one anyway.
constexpr double kMaxGrainHz  = 6.0;
constexpr double kMaxPulseHz  = 6.0;
constexpr double kMaxTugHz    = 3.0;
constexpr double kTugMetres   = 2.5;
/** Speed by which the tug has reached full strength, m/s: field work is slow. */
constexpr double kTugFullSpeed = 3.0;
constexpr double kPi           = 3.14159265358979323846;

/** A fixed pseudo-random value in [0, 1) for each whole number. */
double lattice(std::int64_t index, std::uint64_t salt)
{
    std::uint64_t x = static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ull + salt;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
}

/** Smooth noise in [0, 1]: one random value per whole step of @p phase, eased between them. */
double smoothNoise(double phase, std::uint64_t salt)
{
    const double       whole = std::floor(phase);
    const double       part  = phase - whole;
    const double       ease  = part * part * (3.0 - 2.0 * part);
    const std::int64_t index = static_cast<std::int64_t>(whole);
    const double       from  = lattice(index, salt);
    return from + (lattice(index + 1, salt) - from) * ease;
}

double surfaceStrength(const ForceSettings& s, int surface)
{
    switch (static_cast<Surface>(surface))
    {
    case Surface::Road: return s.surfaceRoad;
    case Surface::Gravel: return s.surfaceGravel;
    case Surface::Grass: return s.surfaceGrass;
    case Surface::Snow: return s.surfaceSnow;
    case Surface::Heap: return s.surfaceHeap;
    case Surface::Field: return s.surfaceField;
    case Surface::Tilled: return s.surfaceTilled;
    case Surface::Ridge: return s.surfaceRidge;
    default: return 1.0;
    }
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

    trackEngineStart(telemetry, dt);
    trackAcceleration(telemetry, dt);
    trackSurface(telemetry, dt);

    const ForceOutput goal = target(telemetry);
    ForceOutput       next;
    next.rollSpring  = approach(m_output.rollSpring, goal.rollSpring, s.springSmoothing, dt);
    next.rollDamper  = approach(m_output.rollDamper, goal.rollDamper, s.damperSmoothing, dt);
    next.rollForce   = approach(m_output.rollForce, goal.rollForce, s.forceSmoothing, dt);
    next.pitchSpring = approach(m_output.pitchSpring, goal.pitchSpring, s.springSmoothing, dt);
    next.pitchDamper = approach(m_output.pitchDamper, goal.pitchDamper, s.damperSmoothing, dt);
    next.rumble      = approach(m_output.rumble, goal.rumble, s.rumbleSmoothing, dt);
    next.rumbleHz    = approach(m_output.rumbleHz, goal.rumbleHz, s.rumbleHzSmoothing, dt);
    // Not eased: the texture is smooth as generated, and easing it would iron it flat.
    next.rumbleTexture = goal.rumbleTexture;
    next.rollKick    = approach(m_output.rollKick, goal.rollKick, s.kickSmoothing, dt);
    next.pitchThump  = approach(m_output.pitchThump, goal.pitchThump, s.kickSmoothing, dt);
    next.rollWiggle  = approach(m_output.rollWiggle, goal.rollWiggle, s.kickSmoothing, dt);
    next.bumpHz      = goal.bumpHz;
    next.pitchBounce = approach(m_output.pitchBounce, goal.pitchBounce, s.kickSmoothing, dt);
    next.pitchTarget = approach(m_output.pitchTarget, goal.pitchTarget, s.swaySmoothing, dt);

    m_output = clamped(next);
    return m_output;
}

void ForceModel::trackEngineStart(const Telemetry* telemetry, double dt)
{
    const ForceSettings& s      = m_settings;
    const bool           starts = telemetry != nullptr && telemetry->active && !telemetry->electric && telemetry->starting;
    // How long the shake is to last: a share of this machine's own start where the game gives it.
    const double startSeconds = telemetry != nullptr ? sanitize(telemetry->startMs, 0.0, 60000.0) / 1000.0 : 0.0;
    const double length       = startSeconds > 0.0 ? unit(s.engineStartShare) * startSeconds : std::max(0.0, s.engineStartSeconds);
    const bool   unlimited    = !(startSeconds > 0.0) && !(s.engineStartSeconds > 0.0);
    const double fadeSeconds  = unlimited ? std::max(0.0, s.engineStartFadeSeconds) : std::min(std::max(0.0, s.engineStartFadeSeconds), length / 2.0);
    const auto   fade         = [&] { m_startShake = fadeSeconds > 0.0 ? std::max(0.0, m_startShake - dt / fadeSeconds) : 0.0; };
    if (telemetry == nullptr || !telemetry->active || telemetry->electric)
    {
        m_startShake = 0.0;
    }
    else if (starts)
    {
        m_startElapsed = m_starting ? m_startElapsed + dt : 0.0;
        // Full strength until it is time to start dying away, so as to be done within its allotted length.
        const double hold = std::max(0.0, length - fadeSeconds);
        if (unlimited || m_startElapsed <= hold)
        {
            m_startShake = 1.0;
        }
        else
        {
            fade();
        }
    }
    else
    {
        // The engine has caught (or the start was abandoned): let the shake die away, not cut off.
        fade();
    }
    m_starting = starts;
}

void ForceModel::trackAcceleration(const Telemetry* telemetry, double dt)
{
    // Limits on what counts as one vehicle carrying on, rather than a pause, a reload or another vehicle.
    constexpr double kLongestGap  = 0.5;  // s between samples
    constexpr double kMassChange  = 0.02; // fraction; hitching an implement changes more and costs one sample
    constexpr double kMaxAccel    = 12.0; // m/s^2, beyond any vehicle in the game

    if (telemetry == nullptr || !telemetry->active)
    {
        m_haveSpeed    = false;
        m_acceleration = 0.0;
        return;
    }
    m_sinceSpeed += dt;
    if (m_haveSpeed && telemetry->seq == m_speedSeq)
    {
        return; // the same sample again: updates run faster than the game writes
    }

    const double speed = sanitize(telemetry->speed, -kMaxSpeed, kMaxSpeed);
    const double mass  = sanitize(telemetry->mass, 0.0, kMaxMass);
    if (m_haveSpeed)
    {
        // The game's own clock, so that a pause (the clock stands still, then carries on) or a long
        // hitch is seen for what it is. Only a mod that sends no clock falls back to time passed here.
        double span = static_cast<double>(telemetry->timeMs - m_speedTimeMs) / 1000.0;
        if (telemetry->timeMs == 0 && m_speedTimeMs == 0)
        {
            span = m_sinceSpeed;
        }
        const bool sameVehicle = std::abs(mass - m_speedMass) <= kMassChange * std::max(mass, m_speedMass);
        if (span > 0.001 && span < kLongestGap && sameVehicle)
        {
            const double raw   = std::clamp((speed - m_speed) / span, -kMaxAccel, kMaxAccel);
            const double tau   = m_settings.accelFilterSeconds;
            const double blend = tau > 0.0 ? 1.0 - std::exp(-span / tau) : 1.0;
            m_acceleration += blend * (raw - m_acceleration);
        }
        else
        {
            m_acceleration = 0.0;
        }
    }
    m_haveSpeed   = true;
    m_speedSeq    = telemetry->seq;
    m_speedTimeMs = telemetry->timeMs;
    m_speed       = speed;
    m_speedMass   = mass;
    m_sinceSpeed  = 0.0;
}

void ForceModel::trackSurface(const Telemetry* telemetry, double dt)
{
    // Paused or on foot, the ground stays where it was: the rumble is fading out anyway.
    if (telemetry == nullptr || !telemetry->active)
    {
        return;
    }
    const ForceSettings& s       = m_settings;
    const int            index   = static_cast<int>(telemetry->surface);
    const std::size_t    current = s.surfacePatterns && index > 0 && index < kSurfaceCount ? static_cast<std::size_t>(index) : 0;
    const double         step    = s.surfaceBlendSeconds > 0.0 ? 1.0 - std::exp(-dt / s.surfaceBlendSeconds) : 1.0;
    // Each share's average over this step, so that the ground passed comes out the same however
    // the time is cut up: the share closes on its goal exponentially, and this is that curve's mean.
    const double meanStep = s.surfaceBlendSeconds > 0.0 ? 1.0 - s.surfaceBlendSeconds * step / dt : 1.0;

    // The texture is laid out along the ground, so it passes at the speed of the wheels.
    const double speed     = sanitize(std::abs(telemetry->speed), 0.0, kMaxSpeed);
    double       grainRate = 0.0;
    double       pulseRate = 0.0;
    double       total     = 0.0;
    for (std::size_t i = 0; i < m_surfaceMix.size(); ++i)
    {
        const double goal = i == current ? 1.0 : 0.0;
        const double mean = m_surfaceMix[i] + (goal - m_surfaceMix[i]) * meanStep;
        const SurfacePattern& pattern = kPatterns[i];
        grainRate += pattern.grainMetres > 0.0 ? mean * speed / pattern.grainMetres : 0.0;
        pulseRate += pattern.pulseMetres > 0.0 ? mean * speed / pattern.pulseMetres : 0.0;

        m_surfaceMix[i] += (goal - m_surfaceMix[i]) * step;
        total += m_surfaceMix[i];
    }
    // Rounding must not let the shares drift from a whole over hours of driving.
    for (double& share : m_surfaceMix)
    {
        share = total > 0.0 ? share / total : 0.0;
    }
    if (!(total > 0.0))
    {
        m_surfaceMix[current] = 1.0;
    }

    // Kept small so that hours of driving cost the phases no precision.
    m_grainPhase = std::fmod(m_grainPhase + std::min(grainRate, kMaxGrainHz) * dt, 65536.0);
    m_pulsePhase = std::fmod(m_pulsePhase + std::min(pulseRate, kMaxPulseHz) * dt, 1.0);
    m_tugPhase   = std::fmod(m_tugPhase + std::min(speed / kTugMetres, kMaxTugHz) * dt, 65536.0);
}

Surface ForceModel::surface() const
{
    const auto strongest = std::max_element(m_surfaceMix.begin(), m_surfaceMix.end());
    return static_cast<Surface>(strongest - m_surfaceMix.begin());
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

    // Toward the side that was hit: right is positive. Equal hits on both sides cancel, and the
    // heavier the rig, the less a hit moves its steering.
    const double lightness = s.bumpKickMassHalf > 0.0 ? s.bumpKickMassHalf / (s.bumpKickMassHalf + mass) : 1.0;
    const double bumpLeft  = unit(telemetry->bumpLeft);
    const double bumpRight = unit(telemetry->bumpRight);
    out.rollKick           = unit(s.bumpKick) * lightness * (bumpRight - bumpLeft);

    // What both sides share has no direction, so it shakes the wheel rather than yanking it.
    const double shared = std::min(bumpLeft, bumpRight);
    out.pitchThump      = unit(s.bumpThump) * shared;
    out.rollWiggle      = unit(s.bumpWiggle) * lightness * shared;

    // The body's own movement. Leaning over to the right is positive and so is a push of the wheel
    // to the right, hence the minus: the wheel goes with the side that is rising, as it does when
    // that side's wheel climbs something and the kick above fires.
    const double rockShare   = s.bodyRockFullRate > 0.0 ? sanitize(telemetry->rollRate / s.bodyRockFullRate, -1.0, 1.0) : 0.0;
    out.rollKick            -= unit(s.bodyRock) * rockShare;
    const double pitchShare  = s.bodyPitchFullRate > 0.0 ? sanitize(telemetry->pitchRate / s.bodyPitchFullRate, -1.0, 1.0) : 0.0;
    const double heaveShare  = s.bodyHeaveFull > 0.0 ? sanitize(telemetry->heave / s.bodyHeaveFull, -1.0, 1.0) : 0.0;
    out.pitchBounce          = unit(s.bodyBounce) * sanitize(0.5 * pitchShare + 0.5 * heaveShare, -1.0, 1.0);

    const double bandLow  = std::min(s.rumbleMinHz, s.rumbleMaxHz);
    const double bandHigh = std::max(s.rumbleMinHz, s.rumbleMaxHz);

    // The ground's own pattern: the mix of surfaces the wheels are on, or just came off.
    SurfacePattern ground{};
    double         groundStrength = 0.0;
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        const double share = m_surfaceMix[static_cast<std::size_t>(i)];
        ground.minRough += share * kPatterns[i].minRough;
        ground.hzFrom += share * kPatterns[i].hzFrom;
        ground.hzTo += share * kPatterns[i].hzTo;
        ground.grain += share * kPatterns[i].grain;
        ground.pulse += share * kPatterns[i].pulse;
        ground.tug += share * kPatterns[i].tug;
        groundStrength += share * sanitize(surfaceStrength(s, i), 0.0, 4.0);
    }

    const double workingBoost = telemetry->working ? 1.0 + s.rumbleWorkingBoost : 1.0;
    const double surface      = s.rumbleSurface * std::max(rough, ground.minRough) * groundStrength * easeToFull(speed, s.rumbleFullSpeed) * workingBoost;
    const double hzShare      = s.rumbleHzFullSpeed > 0.0 ? std::min(speed / s.rumbleHzFullSpeed, 1.0) : 1.0;
    const double surfaceHz    = s.rumbleMinHz + (s.rumbleMaxHz - s.rumbleMinHz) * (ground.hzFrom + (ground.hzTo - ground.hzFrom) * hzShare);
    // Where the engine is between this machine's idle and flat out, and how fast the machine is going.
    const double idleRpm      = telemetry->rpmMin > 0.0 ? sanitize(telemetry->rpmMin, 1.0, kMaxRpm) : s.engineIdleRpm;
    const double reportedFull = telemetry->rpmMax > 0.0 ? sanitize(telemetry->rpmMax, 1.0, kMaxRpm) : s.engineFullRpm;
    const double fullRpm      = reportedFull > idleRpm ? reportedFull : idleRpm + 1.0;
    const double revs         = unit((rpm - idleRpm) / (fullRpm - idleRpm));
    const double pace         = easeToFull(speed, s.rumbleFullSpeed);
    const double effort       = unit(s.engineRumbleRevShare * revs + s.engineRumbleLoadShare * load + s.engineRumbleSpeedShare * pace);
    const double engineShare  = unit(s.engineRumbleIdleShare) + (1.0 - unit(s.engineRumbleIdleShare)) * effort;
    // An electric motor has no firing strokes to feel; what is left for it is the ground.
    const bool   combustion   = !telemetry->electric;
    const double engine       = combustion && rpm > 0.0 ? s.engineRumble * engineShare : 0.0;
    const double hzSpeedShare = unit(s.engineHzSpeedShare);
    const double idleHz       = sanitize(idleRpm / 60.0, bandLow, bandHigh);
    const double engineHz     = idleHz + (bandHigh - idleHz) * unit((1.0 - hzSpeedShare) * revs + hzSpeedShare * pace);
    const double starting     = combustion ? unit(s.engineStartShake) * m_startShake : 0.0;
    const double startingHz   = sanitize(s.engineStartHz, bandLow, bandHigh);

    // One vibration has to carry every source, so its frequency is their mean weighted by
    // strength: whichever is strongest sets the pitch. The start-up shake is added after the cap.
    const double running = std::min(surface + engine, unit(s.rumbleMax));
    const double total   = surface + engine + starting;
    out.rumble           = running + starting;
    out.rumbleHz         = total > 0.0 ? (surface * surfaceHz + engine * engineHz + starting * startingHz) / total : surfaceHz;

    // The grain and the ridges come and go under the wheels; the engine's share of the rumble does not.
    const double lumps       = 1.0 - unit(ground.grain) * (1.0 - smoothNoise(m_grainPhase, 0x67));
    const double ridges      = 1.0 - unit(ground.pulse) * (0.5 + 0.5 * std::cos(2.0 * kPi * m_pulsePhase));
    const double surfacePart = surface + engine > 0.0 ? surface * running / (surface + engine) : 0.0;
    out.rumbleTexture        = out.rumble > 0.0 ? 1.0 - (surfacePart / out.rumble) * (1.0 - lumps * ridges) : 1.0;

    // Broken ground also pulls at the wheel, now one way, now the other.
    const double tug = unit(s.fieldTug) * ground.tug * lightness * easeToFull(speed, kTugFullSpeed);
    out.rollKick += tug * (2.0 * smoothNoise(m_tugPhase, 0x7A) - 1.0);

    out.pitchSpring = s.pitchSpring;
    out.pitchDamper = s.pitchDamper;

    const double accelShare = s.accelSwayFullAccel > 0.0 ? std::clamp(m_acceleration / s.accelSwayFullAccel, -1.0, 1.0) : 0.0;
    out.pitchTarget         = unit(s.accelSway) * accelShare;
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
    output.rumbleTexture = std::isnan(output.rumbleTexture) ? 1.0 : std::clamp(output.rumbleTexture, 0.0, 1.0);
    output.rollKick    = sanitize(output.rollKick, -1.0, 1.0);
    output.pitchThump  = unit(output.pitchThump);
    output.rollWiggle  = unit(output.rollWiggle);
    output.bumpHz      = sanitize(m_settings.bumpShakeHz, 2.0, 40.0);
    output.pitchBounce = sanitize(output.pitchBounce, -1.0, 1.0);
    output.pitchTarget = sanitize(output.pitchTarget, -1.0, 1.0);
    output.rumbleHz    = sanitize(output.rumbleHz, std::min(m_settings.rumbleMinHz, m_settings.rumbleMaxHz),
                                  std::max(m_settings.rumbleMinHz, m_settings.rumbleMaxHz));
    return output;
}

} // namespace bridge
