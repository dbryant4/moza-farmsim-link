#pragma once

#include "ffb_session.hpp"
#include "force_model.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

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
    /** Reverses which way the yoke leans under acceleration. */
    bool flipSway = false;
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
    /** Sine on pitch and sine on roll for a bump taken by both wheels, sharing one period. */
    std::int16_t  thump        = 0;
    std::int16_t  wiggle       = 0;
    std::uint32_t bumpPeriodMs = 140;
    /** Where the pitch spring is centered, so that it holds the sway instead of fighting it. */
    std::int16_t pitchCenter = 0;
    /** Constant-force magnitude on pitch that moves the yoke there, in the base's sign convention. */
    std::int16_t pitchPush = 0;

    bool operator==(const DeviceForces&) const = default;
};

/** Where pitch should sit for @p output, in half-travels from center; positive is a higher reported position. */
double pitchTargetFor(const ForceOutput& output, const LinkTuning& tuning);

/**
 * @p pitchPush is what the pitch position controller asks for, -1..1, positive toward higher
 * positions; it cannot come from @p output because it depends on where the yoke is.
 */
DeviceForces toDeviceForces(const ForceOutput& output, const LinkTuning& tuning, double pitchPush = 0.0);

/** How the yoke handle's lights follow the engine. Brightness is in the handle's own steps, 0..15. */
struct LightTuning
{
    bool   followEngine = false;
    /** Brightness with the engine idling. */
    double low = 3.0;
    /** Idle and full engine speed, 1/min, for a vehicle whose own range the game does not report. */
    double idleRpm = 900.0;
    double fullRpm = 2200.0;
    /** Whether the colour moves toward red near the top of the engine's range. */
    bool colorShift = true;
    /** Share of the range between idle and full speed at which the colour starts to change, 0..1. */
    double colorFrom = 0.6;
};

/** What the yoke's lights should show at one moment. */
struct LightTarget
{
    /** Brightness 0..15; negative means "as the user had them". */
    double level = -1.0;
    /** 0 is the user's own colour, 1 fully shifted to the high-revs colour. */
    double heat = 0.0;
};

/**
 * What the yoke's lights should show for @p telemetry: dark with the engine off, `low` at idle
 * rising to full at the top of this vehicle's own engine speed range, with the colour shifting
 * over the last part of it. With the feature off or nobody driving, the lights are left as found.
 */
LightTarget lightTargetFor(const Telemetry* telemetry, const LightTuning& tuning);

class Settings;
class StatusBoard;

/** The link's tunable values and live status, for a window that wants to show them. */
struct LinkPanel
{
    Settings*             settings = nullptr;
    StatusBoard*          status   = nullptr;
    std::filesystem::path settingsFile;
};

/** Registers every setting the link has, at its default. For the link itself and for anything that draws them without one. */
void registerLinkSettings(Settings& settings);
/** Registers the status items the link publishes. */
void registerLinkStatus(StatusBoard& status);

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

    /**
     * Called once the link is up, with the settings page's port (0 if there is no page), and then
     * about once a second with a one-line state for display. With onStatus set, the once-a-second
     * console line is not printed; changes of state are, so that a log file stays small.
     */
    std::function<void(int uiPort)>              onStarted;
    std::function<void(const std::string& text)> onStatus;

    /**
     * If set, told the two update settings (look for new versions at all; include pre-releases):
     * once before onStarted is called, and again whenever either changes.
     */
    std::function<void(bool check, bool prereleases)> onUpdateSettings;
    /**
     * If set, called with the panel once the link is up, and with null just before the objects it
     * points to go away. Whatever was given the panel must have let go of it by the time the
     * second call returns.
     */
    std::function<void(const LinkPanel* panel)> onPanel;
    /**
     * If set, called on every pass of the loop and between the steps of its orderly end, for a
     * guard that wants to know the loop is still going (see stall_guard.hpp).
     */
    std::function<void()> onLoop;
    /** If set, shown on the settings page as the program's version and asked for again about once a second. */
    std::function<std::string()> versionText;
};

/**
 * Runs the Farming Simulator link until @p stop is set: reads the game's telemetry file, turns it
 * into forces on the base, and serves the settings page. Returns a process exit code.
 */
int runLink(moza::as::Wheelbase& base, const moza::HidDevice& hid, const LinkOptions& options, const std::atomic<bool>& stop);

} // namespace bridge
