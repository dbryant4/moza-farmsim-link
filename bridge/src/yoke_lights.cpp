#include "yoke_lights.hpp"

#include "ffb_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bridge {

namespace {

constexpr auto kShortestGap = std::chrono::milliseconds(300);
// How far a request must move from the level being shown before the level follows it.
constexpr double kStickiness = 0.75;

} // namespace

bool YokeLights::attach()
{
    if (attached())
    {
        return true;
    }

    auto [listErr, devices] = moza::getDeviceList();
    if (listErr != MOZA_ERR_NO_ERROR)
    {
        return false;
    }
    for (const auto& info : devices)
    {
        if (info.category != MOZA_DC_AS || (info.type & MOZA_DT_AS_STICK) == 0 || info.state == MOZA_DS_DISCONNECTED)
        {
            continue;
        }
        MOZA_DEVICE_HANDLE handle = MOZA_INVALID_DEVICE_HANDLE;
        if (Moza_DeviceOpen(info.id, &handle) != MOZA_ERR_NO_ERROR)
        {
            continue;
        }
        moza::as::Stick candidate(handle);

        auto [readErr, brightness] = candidate.ledBrightness().getSync(500);
        if (readErr != MOZA_ERR_NO_ERROR || brightness > kBrightest)
        {
            // Without knowing what to put back, leave this handle's lights alone.
            continue;
        }
        auto [colorErr, color] = candidate.ledColor().getSync(500);

        m_handle        = std::move(candidate);
        m_original      = brightness;
        m_current       = brightness;
        m_colorKnown    = colorErr == MOZA_ERR_NO_ERROR;
        m_originalColor = Rgb{color.r, color.g, color.b};
        m_color         = m_originalColor;
        m_heatStep      = 0;
        if (m_colorKnown)
        {
            std::printf("Yoke lights: '%s', brightness %d of %d, colour %d,%d,%d; this is what they go back to.\n", info.name, m_original,
                        kBrightest, m_color.r, m_color.g, m_color.b);
        }
        else
        {
            std::printf("Yoke lights: '%s', brightness %d of %d; this is what they go back to. Its colour could not be read (%s), "
                        "so the colour is left alone.\n",
                        info.name, m_original, kBrightest, errnoName(colorErr));
        }
        return true;
    }
    return false;
}

bool YokeLights::writeLevel(int level)
{
    const moza::Errno err = m_handle.ledBrightness().set(static_cast<std::uint8_t>(level));
    if (err != MOZA_ERR_NO_ERROR)
    {
        std::fprintf(stderr, "Setting the yoke lights to %d failed: %s\n", level, errnoName(err));
        return false;
    }
    m_current = level;
    return true;
}

bool YokeLights::writeColor(Rgb color)
{
    moza::as::stick_type::LedColorValue value{};
    value.r = color.r;
    value.g = color.g;
    value.b = color.b;
    const moza::Errno err = m_handle.ledColor().set(value);
    if (err != MOZA_ERR_NO_ERROR)
    {
        std::fprintf(stderr, "Setting the yoke lights' colour failed: %s\n", errnoName(err));
        return false;
    }
    m_color = color;
    return true;
}

Rgb YokeLights::colorAt(Rgb original, double heat)
{
    constexpr Rgb amber{255, 150, 0};
    constexpr Rgb red{255, 0, 0};

    const double share = std::isfinite(heat) ? std::clamp(heat, 0.0, 1.0) : 0.0;
    const auto   mix   = [](Rgb from, Rgb to, double t) {
        const auto channel = [t](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(a + (b - a) * t)); };
        return Rgb{channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
    };
    return share <= 0.5 ? mix(original, amber, share * 2.0) : mix(amber, red, (share - 0.5) * 2.0);
}

void YokeLights::show(double level, double heat, Clock::time_point now)
{
    if (!attached() || !std::isfinite(level))
    {
        return;
    }
    const bool asFound = level < 0.0;

    const double wanted = asFound ? m_original : std::clamp(level, 0.0, static_cast<double>(kBrightest));
    // The ends are exact: fully off must not linger one step up, nor full brightness one step down.
    const bool atAnEnd      = wanted <= 0.0 || wanted >= kBrightest || asFound;
    const bool levelChanged = std::abs(wanted - m_current) >= (atAnEnd ? 0.5 : kStickiness);

    // The colour moves in a few steps, and a step is held until the request is most of a step away.
    const double heatSteps   = asFound || !std::isfinite(heat) ? 0.0 : std::clamp(heat, 0.0, 1.0) * kColorSteps;
    const bool   heatAtAnEnd = heatSteps <= 0.0 || heatSteps >= kColorSteps;
    const bool   heatChanged = m_colorKnown && std::abs(heatSteps - m_heatStep) >= (heatAtAnEnd ? 0.5 : kStickiness);

    if ((!levelChanged && !heatChanged) || (m_lastWrite != Clock::time_point{} && now - m_lastWrite < kShortestGap))
    {
        return;
    }
    // One write per call, brightness first: it is what the eye notices.
    if (levelChanged)
    {
        if (writeLevel(static_cast<int>(std::lround(wanted))))
        {
            m_lastWrite = now;
        }
        return;
    }
    const int step = static_cast<int>(std::lround(heatSteps));
    if (writeColor(colorAt(m_originalColor, static_cast<double>(step) / kColorSteps)))
    {
        m_heatStep  = step;
        m_lastWrite = now;
    }
}

void YokeLights::restore()
{
    if (!attached())
    {
        return;
    }
    // Synchronous, so they are on their way before the caller closes devices and shuts the SDK down.
    if (m_current != m_original)
    {
        auto [err, readBack] = m_handle.ledBrightness().setSync(static_cast<std::uint8_t>(m_original), 500);
        if (err == MOZA_ERR_NO_ERROR || err == MOZA_ERR_TIMEOUT)
        {
            m_current = m_original;
        }
        else
        {
            std::fprintf(stderr, "Could not put the yoke lights back to %d: %s\n", m_original, errnoName(err));
        }
    }
    if (m_colorKnown && !(m_color == m_originalColor))
    {
        moza::as::stick_type::LedColorValue value{};
        value.r = m_originalColor.r;
        value.g = m_originalColor.g;
        value.b = m_originalColor.b;
        auto [err, readBack] = m_handle.ledColor().setSync(value, 500);
        if (err == MOZA_ERR_NO_ERROR || err == MOZA_ERR_TIMEOUT)
        {
            m_color    = m_originalColor;
            m_heatStep = 0;
        }
        else
        {
            std::fprintf(stderr, "Could not put the yoke lights' colour back: %s\n", errnoName(err));
        }
    }
}

} // namespace bridge
