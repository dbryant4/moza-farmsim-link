// Serves the tuning page with made-up settings and a simulated drive, so the page can be looked at
// and its controls tried in a browser without the yoke, the SDK or the game.
//
//   web_ui_demo [port] [settings-file]
//
// The loop below is also the shape the real main loop takes: re-read the settings when their
// revision moves, poll the two commands, publish status.

#include "settings.hpp"
#include "status_board.hpp"
#include "web_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int)
{
    g_stop = 1;
}

const double kPi = 3.14159265358979323846;

void registerSettings(bridge::Settings& settings)
{
    settings.addNumber("roll.spring_max", "Steering", "Centering strength at speed",
                       "How hard the yoke pulls back to center once the vehicle is moving at the full-centering speed.", "%", 0, 100,
                       1, 60);
    settings.addNumber("roll.spring_min", "Steering", "Centering strength at standstill",
                       "Centering left over when the vehicle is stopped, so the yoke does not flop.", "%", 0, 100, 1, 10);
    settings.addNumber("roll.spring_full_speed", "Steering", "Speed for full centering",
                       "Vehicle speed at which centering reaches its full strength.", "km/h", 5, 80, 1, 30);
    settings.addNumber("roll.damping", "Steering", "Weight",
                       "Resistance to turning the yoke quickly. More makes the steering feel heavier and calmer.", "%", 0, 100, 1, 25);
    settings.addNumber("roll.align", "Steering", "Self-aligning force",
                       "Pull toward straight ahead while cornering, growing with speed and steering angle.", "%", 0, 100, 1, 40);

    settings.addNumber("rumble.strength", "Surface", "Rumble strength", "How strongly rough ground shakes the yoke.", "%", 0, 100, 1,
                       35);
    settings.addNumber("rumble.freq_min", "Surface", "Rumble frequency at walking pace",
                       "Shake frequency when the vehicle is barely moving.", "Hz", 5, 40, 0.5, 12);
    settings.addNumber("rumble.freq_max", "Surface", "Rumble frequency at full speed",
                       "Shake frequency at 40 km/h and above. In between it rises with speed.", "Hz", 20, 120, 1, 45);
    settings.addToggle("rumble.field_only", "Surface", "Rumble off-road only", "Keep the yoke smooth on paved roads.", true);

    settings.addNumber("pitch.hold", "Pitch", "Hold stiffness",
                       "How firmly the pitch axis is held at center, since steering does not use it.", "%", 0, 100, 1, 80);
    settings.addNumber("pitch.damping", "Pitch", "Damping", "Resistance to moving the pitch axis quickly.", "%", 0, 100, 1, 30);

    settings.addNumber("general.gain", "General", "Overall strength", "Scales every force the bridge produces.", "%", 0, 100, 1, 70);
    settings.addNumber("general.smoothing", "General", "Smoothing",
                       "Time over which force changes are blended. Longer is softer but feels less direct.", "s", 0, 0.5, 0.01, 0.08);
    settings.addToggle("general.release_in_menu", "General", "Let go in menus",
                       "Release the yoke while the game is paused or showing a menu.", true);

    settings.addToggle("demo.freeze", "Demo", "Pause status updates",
                       "Stops this demo publishing status, to show how the page reports a main loop that has stalled.", false);
}

void registerStatus(bridge::StatusBoard& status)
{
    status.add("telemetry", "Game", "Telemetry");
    status.add("vehicle", "Game", "Vehicle");
    status.add("surface", "Game", "Surface");
    status.addRanged("speed", "Game", "Speed", "km/h", 0, 60, 1);
    status.addRanged("steering", "Game", "Steering", "", -1, 1);

    status.addRanged("yoke.roll", "Yoke", "Roll position", "", -1, 1);
    status.addRanged("yoke.pitch", "Yoke", "Pitch position", "", -1, 1);
    status.addRanged("torque.roll", "Yoke", "Roll torque", "N\xC2\xB7m", -1.7, 1.7);
    status.addRanged("torque.pitch", "Yoke", "Pitch torque", "N\xC2\xB7m", -1.7, 1.7);

    status.add("out.state", "Force output", "State");
    status.addRanged("out.centering", "Force output", "Centering", "%", 0, 100, 0);
    status.addRanged("out.damping", "Force output", "Weight", "%", 0, 100, 0);
    status.addRanged("out.align", "Force output", "Self-aligning", "%", -100, 100, 0);
    status.addRanged("out.rumble", "Force output", "Rumble", "%", 0, 100, 0);
    status.addRanged("out.rumble_freq", "Force output", "Rumble frequency", "Hz", 0, 120, 0);
    status.addRanged("out.pitch_hold", "Force output", "Pitch hold", "%", 0, 100, 0);
}

/** The settings as the loop uses them, re-read only when something changed. */
struct Tuning
{
    double springMax       = 0;
    double springMin       = 0;
    double springFullSpeed = 1;
    double damping         = 0;
    double align           = 0;
    double rumble          = 0;
    double rumbleFreqMin   = 0;
    double rumbleFreqMax   = 0;
    bool   rumbleFieldOnly = true;
    double pitchHold       = 0;
    double gain            = 0;
    double smoothing       = 0;
    bool   freeze          = false;

    void read(const bridge::Settings& settings)
    {
        springMax       = settings.get("roll.spring_max");
        springMin       = settings.get("roll.spring_min");
        springFullSpeed = settings.get("roll.spring_full_speed");
        damping         = settings.get("roll.damping");
        align           = settings.get("roll.align");
        rumble          = settings.get("rumble.strength");
        rumbleFreqMin   = settings.get("rumble.freq_min");
        rumbleFreqMax   = settings.get("rumble.freq_max");
        rumbleFieldOnly = settings.enabled("rumble.field_only");
        pitchHold       = settings.get("pitch.hold");
        gain            = settings.get("general.gain") / 100.0;
        smoothing       = settings.get("general.smoothing");
        freeze          = settings.enabled("demo.freeze");
    }
};

} // namespace

int main(int argc, char** argv)
{
    int port = 8777;
    if (argc > 1)
    {
        char* end = nullptr;
        const long parsed = std::strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed < 0 || parsed > 65535)
        {
            std::fprintf(stderr, "usage: %s [port] [settings-file]\n", argv[0]);
            return 2;
        }
        port = static_cast<int>(parsed);
    }
    std::error_code             ec;
    const std::filesystem::path file =
        argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::temp_directory_path(ec) / "moza_web_ui_demo.ini";

    bridge::Settings    settings;
    bridge::StatusBoard status;
    registerSettings(settings);
    registerStatus(status);

    const auto loaded = settings.load(file);
    for (const std::string& warning : loaded.warnings)
    {
        std::printf("settings: %s\n", warning.c_str());
    }

    bridge::WebUi ui(settings, status, file);
    std::string   error;
    if (!ui.start(static_cast<std::uint16_t>(port), &error))
    {
        std::fprintf(stderr, "web UI not started: %s\n", error.c_str());
        return 1;
    }
    std::printf("Tuning page:   http://127.0.0.1:%d/\n", static_cast<int>(ui.port()));
    std::printf("Settings file: %s (%s)\n", file.string().c_str(), loaded.fileFound ? "loaded" : "not there yet, using defaults");
    std::printf("Everything shown is simulated. Ctrl+C to stop.\n");
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Tuning        tuning;
    std::uint64_t seenRevision = settings.revision();
    std::uint64_t seenRelease  = status.releaseRequests();
    bool          forcesWereOn = status.forcesEnabled();
    tuning.read(settings);

    std::mt19937                           random(25);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    const double dt        = 0.05;
    double       t         = 0.0;
    double       speed     = 0.0;
    double       roll      = 0.0;
    double       pitch     = 0.0;
    double       steering  = 0.0;
    double       centering = 0.0;
    double       damping   = 0.0;
    double       align     = 0.0;
    double       rumble    = 0.0;
    double       pitchHold = 0.0;

    auto next = std::chrono::steady_clock::now();
    while (!g_stop)
    {
        next += std::chrono::milliseconds(50);
        std::this_thread::sleep_until(next);
        t += dt;

        // 1. Settings: one cheap comparison per iteration, a re-read only when something changed.
        if (const std::uint64_t revision = settings.revision(); revision != seenRevision)
        {
            seenRevision = revision;
            tuning.read(settings);
            std::printf("settings changed (revision %llu)\n", static_cast<unsigned long long>(revision));
            std::fflush(stdout);
        }

        // 2. Commands from the page.
        const bool forcesOn = status.forcesEnabled();
        if (const std::uint64_t release = status.releaseRequests(); release != seenRelease)
        {
            seenRelease = release;
            std::printf("release requested: every force dropped at once\n");
            std::fflush(stdout);
            centering = damping = align = rumble = pitchHold = 0.0;
        }
        if (forcesOn != forcesWereOn)
        {
            forcesWereOn = forcesOn;
            std::printf("forces %s\n", forcesOn ? "on" : "off");
            std::fflush(stdout);
        }

        // A drive that repeats every 64 s: pull away on the road, turn onto a field, work it, stop.
        const double lap     = std::fmod(t, 64.0);
        const bool   onField = lap >= 22.0 && lap < 54.0;
        const bool   stopped = lap >= 58.0;
        const double cruise  = stopped ? 0.0 : (onField ? 11.0 + 3.0 * std::sin(t * 0.5) : 42.0 + 6.0 * std::sin(t * 0.23));
        speed += (cruise - speed) * dt * (cruise < speed ? 0.9 : 0.35);
        speed = std::max(0.0, speed + (speed > 0.5 ? noise(random) * 0.15 : 0.0));

        // The driver: gentle corrections on the road, headland turns on the field.
        const double moving = std::clamp(speed / 4.0, 0.0, 1.0);
        const double wheel  = onField ? 0.85 * std::sin(t * 0.55) * std::sin(t * 0.55) * std::sin(t * 0.55) + 0.1 * std::sin(t * 2.1)
                                      : 0.22 * std::sin(t * 0.4) + 0.08 * std::sin(t * 1.7 + 1.0);
        roll += (wheel * moving - roll) * dt * 5.0;
        steering += (roll - steering) * dt * 8.0;
        const double bump = onField ? moving * noise(random) * 0.03 : 0.0;
        pitch += (0.04 * std::sin(t * 0.8) + bump - pitch) * dt * 6.0;

        // The force model, such as it is.
        const double speedFactor = std::clamp(speed / std::max(1.0, tuning.springFullSpeed), 0.0, 1.0);
        const double rough       = (onField || !tuning.rumbleFieldOnly) ? (onField ? 1.0 : 0.35) : 0.0;
        const double live        = forcesOn ? tuning.gain : 0.0;
        const double wantCenter  = live * (tuning.springMin + (tuning.springMax - tuning.springMin) * speedFactor);
        const double wantDamping = live * tuning.damping;
        const double wantAlign   = -live * tuning.align * steering * speedFactor;
        const double wantRumble  = live * tuning.rumble * rough * moving * (0.8 + 0.2 * noise(random));
        const double wantPitch   = live * tuning.pitchHold;
        // Switching forces off bypasses the smoothing: off means off now.
        const double blend = forcesOn ? dt / (tuning.smoothing + dt) : 1.0;
        centering += (wantCenter - centering) * blend;
        damping += (wantDamping - damping) * blend;
        align += (wantAlign - align) * blend;
        rumble += (wantRumble - rumble) * blend;
        pitchHold += (wantPitch - pitchHold) * blend;

        const double rumbleFreq = tuning.rumbleFreqMin + (tuning.rumbleFreqMax - tuning.rumbleFreqMin) * std::clamp(speed / 40.0, 0.0, 1.0);
        // The base's own idle torque is there whether or not any effect is.
        const double rollTorque = 0.03 + centering / 100.0 * -roll * 1.4 + align / 100.0 * 0.9 +
                                  rumble / 100.0 * 0.25 * std::sin(2.0 * kPi * rumbleFreq * t);
        const double pitchTorque = 0.04 + pitchHold / 100.0 * -pitch * 6.0;

        // 3. Status for the page.
        if (!tuning.freeze)
        {
            status.setText("telemetry", "connected, 60 Hz (simulated)");
            status.setText("vehicle", stopped ? "Tractor, parked" : "Tractor, driving");
            status.setText("surface", onField ? "Field" : "Road");
            status.set("speed", speed);
            status.set("steering", steering);
            status.set("yoke.roll", roll);
            status.set("yoke.pitch", pitch);
            status.set("torque.roll", std::clamp(rollTorque, -1.7, 1.7));
            status.set("torque.pitch", std::clamp(pitchTorque, -1.7, 1.7));
            status.setText("out.state", forcesOn ? "Applying forces" : "Off (master switch)");
            status.set("out.centering", centering);
            status.set("out.damping", damping);
            status.set("out.align", align);
            status.set("out.rumble", rumble);
            status.set("out.rumble_freq", rumble > 0.5 ? rumbleFreq : 0.0);
            status.set("out.pitch_hold", pitchHold);
        }
    }

    // The page goes first, so nothing can arrive while the rest of the program is shutting down.
    ui.stop();
    std::printf("\nstopped\n");
    return 0;
}
