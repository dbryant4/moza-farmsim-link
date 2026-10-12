#pragma once

#include "moza/moza.hpp"
#include "moza/device_as.hpp"

#include <chrono>
#include <cstdint>

namespace bridge {

struct Rgb
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Rgb&) const = default;
};

/**
 * Drives the brightness and colour of the yoke handle's lights and puts them back afterwards.
 *
 * The handle (a MOZA MFY on the AY210) has one brightness, 0 to 15, and one colour for all of its
 * lights; it reports no per-light or per-hat control. Unlike a force effect these are settings the
 * handle keeps, which MOZA Cockpit also manages, so the values found at attach are restored when
 * this object stops or is destroyed, and writes are kept few: only when a step changes, and not
 * more often than a few times a second.
 */
class YokeLights
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr int kBrightest = 15;

    YokeLights() = default;
    ~YokeLights() { restore(); }

    YokeLights(const YokeLights&)            = delete;
    YokeLights& operator=(const YokeLights&) = delete;

    /**
     * Finds the yoke handle and remembers its brightness. Returns false, changing nothing, if no
     * handle is connected yet or its brightness cannot be read. Safe to call again later: the
     * handle is listed a moment after the base it is plugged into.
     */
    bool attach();

    bool attached() const { return m_handle.isOpen(); }
    /** The brightness the handle had when it was attached. */
    int original() const { return m_original; }
    /** The brightness last written, or the original if nothing has been. */
    int current() const { return m_current; }
    /** The colour last written, or the original. Meaningless if the handle's colour could not be read. */
    Rgb color() const { return m_color; }

    /** Number of distinct colours between the user's own and fully shifted; the shift moves in these steps. */
    static constexpr int kColorSteps = 8;

    /**
     * The colour at @p heat (0..1) for a handle whose own colour is @p original: unchanged at 0,
     * amber half way, red at 1, like a rev counter running into its red zone.
     */
    static Rgb colorAt(Rgb original, double heat);

    /**
     * Asks for brightness @p level, 0..15, and colour shift @p heat, 0..1; a negative level means
     * "as they were found". The handle only has whole brightness steps, so a level is held until
     * the request has moved most of a step away from it, which keeps a wandering engine speed from
     * flickering the lights between two steps; the colour moves in kColorSteps steps for the same
     * reason. At most one of the two is written per call.
     */
    void show(double level, double heat, Clock::time_point now);

    /** Writes the original brightness and colour back if they were changed. Safe to call more than once. */
    void restore();

private:
    bool writeLevel(int level);
    bool writeColor(Rgb color);

    moza::as::Stick   m_handle;
    int               m_original = 0;
    int               m_current  = 0;
    /** False when the handle's colour could not be read at attach: then it is never written. */
    bool              m_colorKnown    = false;
    Rgb               m_originalColor;
    Rgb               m_color;
    int               m_heatStep = 0;
    Clock::time_point m_lastWrite{};
};

} // namespace bridge
