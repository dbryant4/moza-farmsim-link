#pragma once

#include "telemetry.hpp"

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
     * Engine vibration at full load while the engine turns, 0..1, at crankshaft frequency (rpm / 60)
     * held inside the rumble band. Off by default: it is the only rumble present at standstill.
     */
    double engineRumble = 0.0;
    /** Fraction of engineRumble left at zero engine load, 0..1. */
    double engineRumbleIdleShare = 0.5;

    // Pitch is not a control in the game; it is only held centered so it does not flop in and out.

    /** Pitch centering stiffness while driving, 0..1. */
    double pitchSpring = 0.80;
    /** Pitch damping while driving, 0..1. */
    double pitchDamper = 0.30;

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

    /** Swaps the tuning while running. Outputs carry on from where they are and ease toward the new targets. */
    void setSettings(const ForceSettings& settings) { m_settings = settings; }

private:
    ForceOutput clamped(ForceOutput output) const;

    ForceSettings m_settings;
    ForceOutput   m_output;
};

} // namespace bridge
