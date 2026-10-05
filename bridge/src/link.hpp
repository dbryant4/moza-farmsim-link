#pragma once

#include "ffb_session.hpp"
#include "force_model.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace bridge {

/** How the force model's normalized outputs are scaled and routed onto the base. */
struct LinkTuning
{
    /** Multiplies every force, 0..1. Zero is how "forces off" reaches the device. */
    double strength = 1.0;
    /** Reverses the constant force, in case the game's steering sign and the base's disagree. */
    bool flipPull = false;
    /** Rumble goes to pitch by default: roll is light enough that shaking it would steer the vehicle. */
    bool rumbleOnRoll = false;
    /** Raw value that 100% maps to. */
    int fullScale = 32767;
};

/** Raw effect parameters for one moment, ready to send. */
struct DeviceForces
{
    std::int16_t  rollSpring     = 0;
    std::int16_t  pitchSpring    = 0;
    std::int16_t  rollDamper     = 0;
    std::int16_t  pitchDamper    = 0;
    /** Constant-force magnitude on roll, already in the base's sign convention. */
    std::int16_t  pull           = 0;
    std::int16_t  rumble         = 0;
    std::uint32_t rumblePeriodMs = 100;
    Axis          rumbleAxis     = Axis::Pitch;

    bool operator==(const DeviceForces&) const = default;
};

DeviceForces toDeviceForces(const ForceOutput& output, const LinkTuning& tuning);

struct LinkOptions
{
    std::filesystem::path telemetryPath;
    std::filesystem::path settingsPath;
    /** Port for the settings page on 127.0.0.1, or 0 for no page. */
    int    uiPort    = 8777;
    int    fullScale = 32767;
    /** Stop after this long; 0 runs until @p stop is set. */
    double seconds = 0.0;
    bool   verbose = false;
};

/**
 * Runs the Farming Simulator link until @p stop is set: reads the game's telemetry file, turns it
 * into forces on the base, and serves the settings page. Returns a process exit code.
 */
int runLink(moza::as::Wheelbase& base, const moza::HidDevice& hid, const LinkOptions& options, const std::atomic<bool>& stop);

} // namespace bridge
