#pragma once

#include "telemetry.hpp"

#include <array>

namespace bridge {

/** Normalized force settings for the device layer; 1.0 is 100% of the matching effect. */
struct ForceOutput
{
    double rollSpring  = 0.0; // 0..1 centering stiffness on roll (the steering axis)
    double rollDamper  = 0.0; // 0..1 damping on roll
    double rollForce   = 0.0; // -1..1 constant force on roll; positive pushes the wheel toward steering right
    double pitchSpring = 0.0; // 0..1 centering stiffness on pitch (held centered; not used for steering)
    double pitchDamper = 0.0; // 0..1
    double rumble      = 0.0; // 0..1 vibration strength
    double rumbleHz    = 0.0; // vibration frequency
    double rumbleTexture = 1.0; // 0..1 what the rumble is multiplied by right now: the surface's grain passing under the wheels
    double rollKick    = 0.0; // -1..1 brief extra force on roll, added to rollForce; same sign convention
    double pitchThump  = 0.0; // 0..1 brief in-out shake on pitch
    double rollWiggle  = 0.0; // 0..1 brief left-right shake on roll
    double bumpHz      = 0.0; // frequency of the thump and the wiggle
    double pitchBounce = 0.0; // -1..1 force on pitch from the machine's own pitching and bouncing; positive pushes the same way as a positive pitchTarget
    double pitchTarget = 0.0; // -1..1 where pitch should sit, in half-travels from center; positive while gaining speed
};

/** How one output is eased toward its target. */
struct Smoothing
{
    /** Low-pass time constant, seconds. 0 leaves only the slew limit. */
    double seconds = 0.0;
    /** Largest rate of change, output units per second. 0 leaves only the low-pass. */
    double perSecond = 0.0;
};

/**
 * Tuning of the force model. Strengths are fractions of the device's full effect. For scale, on
 * the AY210 a 100% spring measured about 0.30 N*m per half-travel and 100% constant force about
 * 0.41 N*m, while roll starts to move at 0.05 N*m and pitch at 0.15 N*m (see README.md).
 */
struct ForceSettings
{
    // Roll centering spring.

    /** Stiffness at standstill, 0..1. Zero: a parked tractor's wheel stays where it is left. */
    double rollSpringStopped = 0.0;
    /** Stiffness at and above rollSpringFullSpeed, 0..1. */
    double rollSpringMax = 0.60;
    /** Speed at which the spring reaches its maximum, m/s. 12 m/s is 43 km/h, a tractor's road speed. */
    double rollSpringFullSpeed = 12.0;

    // Roll damper: the weight of the steering.

    /** Damping that is always there while driving, 0..1. */
    double rollDamperBase = 0.15;
    /** Damping an infinitely heavy rig would add, 0..1. */
    double rollDamperMass = 0.25;
    /** Total mass that adds half of rollDamperMass, kg. */
    double rollDamperMassHalf = 10000.0;
    /** Extra damping at standstill, where the tyres scrub instead of rolling, 0..1. */
    double rollDamperStandstill = 0.20;
    /** Speed by which the standstill damping has faded out, m/s. */
    double rollDamperStandstillSpeed = 1.5;

    // Constant force on roll. Both terms pull toward center in proportion to steering deflection.

    /** Self-aligning pull at full lock, at and above selfAlignFullSpeed with the wheels gripping, 0..1. */
    double selfAlign = 0.15;
    /** Speed at which the self-aligning pull reaches its maximum, m/s. */
    double selfAlignFullSpeed = 10.0;
    /** Driven-wheel slip (0..1) up to which the self-aligning pull is intact. */
    double selfAlignSlipStart = 0.15;
    /** Driven-wheel slip (0..1) at which the self-aligning pull is gone. */
    double selfAlignSlipEnd = 0.60;
    /** Extra pull at full lock while a lowered implement works the ground, 0..1. Not reduced by slip. */
    double implementDrag = 0.10;
    /** Speed at which the implement pull reaches its maximum, m/s. Field work is slow, so this is low. */
    double implementDragFullSpeed = 3.0;
    /** Cap on the constant force as a whole, 0..1, whatever the terms above add up to. */
    double rollForceLimit = 0.30;

    // Kick: a front wheel hitting a log or a kerb is held back for an instant, which yanks the
    // steering toward the side that hit. Not subject to rollForceLimit; it is meant to be a jolt.

    /** Kick when one steered wheel of a very light machine takes a full hit (bump = 1) and the other none, 0..1. */
    double bumpKick = 0.5;
    /**
     * Total mass at which the kick is down to half, kg. A log that throws a skid steer's wheel
     * sideways barely moves a 20 t tractor's, so the kick shrinks as the rig gets heavier. 0 turns
     * the scaling off.
     */
    double bumpKickMassHalf = 6000.0;

    // Both steered wheels jolted together, as when crossing rail tracks or meeting a kerb head on.
    // There is no side to kick toward, so the wheel shakes instead: in and out, and a little left
    // and right. Each lasts as long as the bump does.

    /** In-out shake on pitch at a full hit on both wheels, 0..1. Pitch needs a lot before it moves. */
    double bumpThump = 0.7;
    /** Left-right shake on roll for the same hit on a very light machine, 0..1. Shrinks with mass like the kick. */
    double bumpWiggle = 0.25;
    /** Frequency of both shakes, Hz. Low: these are a couple of lurches, not a buzz. */
    double bumpShakeHz = 7.0;

    // The machine's own movement, as seen from the seat when the cab sways against the horizon
    // off road: the wheel is pushed as the body rocks, the yoke as it pitches and bounces. These
    // follow what the body does, so a heavy machine that barely moves barely shows them.

    /** Force on roll with the body rocking sideways at bodyRockFullRate, 0..1. */
    double bodyRock = 0.30;
    /** Rocking rate that gives all of bodyRock, rad/s. A tractor on a rough field rocks at 0.2 to 0.3. */
    double bodyRockFullRate = 0.5;
    /** Force on pitch with the body pitching and bouncing at the two rates below, 0..1. Pitch needs a lot before it moves. */
    double bodyBounce = 0.60;
    /** Pitching rate, rad/s, and vertical acceleration, m/s^2, that each give half of bodyBounce. */
    double bodyPitchFullRate = 0.4;
    double bodyHeaveFull     = 5.0;

    // Rumble.

    /** Strength on the roughest surface (rough = 1) at and above rumbleFullSpeed, 0..1. */
    double rumbleSurface = 0.20;
    /** Speed at which surface rumble reaches full strength, m/s. */
    double rumbleFullSpeed = 8.0;
    /** Fractional increase in surface rumble while an implement works the ground; 0.5 is +50%. */
    double rumbleWorkingBoost = 0.5;
    /** Cap on rumble strength as a whole, 0..1. */
    double rumbleMax = 0.30;
    /** Rumble frequency at standstill, Hz. */
    double rumbleMinHz = 8.0;
    /** Rumble frequency at and above rumbleHzFullSpeed, Hz. */
    double rumbleMaxHz = 25.0;
    /** Speed at which the rumble frequency reaches rumbleMaxHz, m/s. */
    double rumbleHzFullSpeed = 12.0;
    /**
     * Engine vibration with the engine flat out, under full load, at speed, 0..1. Off by default:
     * it is the only rumble present at standstill. How much of it there is at any moment, and how
     * fast it shakes, follow the engine's speed above all, then its load and the machine's pace.
     */
    double engineRumble = 0.0;
    /** Fraction of engineRumble left with the engine idling, unloaded, at a standstill, 0..1. */
    double engineRumbleIdleShare = 0.5;
    /**
     * What the rest is made of. Each is a share of the way from idle to full: engine speed between
     * this machine's idle and maximum, engine load, and road speed up to rumbleFullSpeed.
     */
    double engineRumbleRevShare   = 0.50;
    double engineRumbleLoadShare  = 0.25;
    double engineRumbleSpeedShare = 0.25;
    /**
     * The frequency runs from the slow end of the rumble band at idle to the fast end flat out;
     * this much of that climb comes from road speed rather than engine speed, 0..1.
     */
    double engineHzSpeedShare = 0.20;
    /** Idle and maximum engine speed assumed for a machine that does not report its own, 1/min. */
    double engineIdleRpm = 850.0;
    double engineFullRpm = 2200.0;
    /**
     * Shake while a combustion engine is being started, 0..1: the starter cranking and the engine
     * catching. It comes on top of rumbleMax rather than under it, because the point of it is to
     * stand out from the running rumble. Electric vehicles get none.
     */
    double engineStartShake = 0.6;
    /** Frequency of the start-up shake, Hz, held inside the rumble band. Cranking is slow and lumpy. */
    double engineStartHz = 9.0;
    /**
     * How much of the machine's own start the shake lasts for, 0..1, from the start being triggered
     * to the end of its dying away. Each machine takes its own time to start (the game goes by the
     * length of its start sound) and says so; the shake makes its point in part of that. 1 lasts
     * the whole start.
     */
    double engineStartShare = 0.5;
    /**
     * Longest the shake lasts, seconds, on a machine whose start time the game does not give.
     * 0 lets it last as long as the game keeps the engine starting.
     */
    double engineStartSeconds = 1.5;
    /** How long the start-up shake takes to die away, seconds; never more than half of its length. */
    double engineStartFadeSeconds = 0.4;

    // Surfaces. Each kind of ground has its own pattern of rumble (see kPatterns in
    // force_model.cpp): where in the frequency band it sits, how lumpy it is, whether it pulses
    // as ridges pass. These set how much of each there is; the pattern itself is fixed.

    /** Off gives every surface the same plain rumble, scaled by its roughness only. */
    bool surfacePatterns = true;
    /** Strength per surface, as a multiple of what its roughness alone would give. */
    double surfaceRoad   = 0.0; // paved ground is smooth; raising this gives it a faint, fast buzz
    double surfaceGravel = 1.0;
    double surfaceGrass  = 0.7;
    double surfaceSnow   = 0.6;
    double surfaceHeap   = 1.0;
    double surfaceField  = 1.0;
    double surfaceTilled = 1.0;
    double surfaceRidge  = 1.0;
    /**
     * Irregular pull on the steering from broken ground, 0..1 of full force for a very light
     * machine on a ploughed field at speed; other surfaces get a share of it or none. Shrinks with
     * mass like the kick.
     */
    double fieldTug = 0.12;
    /** How long the feel takes to change when the ground under the wheels does, seconds. */
    double surfaceBlendSeconds = 0.25;

    // Pitch is not a control in the game; it is only held centered so it does not flop in and out.

    /** Pitch centering stiffness while driving, 0..1. */
    double pitchSpring = 0.80;
    /** Pitch damping while driving, 0..1. */
    double pitchDamper = 0.30;

    // Sway: the yoke leans a little in and out with acceleration and braking.

    /** How far pitch moves from center at accelSwayFullAccel, in half-travels, 0..1. 0 turns it off. */
    double accelSway = 0.12;
    /** Acceleration that gives the full sway, m/s^2. A tractor pulling away manages 1 to 2; hard braking more. */
    double accelSwayFullAccel = 2.5;
    /** Time constant of the acceleration estimate, seconds. Speed arrives in 0.01 m/s steps and needs it. */
    double accelFilterSeconds = 0.20;

    // Idle: no fresh telemetry, or the player is not in a vehicle. Applies to both axes.

    /** Centering stiffness, 0..1. */
    double idleSpring = 0.20;
    /** Damping, 0..1. */
    double idleDamper = 0.10;

    // Smoothing. With both parts set, no output moves faster than perSecond, and a step in
    // telemetry reaches the device as a ramp that eases into its new value.

    Smoothing springSmoothing{0.30, 2.0};
    Smoothing damperSmoothing{0.30, 2.0};
    /** Shorter than the rest: this force follows the steering and feels detached if it lags. */
    Smoothing forceSmoothing{0.12, 2.0};
    Smoothing rumbleSmoothing{0.15, 2.0};
    /** perSecond is in Hz per second here. */
    Smoothing rumbleHzSmoothing{0.25, 40.0};
    /** Barely smoothed: the mod already shapes each bump, and a softened jolt is not a jolt. */
    Smoothing kickSmoothing{0.015, 60.0};
    /** perSecond is in half-travels per second here. Slow: a lean, not a shove. */
    Smoothing swaySmoothing{0.20, 0.8};
    /**
     * Longest time one update advances the smoothing by, seconds. After a stall the model resumes
     * from where it was at this pace, instead of covering the whole gap in one jump. 0 removes the cap.
     */
    double maxStepSeconds = 0.10;
};

/**
 * Turns telemetry into force settings: what a tractor's steering would feel like, as far as a
 * spring, a damper, a constant force and a vibration can say it. Pure arithmetic with no device
 * access. Every output moves continuously, including from construction, when all are zero.
 * Besides its outputs it remembers only an engine start in progress, the last speed sample, from
 * which it estimates acceleration, and how far along the ground's texture the wheels are.
 */
class ForceModel
{
public:
    explicit ForceModel(const ForceSettings& settings = {});

    /**
     * Advances the smoothing by @p dtSeconds toward the forces for @p telemetry and returns them.
     * Pass null when there is no fresh sample; that and `active=0` both fade to the idle forces.
     * A zero, negative or NaN dt changes nothing.
     */
    ForceOutput update(const Telemetry* telemetry, double dtSeconds);

    /**
     * Where the outputs are heading for @p telemetry, before smoothing. rumbleHz has no target
     * of its own while idle; it stays where it is so a fading rumble does not also change pitch.
     */
    ForceOutput target(const Telemetry* telemetry) const;

    /** The outputs as of the last update. */
    const ForceOutput& output() const { return m_output; }

    const ForceSettings& settings() const { return m_settings; }

    /** The vehicle's forward acceleration as estimated from its speed telemetry, m/s^2. */
    double acceleration() const { return m_acceleration; }

    /** The surface whose pattern currently dominates the rumble. */
    Surface surface() const;

    /** Swaps the tuning while running. Outputs carry on from where they are and ease toward the new targets. */
    void setSettings(const ForceSettings& settings) { m_settings = settings; }

private:
    ForceOutput clamped(ForceOutput output) const;
    void        trackEngineStart(const Telemetry* telemetry, double dt);
    void        trackAcceleration(const Telemetry* telemetry, double dt);
    void        trackSurface(const Telemetry* telemetry, double dt);

    ForceSettings m_settings;
    ForceOutput   m_output;
    /** 1 while the engine is being started, then falling to 0 over engineStartFadeSeconds. */
    double m_startShake = 0.0;
    /** Whether the last sample was of an engine being started, and for how long that has been so. */
    bool   m_starting     = false;
    double m_startElapsed = 0.0;

    // The last telemetry sample the acceleration estimate was advanced with.
    bool          m_haveSpeed    = false;
    std::uint64_t m_speedSeq     = 0;
    std::int64_t  m_speedTimeMs  = 0;
    double        m_speed        = 0.0;
    double        m_speedMass    = 0.0;
    double        m_sinceSpeed   = 0.0;
    double        m_acceleration = 0.0;

    /** How much of each surface's pattern is in the mix; sums to 1. Crossing onto new ground shifts it over a moment. */
    std::array<double, kSurfaceCount> m_surfaceMix{1.0};
    // How far the wheels have come through the ground's texture, in lumps, ridges and tugs.
    double m_grainPhase = 0.0;
    double m_pulsePhase = 0.0;
    double m_tugPhase   = 0.0;
};

} // namespace bridge
