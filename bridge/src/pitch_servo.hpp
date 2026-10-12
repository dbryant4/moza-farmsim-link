#pragma once

#include <algorithm>
#include <cmath>

namespace bridge {

/**
 * Tuning of the pitch position controller. Positions are in half-travels: 0 is center, +/-1 the
 * ends. Pushes are fractions of the device's full constant force.
 */
struct PitchServoSettings
{
    /** Within this distance of the target nothing is pushed, so the axis rests instead of hunting. */
    double deadband = 0.03;
    /** Push per half-travel of error beyond the deadband. */
    double gain = 1.5;
    /** How fast the extra push builds while the axis is not moving, per second. */
    double rampPerSecond = 1.2;
    /** Push taken off per half-travel-per-second of speed, to stop it running away once it breaks free. */
    double damping = 0.35;
    /** Below this speed toward the target, in half-travels per second, the axis counts as stuck. */
    double stuckSpeed = 0.05;
    /** Largest push in either direction. */
    double maxPush = 0.8;
    /** Fastest the push may change, per second. Keeps the start and the stop from being a knock. */
    double slewPerSecond = 6.0;
};

/**
 * Moves the yoke's pitch axis to a target position with a constant-force effect.
 *
 * Pitch on the AY210 does not move at all below about 0.15 N*m, which is more than a third of the
 * constant force available, so a push in proportion to the error would do nothing for small errors
 * and lurch for large ones. Instead the push builds up gradually until the axis starts to move,
 * holds there while it travels, and lets go as soon as it is close enough.
 */
class PitchServo
{
public:
    explicit PitchServo(const PitchServoSettings& settings = {}) : m_settings(settings) {}

    /** Returns the push for this step: positive pushes toward higher positions. */
    double update(double target, double position, double dt)
    {
        const PitchServoSettings& s = m_settings;
        if (!(dt > 0.0) || !std::isfinite(target) || !std::isfinite(position))
        {
            return m_push;
        }

        const double speed = m_started ? (position - m_lastPosition) / dt : 0.0;
        m_lastPosition     = position;
        m_started          = true;

        const double error = target - position;
        double       wanted = 0.0;
        if (std::abs(error) <= s.deadband)
        {
            m_boost = 0.0;
        }
        else
        {
            const double direction = error > 0.0 ? 1.0 : -1.0;
            if (direction != m_direction)
            {
                // The target is now on the other side: what was built up pushes the wrong way.
                m_boost     = 0.0;
                m_direction = direction;
            }
            if (speed * direction < s.stuckSpeed)
            {
                m_boost = std::min(m_boost + s.rampPerSecond * dt, s.maxPush);
            }
            wanted = direction * (s.gain * (std::abs(error) - s.deadband) + m_boost) - s.damping * speed;
            wanted = std::clamp(wanted, -s.maxPush, s.maxPush);
        }

        const double step = s.slewPerSecond * dt;
        m_push            = std::clamp(wanted, m_push - step, m_push + step);
        return m_push;
    }

    /** Forgets everything and pushes nothing, e.g. when forces are switched off. */
    void reset()
    {
        m_push      = 0.0;
        m_boost     = 0.0;
        m_direction = 0.0;
        m_started   = false;
    }

    double push() const { return m_push; }

private:
    PitchServoSettings m_settings;
    double             m_push         = 0.0;
    double             m_boost        = 0.0;
    double             m_direction    = 0.0;
    double             m_lastPosition = 0.0;
    bool               m_started      = false;
};

} // namespace bridge
