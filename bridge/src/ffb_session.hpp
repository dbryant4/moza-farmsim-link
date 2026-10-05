#pragma once

#include "moza/moza.hpp"
#include "moza/device_as.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bridge {

const char* errnoName(moza::Errno err);

/** Renders any SDK value (scalars, strings, nested lists) for diagnostics. */
std::string formatValue(const moza::Value& value);

/**
 * Waits for the device service to enumerate an AS flight base and returns the first one whose
 * name contains @p nameFilter (empty matches any). Devices show up some time after
 * moza::initialize() returns, so this polls rather than reading the list once.
 */
std::optional<moza::DeviceInfo> findFlightBase(int                      timeoutMs,
                                               const std::string&       nameFilter = {},
                                               const std::atomic<bool>* cancel     = nullptr);

/**
 * Index of an axis's condition block inside an effect (HID PID "parameter block offset").
 * X is assumed to be roll (the SDK names its X-axis binding "Aileron"); not yet confirmed on hardware.
 */
enum class Axis : std::uint8_t
{
    Roll  = 0,
    Pitch = 1,
};

using EffectType = moza::as::wheelbase_type::MainCtrlFfbNewEffectRequestType;

/** Symmetric condition block: the same coefficient and saturation on both sides of the center. */
struct Condition
{
    std::int16_t  coefficient = 0;
    std::uint16_t saturation  = 0;
    std::int16_t  center      = 0;
    std::uint16_t deadBand    = 0;
};

/**
 * Builds a condition block from a 0-100 strength. @p fullScale is the raw value 100% maps to. The
 * SDK does not document it; torque measured on an AY210 fits 32767 for coefficient and center offset.
 */
Condition conditionFromPercent(double percent, int fullScale);

/** The enableAxis bits and direction angles (degrees * 100) sent with a force effect. */
struct ForceAxes
{
    bool          x               = false;
    bool          y               = false;
    bool          directionEnable = false;
    std::uint16_t directionX      = 0;
    std::uint16_t directionY      = 0;
};

/**
 * Addresses a force effect to one axis. Measured on an AY210: with direction enabled the first
 * angle selects the motor, 0 degrees for pitch and 90 for roll; roll at 0 degrees produces nothing.
 */
ForceAxes forceAlong(Axis axis);

/**
 * Owns the effects this process registers on a flight base through the MainCtrl_Ffb* commands,
 * and releases them when stopped or destroyed.
 *
 * Every effect is registered on both axes, so an axis is left free by sending it a zeroed block.
 */
class FfbSession
{
public:
    /** @param durationMs Duration sent with every update; forces lapse this long after updates stop. */
    FfbSession(moza::as::Wheelbase& base, std::uint16_t durationMs, bool verbose = false);
    ~FfbSession();

    FfbSession(const FfbSession&)            = delete;
    FfbSession& operator=(const FfbSession&) = delete;

    /** Frees every effect on the device and starts FFB calculation. */
    bool begin();

    /** Registers an effect and returns its block index, or 0 on failure. */
    std::uint8_t addEffect(EffectType type);

    bool setCondition(std::uint8_t effect, Axis axis, const Condition& condition);
    bool setConstant(std::uint8_t effect, std::int16_t magnitude, const ForceAxes& axes);
    /**
     * Sets a periodic (sine etc.) effect's amplitude and period in milliseconds. A zero @p durationMs
     * uses the session's duration; pass a longer one for an effect that is started once and left to
     * run, since re-sending Start restarts its waveform.
     */
    bool setPeriodic(std::uint8_t effect, std::int16_t magnitude, std::uint32_t periodMs, const ForceAxes& axes,
                     std::uint16_t durationMs = 0);
    bool start(std::uint8_t effect);

    /** Stops and frees this session's effects. Safe to call more than once. */
    void stop();

private:
    bool control(int value, const char* what);
    bool operate(std::uint8_t effect, moza::as::wheelbase_type::MainCtrlFfbEffectOperationRequestOperation operation);

    moza::as::Wheelbase&      m_base;
    std::uint16_t             m_durationMs;
    bool                      m_verbose;
    bool                      m_active = false;
    std::vector<std::uint8_t> m_effects;
};

} // namespace bridge
