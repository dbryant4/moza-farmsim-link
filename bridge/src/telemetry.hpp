#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace bridge {

/** What the vehicle is driving on, as far as the feel of it goes. Key `surface`, written as the number. */
enum class Surface
{
    /** The mod did not say (an older mod, or wheels in the air). Never written. */
    Unknown = 0,
    /** Asphalt, concrete, bridges, yard slabs. */
    Road,
    /** Gravel and dirt tracks: hard ground that is not paved. */
    Gravel,
    Grass,
    Snow,
    /** A heap of tipped material. */
    Heap,
    /** A field with a settled surface: sown, rolled, a seedbed, stubble left standing. */
    Field,
    /** A field broken up into clods: ploughed or cultivated. */
    Tilled,
    /** A field laid up in ridges. */
    Ridge,
};

constexpr int kSurfaceCount = 9;

/**
 * One sample of the file the FS25 mod rewrites. Keys, units and defaults are fixed by
 * docs/telemetry.md; the values are carried as written, with no range clamping.
 */
struct Telemetry
{
    /** Incremented by the mod on every write; the only required key. */
    std::uint64_t seq = 0;
    /** Key `t`: game time of the sample, ms. */
    std::int64_t timeMs = 0;
    /** The local player is driving a vehicle. When false the remaining fields may be defaults. */
    bool active = false;
    /** Forward speed, m/s; negative when reversing. */
    double speed = 0.0;
    /** Steering position as the game applies it to the wheels, -1..1; positive is steering right. */
    double steer = 0.0;
    /** Vehicle plus everything attached to it, kg. */
    double mass = 0.0;
    /** Engine speed, 1/min. */
    double rpm = 0.0;
    /** Engine load, 0..1. */
    double load = 0.0;
    /** This vehicle's idle and maximum engine speed, 1/min; 0 when the game does not say. */
    double rpmMin = 0.0;
    double rpmMax = 0.0;
    /** Largest longitudinal slip among the driven wheels, 0..1. */
    double slip = 0.0;
    /** Surface roughness, 0 (asphalt) .. 1 (freshly ploughed). */
    double rough = 0.0;
    /** What kind of ground most of the wheels are on. */
    Surface surface = Surface::Unknown;
    /** The vehicle is on a field. */
    bool field = false;
    /** Any attached implement is lowered. */
    bool lowered = false;
    /** Any attached implement is actively working the ground. */
    bool working = false;
    /** The engine is being started: triggered but not yet running. Never set for an electric vehicle. */
    bool starting = false;
    /** Key `startms`: how long this machine takes to start, ms; 0 when the game does not say. */
    double startMs = 0.0;
    /** Driven by an electric motor, with no combustion engine. */
    bool electric = false;
    /** How hard the left / right steered wheel is being jolted by an obstacle right now, 0..1. */
    double bumpLeft  = 0.0;
    double bumpRight = 0.0;
    /** How fast the machine's body is rocking sideways, rad/s; leaning over to the right is positive. */
    double rollRate = 0.0;
    /** How fast it is pitching, rad/s; nose rising is positive. */
    double pitchRate = 0.0;
    /** Vertical acceleration of the body, m/s^2; upward is positive. */
    double heave = 0.0;

    bool operator==(const Telemetry&) const = default;
};

/**
 * Parses the one line the telemetry file holds. Framing is strict, because it is what tells a
 * complete write from a partial one: the `MOZAFS 1 ` prefix, a final `end=1` token and a valid
 * `seq` are all required. Everything else is lenient: unknown keys are skipped, missing keys keep
 * their default, and a value that is not a finite plain decimal number falls back to its default
 * instead of costing the whole sample. One trailing "\n" or "\r\n" is accepted.
 *
 * Does not depend on the process locale.
 */
std::optional<Telemetry> parseTelemetry(std::string_view line);

enum class TelemetryState
{
    /** No complete sample has been read yet: the file is missing, empty, or not ours. */
    NeverSeen,
    /** `seq` changed within the staleness threshold; the sample describes the game right now. */
    Fresh,
    /** The file has stopped changing: the game is paused, in a menu, or gone. */
    Stale,
};

/** What TelemetryReader::poll() reports. */
struct TelemetryView
{
    TelemetryState state = TelemetryState::NeverSeen;
    /** The latest complete sample, whatever its age; all defaults until one has been seen. */
    Telemetry sample;
    /** Time since `seq` was last seen to change (since the first sample, if it never has). */
    std::chrono::steady_clock::duration sinceChange{};

    /** The sample while it is fresh, otherwise null: the form ForceModel::update() takes. */
    const Telemetry* fresh() const { return state == TelemetryState::Fresh ? &sample : nullptr; }
};

/**
 * Polls the telemetry file. Each poll opens the file, reads it once and closes it again, so the
 * file is never held between polls. Not thread-safe; poll from one thread.
 */
class TelemetryReader
{
public:
    using Clock = std::chrono::steady_clock;

    /** @param staleAfter With no `seq` change for this long, the sample is reported stale. */
    explicit TelemetryReader(std::filesystem::path path, Clock::duration staleAfter = std::chrono::milliseconds(500));

    /**
     * Reads the file and returns the current view, which stays valid until the next poll. A read
     * that fails or catches a partial write leaves the previous sample in place. @p now is taken as
     * an argument so tests can drive the clock.
     *
     * A sample only becomes fresh once `seq` has been seen to change. The first sample read after
     * start-up could be minutes old, left behind by a game that has since exited, so it is reported
     * stale until the file moves.
     */
    const TelemetryView& poll(Clock::time_point now);

    /** The view from the last poll, without touching the file. */
    const TelemetryView& view() const { return m_view; }

    const std::filesystem::path& path() const { return m_path; }

private:
    std::optional<Telemetry> read() const;

    std::filesystem::path m_path;
    Clock::duration       m_staleAfter;
    TelemetryView         m_view;
    Clock::time_point     m_lastChange{};
    bool                  m_seen   = false;
    bool                  m_moving = false;
};

/**
 * Where the mod writes on a default Windows install:
 * %USERPROFILE%\Documents\My Games\FarmingSimulator2025\modSettings\FS25_MozaYokeLink\telemetry.txt.
 * Empty when USERPROFILE is not set. A Documents folder redirected elsewhere (OneDrive) is not
 * followed; the path has to be given explicitly then.
 */
std::filesystem::path defaultTelemetryPath();

/**
 * Farming Simulator 25's own folder for this user (the one holding mods, modSettings and the
 * savegames), whether or not it exists. Empty where there is no such place.
 */
std::filesystem::path defaultGameFolder();

} // namespace bridge
