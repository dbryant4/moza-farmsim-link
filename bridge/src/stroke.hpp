#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bridge {

struct StrokeSettings
{
    /** Normalized axis positions (0-1) that count as the two ends of travel. */
    double low  = 0.03;
    double high = 0.97;
    /** Full end-to-end round trips before returning to center. */
    int    cycles = 2;
    /** Peak force magnitude, raw. */
    double force = 0.0;
    /** Time to ramp from zero to peak force, so reversals are not a jolt. */
    double rampSeconds = 0.5;
    /** At peak force with no movement for this long, the end is taken as reached. */
    double stallSeconds = 1.5;
    /** On the way back to center, force tapers from peak at this distance down to zero at center. */
    double centerTaper = 0.15;
    /** Within this distance of center the return is finished. */
    double centerTolerance = 0.02;
};

/**
 * Drives one axis end to end with a constant-force effect, using the axis position as feedback:
 * push toward one end until it gets there or stalls, reverse, and after the last cycle come back
 * toward center with a force that fades as it gets close, so it settles instead of overshooting.
 * Friction stops it a little short of exact center. On the AY210 a positive magnitude lowers the
 * reported position on both axes.
 */
class StrokeController
{
public:
    enum class Phase
    {
        TowardLow,
        TowardHigh,
        TowardCenter,
        Done,
    };

    explicit StrokeController(const StrokeSettings& settings) : m_settings(settings) {}

    /** Advances by @p dt seconds given the current position and returns the magnitude to send. */
    std::int16_t update(double position, double dt)
    {
        if (!m_started)
        {
            m_started      = true;
            m_progressMark = position;
        }

        trackStall(position, dt);
        if (m_phase != Phase::Done && (reachedTarget(position) || stalled()))
        {
            // Settling short of exact center is how the return normally ends; only a missed end counts.
            if (!reachedTarget(position) && m_phase != Phase::TowardCenter)
            {
                ++m_stalls;
            }
            advance();
        }

        const double target = targetForce(position);
        const double step   = m_settings.rampSeconds > 0 ? m_settings.force * dt / m_settings.rampSeconds : m_settings.force;
        // Building force up is ramped to avoid a jolt; easing off is allowed to be quick, otherwise
        // the fading return to center lags the position and shoves the axis past the middle.
        const bool   easingOff = std::abs(target) < std::abs(m_force) && target * m_force >= 0.0;
        const double limit     = easingOff ? step * kReleaseSpeedup : step;
        m_force                = std::clamp(target, m_force - limit, m_force + limit);
        return static_cast<std::int16_t>(std::clamp(std::lround(m_force), -32767L, 32767L));
    }

    Phase phase() const { return m_phase; }
    /** True once the stroke is over and the force has ramped back to zero. */
    bool finished() const { return m_phase == Phase::Done && std::abs(m_force) < 1.0; }
    /** How many ends were given up on because the axis stopped moving short of them. */
    int stalls() const { return m_stalls; }

    const char* phaseName() const
    {
        switch (m_phase)
        {
        case Phase::TowardLow: return "toward low end";
        case Phase::TowardHigh: return "toward high end";
        case Phase::TowardCenter: return "back to center";
        default: return "done";
        }
    }

private:
    double targetForce(double position) const
    {
        switch (m_phase)
        {
        case Phase::TowardLow: return m_settings.force;
        case Phase::TowardHigh: return -m_settings.force;
        case Phase::TowardCenter: return m_settings.force * std::clamp((position - 0.5) / m_settings.centerTaper, -1.0, 1.0);
        default: return 0.0;
        }
    }

    bool reachedTarget(double position) const
    {
        switch (m_phase)
        {
        case Phase::TowardLow: return position <= m_settings.low;
        case Phase::TowardHigh: return position >= m_settings.high;
        case Phase::TowardCenter: return std::abs(position - 0.5) <= m_settings.centerTolerance;
        default: return false;
        }
    }

    void trackStall(double position, double dt)
    {
        if (std::abs(position - m_progressMark) > 0.005)
        {
            m_progressMark = position;
            m_stillFor     = 0.0;
        }
        else if (m_phase == Phase::TowardCenter || std::abs(m_force) >= m_settings.force - 1.0)
        {
            // Toward an end only time at peak force counts, since the axis is not expected to move
            // during the ramp. Toward center the force fades by design, so any standstill counts.
            m_stillFor += dt;
        }
    }

    bool stalled() const { return m_stillFor >= m_settings.stallSeconds; }

    void advance()
    {
        m_stillFor = 0.0;
        if (m_phase == Phase::TowardLow)
        {
            m_phase = Phase::TowardHigh;
        }
        else if (m_phase == Phase::TowardHigh)
        {
            m_phase = ++m_completed >= m_settings.cycles ? Phase::TowardCenter : Phase::TowardLow;
        }
        else
        {
            m_phase = Phase::Done;
        }
    }

    static constexpr double kReleaseSpeedup = 5.0;

    StrokeSettings m_settings;
    Phase          m_phase         = Phase::TowardLow;
    bool           m_started       = false;
    int            m_completed     = 0;
    int            m_stalls        = 0;
    double         m_force         = 0.0;
    double         m_progressMark  = 0.0;
    double         m_stillFor      = 0.0;
};

} // namespace bridge
