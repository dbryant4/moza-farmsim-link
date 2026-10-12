// MVP: hold a fixed spring + damper on a MOZA flight base through the MOZA SDK, with no game
// telemetry involved. Its job is to prove the SDK's force feedback path works on the real yoke.

#include "ffb_session.hpp"
#include "link.hpp"
#include "stall_guard.hpp"
#include "stroke.hpp"
#include "telemetry.hpp"

#include "moza/private/version.h"

#include <atomic>
#include <cstdarg>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

#ifdef _WIN32
#include "update_check.hpp"
#include "updater.hpp"
#endif

#ifdef BRIDGE_TRAY
#include "mod_sync.hpp"
#include "tray.hpp"
#endif

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "dev"
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_cleanedUp{false};
std::atomic<bool> g_verbose{false};
std::atomic<int>  g_asyncErrors{0};
std::atomic<int>  g_deviceId{-1};

/** The last thing that stopped the bridge from doing its job, for the windowless build to show the user. */
std::string       g_problem;
std::atomic<bool> g_deviceLost{false};

/** Reports why the bridge cannot carry on: to stderr, and remembered for a message box where there is no console. */
void problem(const char* format, ...)
{
    char    text[600];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "%s\n", text);
    g_problem = text;
}

/** Where releases are published: "owner/name" on GitHub. */
[[maybe_unused]] constexpr const char* kRepository = "dbryant4/moza-farmsim-link";

// The link loop calls into the MOZA SDK on every pass, and the SDK has been seen to lock itself up
// for good (two of its own threads waiting on each other, 2026-10-11). Nothing can free a thread
// stuck like that, so the loop is watched, and a stuck process is ended and started afresh.

/** Longest the loop may go without a pass. Its slowest honest step, finding the yoke handle, takes about a second. */
constexpr auto kStallLimit = std::chrono::seconds(10);
/** A run shorter than this that ends stuck counts toward giving up. */
[[maybe_unused]] constexpr auto kSettledRun = std::chrono::minutes(10);
[[maybe_unused]] constexpr int  kMostRestarts = 3;

Clock::time_point g_linkStarted{};
/** How many times in a row the link has restarted itself after getting stuck; 0 when a person started it. */
[[maybe_unused]] int g_restarts = 0;

[[noreturn]] void endStuckProcess(int code)
{
    std::fflush(nullptr);
#ifdef _WIN32
    // Not exit(): that would wait for the very threads that are stuck.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
}

#ifdef BRIDGE_TRAY
void restartAfterStall();
#endif

void onLinkStalled(std::chrono::milliseconds silent)
{
    std::fprintf(stderr,
                 "The link has not moved for %.0f s: it is stuck inside the MOZA SDK, which cannot be undone from within. "
                 "The base lets go of its forces by itself. Ending this process.\n",
                 static_cast<double>(silent.count()) / 1000.0);
#ifdef BRIDGE_TRAY
    // Not when the user had already asked it to stop: then ending is all that was wanted.
    if (!g_stop)
    {
        restartAfterStall();
    }
#endif
    endStuckProcess(71);
}

#ifdef BRIDGE_TRAY
bridge::Tray* g_tray = nullptr;
/** Held by the helper threads while they use g_tray, and by main while it takes the icon away. */
std::mutex g_trayMutex;

template <typename Use>
void withTray(Use use)
{
    std::scoped_lock lock(g_trayMutex);
    if (g_tray != nullptr)
    {
        use(*g_tray);
    }
}

bridge::Updater*  g_updater = nullptr;
/** Follows the "look for new versions" setting once the link has loaded its settings. */
std::atomic<bool> g_checkUpdates{true};
/** Follows the "include pre-releases" setting the same way. */
std::atomic<bool> g_prereleases{false};
std::atomic<bool> g_updateInstallable{false};
std::mutex        g_updateMutex;
/** The newer release that was found, if any; guarded by g_updateMutex. */
std::string       g_newerVersion;
#endif

std::atomic<const char*> g_shutdownStep{"starting teardown"};
/** What the process exits with if teardown has to be cut short. */
std::atomic<int>         g_exitCode{70};

void shutdownStep(const char* what)
{
    g_shutdownStep = what;
    if (g_verbose)
    {
        std::printf("shutdown: %s\n", what);
    }
}

/**
 * Ends the process a few seconds after teardown begins if it has not finished by itself.
 *
 * The MOZA SDK has blocked forever during teardown on the real base. Effects carry a finite
 * duration, so the base lets go on its own once updates stop, whether or not teardown completes.
 */
void armExitWatchdog()
{
    static std::atomic<bool> armed{false};
    if (armed.exchange(true))
    {
        return;
    }
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::fprintf(stderr, "Shutdown stalled while %s; ending the process.\n", g_shutdownStep.load());
        std::fflush(nullptr);
#ifdef _WIN32
        // Not exit(): that runs the SDK DLL's detach code, which is what may be stuck.
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_exitCode.load()));
#else
        std::_Exit(g_exitCode.load());
#endif
    }).detach();
}

struct ExitWatchdogGuard
{
    ~ExitWatchdogGuard() { armExitWatchdog(); }
};

struct Options
{
    /** Hold a fixed spring + damper instead of linking to the game. */
    bool        hold           = false;
    /** Run even though a flight simulator that uses the yoke is running. */
    bool        allowFlightSim = false;
    std::string telemetryPath;
    std::string settingsPath;
    int         uiPort = 8777;
    bool        list    = false;
    bool        verbose = false;
    bool        restart = true;
    bool        roll    = true;
    bool        pitch   = false;
    std::string device;
    double      springPercent = 30.0;
    double      damperPercent = 20.0;
    int         fullScale     = 32767;
    int         durationMs    = 1000;
    int         refreshMs     = 200;
    double      seconds       = 0.0;
    double      waitSeconds   = 10.0;
    bool        waitGiven     = false;
    /** Tell a running windowless instance to exit, then exit. */
    bool        quit          = false;
    /** Ask GitHub about the latest release and exit; with fetchUpdate, also download and verify its installer. */
    bool        checkUpdate   = false;
    bool        fetchUpdate   = false;
    /** For the two above: count releases marked pre-release as well. */
    bool        prereleases   = false;
    /** Set by the program when it starts itself again after getting stuck: which restart in a row this is, and the process to wait for. */
    int         restarted     = 0;
    unsigned long afterPid    = 0;
    /** Test option: freeze the link loop this many seconds in, as a call into the SDK that never returns would. 0 is off. */
    double      testStallAfter = 0.0;
    /** Axis whose spring center is swept back and forth, or -1 for a fixed center. */
    int         sweepAxis             = -1;
    double      sweepAmplitudePercent = 20.0;
    double      sweepPeriodSeconds    = 4.0;
    /** Axis pushed back and forth with a constant-force effect, or -1 for none. */
    int         forceAxis             = -1;
    double      forceAmplitudePercent = 10.0;
    double      forcePeriodSeconds    = 4.0;
    /** Axis shaken by a periodic sine effect the base generates itself, or -1 for none. */
    int         sineAxis             = -1;
    double      sineAmplitudePercent = 10.0;
    int         sinePeriodMs         = 100;
    /** Axis driven end to end under position feedback, or -1 for none. */
    int         strokeAxis         = -1;
    double      strokeForcePercent = 80.0;
    int         strokeCycles       = 2;
};

void printUsage()
{
    std::puts(
        "Usage: moza_fs_bridge [options]\n"
        "\n"
        "With no test option: links a MOZA flight base to Farming Simulator 25. Reads the telemetry\n"
        "file written by the FS25_MozaYokeLink mod, turns it into steering forces, and serves a\n"
        "settings page at http://127.0.0.1:8777/ until Ctrl+C.\n"
        "\n"
        "  --telemetry PATH   Telemetry file (default: the mod's file in the FS25 profile folder)\n"
        "  --settings PATH    Settings file (default: moza_fs_bridge.ini next to this program)\n"
        "  --ui-port N        Port for the settings page (default: 8777)\n"
        "  --no-ui            Do not serve the settings page\n"
        "  --allow-flight-sim Run even while X-Plane or another flight simulator is running\n"
        "\n"
        "Test options (no game needed):\n"
        "  --list             Print devices and force feedback settings, then exit (no forces)\n"
        "  --check-update     Ask GitHub for the latest release and print it, then exit (no yoke needed)\n"
        "  --fetch-update     The same, and also download and verify its installer without running it\n"
        "  --prereleases      With either of those: count releases marked pre-release as well\n"
        "  --test-stall-after S  In link mode: freeze the loop S seconds in, to try the guard that ends a stuck link\n"
        "  --hold             Hold a fixed spring + damper (see --axis, --spring, --damper)\n"
        "  --device TEXT      Use the flight base whose name contains TEXT (default: first found)\n"
        "  --axis AXIS        roll, pitch or both (default: roll)\n"
        "  --spring PCT       Centering strength, 0-100 (default: 30)\n"
        "  --damper PCT       Damping strength, 0-100 (default: 20)\n"
        "  --scale RAW        Raw value that 100% maps to (default: 32767)\n"
        "  --duration-ms MS   Effect duration sent with each update (default: 1000)\n"
        "  --refresh-ms MS    Interval between updates (default: 200)\n"
        "  --sweep AXIS       Move the spring center of roll or pitch back and forth, so the\n"
        "                     yoke travels on its own (implies a spring on that axis)\n"
        "  --sweep-amplitude PCT  Sweep size as a share of --scale, 0-100 (default: 20)\n"
        "  --sweep-period S   Seconds per full back-and-forth cycle (default: 4)\n"
        "  --force AXIS       Push roll or pitch back and forth with a constant-force effect;\n"
        "                     the spring and damper on that axis stay on and limit travel and speed\n"
        "  --force-amplitude PCT  Peak force as a share of --scale, 0-100 (default: 10)\n"
        "  --force-period S   Seconds per full back-and-forth cycle (default: 4)\n"
        "  --sine AXIS        Shake roll or pitch with a sine effect generated by the base\n"
        "  --sine-amplitude PCT, --sine-period-ms MS   Its strength (default: 10) and period (default: 100)\n"
        "  --stroke AXIS      Drive roll or pitch all the way to each end and back to center,\n"
        "                     watching its position; the centering spring on that axis is off\n"
        "  --stroke-force PCT Push strength as a share of --scale, 0-100 (default: 80)\n"
        "  --stroke-cycles N  End-to-end round trips before centering (default: 2)\n"
        "  --no-restart       Re-send parameters only, without re-sending Start on each update\n"
        "  --seconds S        Stop after S seconds; 0 runs until Ctrl+C (default: 0)\n"
        "  --wait S           Seconds to wait for the base to appear (default: 10)\n"
        "  --verbose          Print SDK log output and command results\n"
        "  --version          Print the version\n"
        "  --help             Show this text");
}

[[noreturn]] void usageError(const std::string& message)
{
    std::fprintf(stderr, "%s\n\n", message.c_str());
    printUsage();
    std::exit(64);
}

double parseNumber(const char* option, const char* text, double min, double max)
{
    char*        end   = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= min && value <= max))
    {
        usageError(std::string(option) + " expects a number between " + std::to_string(static_cast<long long>(min)) +
                   " and " + std::to_string(static_cast<long long>(max)) + ", got '" + text + "'");
    }
    return value;
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg  = argv[i];
        const auto        next = [&]() -> const char* {
            if (i + 1 >= argc)
            {
                usageError(arg + " needs a value");
            }
            return argv[++i];
        };

        if (arg == "--version")
        {
            std::puts("moza_fs_bridge " BRIDGE_VERSION);
            std::exit(0);
        }
        if (arg == "--help" || arg == "-h")
        {
            printUsage();
            std::exit(0);
        }
        else if (arg == "--list") options.list = true;
        else if (arg == "--hold") options.hold = true;
        else if (arg == "--allow-flight-sim") options.allowFlightSim = true;
        else if (arg == "--telemetry") options.telemetryPath = next();
        else if (arg == "--settings") options.settingsPath = next();
        else if (arg == "--ui-port") options.uiPort = static_cast<int>(parseNumber("--ui-port", next(), 1, 65535));
        else if (arg == "--no-ui") options.uiPort = 0;
        else if (arg == "--verbose") options.verbose = true;
        else if (arg == "--no-restart") options.restart = false;
        else if (arg == "--device") options.device = next();
        else if (arg == "--axis")
        {
            const std::string axis = next();
            if (axis != "roll" && axis != "pitch" && axis != "both")
            {
                usageError("--axis expects roll, pitch or both, got '" + axis + "'");
            }
            options.roll  = axis != "pitch";
            options.pitch = axis != "roll";
        }
        else if (arg == "--sweep")
        {
            const std::string axis = next();
            if (axis != "roll" && axis != "pitch")
            {
                usageError("--sweep expects roll or pitch, got '" + axis + "'");
            }
            options.sweepAxis = static_cast<int>(axis == "roll" ? bridge::Axis::Roll : bridge::Axis::Pitch);
        }
        else if (arg == "--sweep-amplitude") options.sweepAmplitudePercent = parseNumber("--sweep-amplitude", next(), 0, 100);
        else if (arg == "--sweep-period") options.sweepPeriodSeconds = parseNumber("--sweep-period", next(), 1, 120);
        else if (arg == "--force")
        {
            const std::string axis = next();
            if (axis != "roll" && axis != "pitch")
            {
                usageError("--force expects roll or pitch, got '" + axis + "'");
            }
            options.forceAxis = static_cast<int>(axis == "roll" ? bridge::Axis::Roll : bridge::Axis::Pitch);
        }
        else if (arg == "--sine")
        {
            const std::string axis = next();
            if (axis != "roll" && axis != "pitch")
            {
                usageError("--sine expects roll or pitch, got '" + axis + "'");
            }
            options.sineAxis = static_cast<int>(axis == "roll" ? bridge::Axis::Roll : bridge::Axis::Pitch);
        }
        else if (arg == "--sine-amplitude") options.sineAmplitudePercent = parseNumber("--sine-amplitude", next(), 0, 100);
        else if (arg == "--sine-period-ms") options.sinePeriodMs = static_cast<int>(parseNumber("--sine-period-ms", next(), 1, 60000));
        else if (arg == "--stroke")
        {
            const std::string axis = next();
            if (axis != "roll" && axis != "pitch")
            {
                usageError("--stroke expects roll or pitch, got '" + axis + "'");
            }
            options.strokeAxis = static_cast<int>(axis == "roll" ? bridge::Axis::Roll : bridge::Axis::Pitch);
        }
        else if (arg == "--stroke-force") options.strokeForcePercent = parseNumber("--stroke-force", next(), 0, 100);
        else if (arg == "--stroke-cycles") options.strokeCycles = static_cast<int>(parseNumber("--stroke-cycles", next(), 1, 20));
        else if (arg == "--force-amplitude") options.forceAmplitudePercent = parseNumber("--force-amplitude", next(), 0, 100);
        else if (arg == "--force-period") options.forcePeriodSeconds = parseNumber("--force-period", next(), 1, 120);
        else if (arg == "--spring") options.springPercent = parseNumber("--spring", next(), 0, 100);
        else if (arg == "--damper") options.damperPercent = parseNumber("--damper", next(), 0, 100);
        else if (arg == "--scale") options.fullScale = static_cast<int>(parseNumber("--scale", next(), 1, 65535));
        else if (arg == "--duration-ms") options.durationMs = static_cast<int>(parseNumber("--duration-ms", next(), 0, 65535));
        else if (arg == "--refresh-ms") options.refreshMs = static_cast<int>(parseNumber("--refresh-ms", next(), 10, 60000));
        else if (arg == "--seconds") options.seconds = parseNumber("--seconds", next(), 0, 86400);
        else if (arg == "--wait")
        {
            options.waitSeconds = parseNumber("--wait", next(), 0, 600);
            options.waitGiven   = true;
        }
        else if (arg == "--quit") options.quit = true;
        else if (arg == "--check-update") options.checkUpdate = true;
        else if (arg == "--fetch-update") options.checkUpdate = options.fetchUpdate = true;
        else if (arg == "--prereleases") options.prereleases = true;
        else if (arg == "--test-stall-after") options.testStallAfter = parseNumber("--test-stall-after", next(), 0, 86400);
        else if (arg == "--restarted") options.restarted = static_cast<int>(parseNumber("--restarted", next(), 0, 1000));
        else if (arg == "--after-pid") options.afterPid = static_cast<unsigned long>(parseNumber("--after-pid", next(), 0, 4294967295.0));
        else usageError("Unknown option: " + arg);
    }

    // A swept or pushed axis needs its spring on whatever --axis said.
    if (options.strokeAxis >= 0 && (options.forceAxis >= 0 || options.sweepAxis >= 0))
    {
        usageError("--stroke cannot be combined with --force or --sweep");
    }
    const auto driven = [&](bridge::Axis axis) {
        return options.sineAxis == static_cast<int>(axis) || options.sweepAxis == static_cast<int>(axis) ||
               options.forceAxis == static_cast<int>(axis) ||
               options.strokeAxis == static_cast<int>(axis);
    };
    options.roll  = options.roll || driven(bridge::Axis::Roll);
    options.pitch = options.pitch || driven(bridge::Axis::Pitch);
    return options;
}

#ifdef _WIN32
BOOL WINAPI onConsoleControl(DWORD type)
{
    g_stop = true;
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
    {
        return TRUE;
    }
    // Closing the window, logoff and shutdown end the process as soon as this handler returns,
    // so hold it until the main thread has released the forces.
    for (int i = 0; i < 400 && !g_cleanedUp; ++i)
    {
        Sleep(10);
    }
    return TRUE;
}
#else
void onSignal(int)
{
    g_stop = true;
}
#endif

void installStopHandler()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif
}

// Runs on an SDK thread. Pointers inside the event are only valid during the call.
void onEvent(const MOZA_EVENT_DATA* event, void*)
{
    if (!event)
    {
        return;
    }

    switch (event->type)
    {
    case MOZA_ET_SERVICE_CONNECTION_CLOSED:
        std::fprintf(stderr, "The MOZA device service closed the connection.\n");
        g_deviceLost = true;
        g_stop       = true;
        break;
    case MOZA_ET_DEVICE_DISCONNECTED:
        if (static_cast<int>(event->data.deviceDisconnected.deviceId) == g_deviceId)
        {
            std::fprintf(stderr, "The flight base disconnected.\n");
            g_deviceLost = true;
            g_stop       = true;
        }
        break;
    case MOZA_ET_DEVICE_COMMAND_ERROR_OCCURRED:
        // Asynchronous sends report their failures here rather than from the send call.
        if (g_asyncErrors++ < 5 || g_verbose)
        {
            const char* name = event->data.commandErrorOccurred.commandName;
            std::fprintf(stderr, "Command %d (%s) failed: %s\n",
                         static_cast<int>(event->data.commandErrorOccurred.commandId), name ? name : "?",
                         bridge::errnoName(event->data.commandErrorOccurred.error));
        }
        break;
    default:
        break;
    }
}

// Without a log callback the SDK writes its own debug log to the console, so one is always
// registered and simply stays quiet unless --verbose was given.
void onLog(const MOZA_LOG_CONTEXT* context, void*)
{
    if (g_verbose && context && context->message)
    {
        std::fprintf(stderr, "[sdk] %.*s\n", static_cast<int>(context->messageSize), context->message);
    }
}

void printDevices()
{
    auto [err, devices] = moza::getDeviceList();
    if (err != MOZA_ERR_NO_ERROR)
    {
        std::fprintf(stderr, "getDeviceList failed: %s\n", bridge::errnoName(err));
        return;
    }

    std::printf("MOZA devices (%zu):\n", devices.size());
    for (const auto& info : devices)
    {
        std::printf("  id %u  %-32s category %d  type 0x%04x  state %d  usb %04x:%04x  hid devices %d\n",
                    static_cast<unsigned>(info.id), info.name, static_cast<int>(info.category),
                    static_cast<unsigned>(info.type), static_cast<int>(info.state),
                    static_cast<unsigned>(info.vendorId), static_cast<unsigned>(info.productId),
                    static_cast<int>(info.hidDeviceCount));
    }
}

/** True for the names MOZA gives lighting parameters and commands. */
bool isLightingName(const char* name)
{
    for (const char* word : {"Led", "LED", "Light", "Lamp", "Rgb", "RGB", "Bright"})
    {
        if (std::strstr(name, word) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/**
 * Prints, for every MOZA device, the lighting parameters it reports and their current values.
 * What a connected device lists is the authority on what it supports; the SDK headers describe
 * the whole product family. Read-only.
 */
void printLighting()
{
    // A handle plugged into the base turns up a moment after the base itself, so wait until the
    // list has stopped growing rather than taking the first answer.
    std::vector<moza::DeviceInfo> devices;
    const auto                    deadline   = Clock::now() + std::chrono::seconds(5);
    auto                          lastGrowth = Clock::now();
    while (Clock::now() < deadline && Clock::now() - lastGrowth < std::chrono::milliseconds(1500))
    {
        auto [listErr, current] = moza::getDeviceList();
        if (listErr == MOZA_ERR_NO_ERROR && current.size() > devices.size())
        {
            devices    = std::move(current);
            lastGrowth = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::puts("Lighting each device reports:");
    for (const auto& info : devices)
    {
        auto [openErr, device] = moza::Device::open(info.id);
        if (openErr != MOZA_ERR_NO_ERROR)
        {
            std::printf("  %s: could not be opened (%s)\n", info.name, bridge::errnoName(openErr));
            continue;
        }

        auto [parameterErr, parameters] = device.getParameterList();
        auto [commandErr, commands]     = device.getCommandList();
        int  shown                      = 0;
        std::printf("  %s (%zu parameters, %zu commands in all)\n", info.name, parameterErr == MOZA_ERR_NO_ERROR ? parameters.size() : 0,
                    commandErr == MOZA_ERR_NO_ERROR ? commands.size() : 0);
        for (const auto& parameter : parameters)
        {
            if (!isLightingName(parameter.name))
            {
                continue;
            }
            std::string value = "write-only";
            if (parameter.readable)
            {
                auto [valueErr, current] = device.parameter(parameter.id).getValueSync(300);
                value                    = valueErr == MOZA_ERR_NO_ERROR ? bridge::formatValue(current) : bridge::errnoName(valueErr);
            }
            std::printf("    parameter %-5d %-46s %s%s = %s\n", static_cast<int>(parameter.id), parameter.name, parameter.readable ? "r" : "-",
                        parameter.writable ? "w" : "-", value.c_str());
            ++shown;
        }
        for (const auto& command : commands)
        {
            if (isLightingName(command.name))
            {
                std::printf("    command   %-5d %s\n", static_cast<int>(command.id), command.name);
                ++shown;
            }
        }
        if (shown == 0)
        {
            std::puts("    none");
        }
    }
}

void printForceSettings(const moza::as::Wheelbase& base)
{
    using Base = moza::as::Wheelbase;

    struct Setting
    {
        const char*       name;
        moza::ParameterId id;
        const char*       meaning;
    };
    const Setting settings[] = {
        {"Main_CtrlFfbEnable", Base::kCtrlFfbEnableParameterId, "0 = disabled, 1 = enabled"},
        {"Main_CtrlModeSet", Base::kCtrlModeSetParameterId, "0 = telemetry FFB, 1 = DirectInput FFB, 2 = both"},
        {"Main_SteerForceMode", Base::kSteerForceModeParameterId, "0 = force feedback, 1 = force sensing"},
        {"Main_LimitToqueEnable", Base::kLimitToqueEnableParameterId, "1 = base force output disabled"},
        {"Main_Steer_TorqueMax", Base::kSteerTorqueMaxParameterId, "torque limit, %"},
        {"Main_Steer_GameForceFeedbackCoefficient", Base::kSteerGameForceFeedbackCoefficientParameterId, "game FFB strength, %"},
        {"Main_Steer_SpringCoefficient", Base::kSteerSpringCoefficientParameterId, "spring effect scale, %"},
        {"Main_Steer_DampCoefficient", Base::kSteerDampCoefficientParameterId, "damper effect scale, %"},
    };

    std::puts("Force feedback settings on the base:");
    for (const Setting& setting : settings)
    {
        auto [err, value] = base.parameter(setting.id).getValueSync(300);
        const std::string text = err == MOZA_ERR_NO_ERROR ? bridge::formatValue(value) : bridge::errnoName(err);
        std::printf("  %-40s %-12s (%s)\n", setting.name, text.c_str(), setting.meaning);
    }
}

std::string positionText(const moza::HidDevice& hid)
{
    if (hid)
    {
        auto [err, state] = hid.getInputState();
        if (err == MOZA_ERR_NO_ERROR)
        {
            char text[80];
            std::snprintf(text, sizeof(text), "X %.3f (%d)  Y %.3f (%d)", state.axes[MOZA_HA_X].normalizedValue,
                          static_cast<int>(state.axes[MOZA_HA_X].rawValue), state.axes[MOZA_HA_Y].normalizedValue,
                          static_cast<int>(state.axes[MOZA_HA_Y].rawValue));
            return text;
        }
    }
    return "n/a";
}

/**
 * Opens a HID interface for reading the yoke position: the first one that reports axes, or
 * failing that the first one, since the AY210 lists its interface with no axes until it is opened.
 */
moza::HidDevice openAxisHid(const moza::as::Wheelbase& base)
{
    auto [err, hids] = base.getHidList();
    if (err != MOZA_ERR_NO_ERROR)
    {
        std::fprintf(stderr, "getHidList failed: %s\n", bridge::errnoName(err));
        return {};
    }

    std::puts("HID interfaces:");
    moza::HidDevice chosen;
    for (std::size_t i = 0; i < hids.size(); ++i)
    {
        const auto& hid = hids[i];
        std::printf("  hid %zu  %-32s axes %d  buttons %d\n", i, hid.productName, static_cast<int>(hid.axisCount),
                    static_cast<int>(hid.buttonCount));
        if (!chosen && hid.axisCount >= 2)
        {
            chosen = base.getHid(static_cast<std::uint8_t>(i));
        }
    }

    if (!chosen && !hids.empty())
    {
        chosen = base.getHid(0);
    }

    if (chosen)
    {
        const moza::Errno openErr = chosen.open();
        if (openErr != MOZA_ERR_NO_ERROR)
        {
            std::fprintf(stderr, "Could not open hid %d for reading axes: %s\n", static_cast<int>(chosen.index()),
                         bridge::errnoName(openErr));
            return {};
        }
        // The first snapshot after opening can predate any input report.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::printf("Yoke position, normalized (raw), hid %d: %s\n", static_cast<int>(chosen.index()), positionText(chosen).c_str());
    }
    return chosen;
}

std::string torqueText(const moza::RawParameter& parameter)
{
    // Main_Steer_TorqueOutX/Y: the SDK hands back N*m as a double (it divides the raw reading by 10000).
    auto [err, value] = parameter.fetchSync(100);
    if (err != MOZA_ERR_NO_ERROR)
    {
        return "n/a";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%+.4f Nm", value.isFloat() ? value.asDouble() : value.asInt() / 10000.0);
    return text;
}

void printStatus(double elapsed, const moza::as::Wheelbase& base, const moza::HidDevice& hid, int sendErrors,
                 const std::string& note)
{
    std::printf("[%6.1fs] axes %s | torque X %s  Y %s | send errors %d%s\n", elapsed, positionText(hid).c_str(),
                torqueText(base.steerTorqueOutX().raw()).c_str(), torqueText(base.steerTorqueOutY().raw()).c_str(),
                sendErrors + g_asyncErrors.load(), note.c_str());
    std::fflush(stdout);
}

bool isLinkMode(const Options& options)
{
    return !options.checkUpdate && !options.list && !options.hold && options.sweepAxis < 0 && options.forceAxis < 0 && options.sineAxis < 0 && options.strokeAxis < 0;
}

/** Folder holding this program, where its settings file lives by default. */
std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    wchar_t      buffer[MAX_PATH];
    const DWORD  length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    std::error_code error;
    const auto      current = std::filesystem::current_path(error);
    return error ? std::filesystem::path(".") : current;
}

/**
 * Name of a running flight simulator that drives the yoke through MOZA Cockpit, or empty.
 *
 * The bridge frees every effect on the base when it starts and stops them all when it exits,
 * which would cut the forces out from under a flight in progress.
 */
#ifdef _WIN32
std::string runningProcess(std::initializer_list<const wchar_t*> names);
#endif

std::string runningFlightSim()
{
#ifdef _WIN32
    return runningProcess({L"X-Plane.exe", L"FlightSimulator.exe", L"FlightSimulator2024.exe", L"DCS.exe", L"Il-2.exe"});
#else
    return {};
#endif
}

#ifdef _WIN32
/** The first of @p names that is a running program, or empty. */
std::string runningProcess(std::initializer_list<const wchar_t*> names)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    std::string    found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && found.empty(); more = Process32NextW(snapshot, &entry))
    {
        for (const wchar_t* name : names)
        {
            if (_wcsicmp(entry.szExeFile, name) == 0)
            {
                for (const wchar_t* c = name; *c; ++c)
                {
                    found += static_cast<char>(*c);
                }
            }
        }
    }
    CloseHandle(snapshot);
    return found;
}
#endif

int run(const Options& options)
{
    const auto found = bridge::findFlightBase(static_cast<int>(options.waitSeconds * 1000), options.device, &g_stop);
    printDevices();
    if (!found)
    {
        if (!g_stop)
        {
            problem("No MOZA flight base found. Check that it is powered, connected and visible in MOZA Cockpit.");
        }
        return 2;
    }

    MOZA_DEVICE_HANDLE handle  = MOZA_INVALID_DEVICE_HANDLE;
    const moza::Errno  openErr = Moza_DeviceOpen(found->id, &handle);
    if (openErr != MOZA_ERR_NO_ERROR)
    {
        problem("Could not open '%s': %s", found->name, bridge::errnoName(openErr));
        return 2;
    }
    moza::as::Wheelbase base(handle);
    g_deviceId = static_cast<int>(found->id);

    std::printf("Using flight base '%s' (id %u).\n", found->name, static_cast<unsigned>(found->id));
    printForceSettings(base);
    const moza::HidDevice   hid = openAxisHid(base);
    const ExitWatchdogGuard teardownGuard;

    std::printf("Motor torque: X %s  Y %s\n", torqueText(base.steerTorqueOutX().raw()).c_str(),
                torqueText(base.steerTorqueOutY().raw()).c_str());

    if (options.list)
    {
        printLighting();
        return 0;
    }

    auto [enableErr, ffbEnabled] = base.ctrlFfbEnable().getSync(300);
    if (enableErr == MOZA_ERR_NO_ERROR && ffbEnabled == 0)
    {
        problem("Force feedback is disabled on the base (Main_CtrlFfbEnable = 0). Enable it in MOZA Cockpit and retry.");
        return 3;
    }

    if (isLinkMode(options))
    {
        bridge::LinkOptions link;
        link.telemetryPath = options.telemetryPath.empty() ? bridge::defaultTelemetryPath() : std::filesystem::path(options.telemetryPath);
        link.settingsPath  = options.settingsPath.empty() ? executableDirectory() / "moza_fs_bridge.ini" : std::filesystem::path(options.settingsPath);
        link.uiPort        = options.uiPort;
        link.fullScale     = options.fullScale;
        link.seconds       = options.seconds;
        link.verbose       = options.verbose;
        link.versionText   = [] { return std::string(BRIDGE_VERSION); };
#ifdef BRIDGE_TRAY
        if (g_updater != nullptr)
        {
            link.onUpdateSettings = [](bool check, bool prereleases) {
                static bool told = false;
                g_checkUpdates   = check;
                g_prereleases    = prereleases;
                // The first telling is the settings as loaded; after that, the user has just changed
                // one, and should not have to wait a day to see what it does.
                if (told)
                {
                    g_updater->recheck();
                }
                told = true;
            };
            link.versionText     = [] {
                std::scoped_lock lock(g_updateMutex);
                return g_newerVersion.empty() ? std::string(BRIDGE_VERSION)
                                              : BRIDGE_VERSION " (" + g_newerVersion + " is out: right-click the icon by the clock)";
            };
        }
        if (g_tray != nullptr)
        {
            link.onStarted = [](int port) {
                if (g_updater != nullptr)
                {
                    // Only now, with the settings loaded, is it known whether the user wants checks at all.
                    g_updater->start();
                }
                if (port > 0)
                {
                    g_tray->setSettingsUrl("http://127.0.0.1:" + std::to_string(port) + "/");
                }
                if (g_restarts > 0)
                {
                    g_tray->notify("MOZA FS25 link restarted itself", "MOZA's SDK had stopped answering. Forces are back; the log has the details.");
                }
                else
                {
                    g_tray->notify("MOZA FS25 link is running", port > 0 ? "Click this icon to open the settings."
                                                                        : "The settings page could not start; see the log.");
                }
            };
            link.onStatus = [](const std::string& text) { g_tray->setStatus("MOZA FS25 link: " + text); };
        }
#endif

        bridge::StallGuard guard(kStallLimit, &onLinkStalled);
        g_linkStarted = Clock::now();
        const double freezeAfter = options.testStallAfter;
        link.onLoop              = [&guard, freezeAfter] {
            if (freezeAfter > 0.0 && Clock::now() - g_linkStarted > std::chrono::duration<double>(freezeAfter))
            {
                std::puts("--test-stall-after: the loop stops here and never comes back.");
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::hours(1));
                }
            }
            guard.beat();
        };

        const int result = bridge::runLink(base, hid, link, g_stop);

        // From here the exit watchdog below is what bounds a teardown that does not finish.
        guard.disarm();
        armExitWatchdog();
        shutdownStep("closing the HID interface");
        if (hid)
        {
            hid.close();
        }
        shutdownStep("closing the device");
        base.close();
        return result;
    }

    const bridge::Condition off{};
    const bridge::Condition spring = bridge::conditionFromPercent(options.springPercent, options.fullScale);
    const bridge::Condition damper = bridge::conditionFromPercent(options.damperPercent, options.fullScale);

    bridge::FfbSession      ffb(base, static_cast<std::uint16_t>(options.durationMs), options.verbose);
    const ExitWatchdogGuard releaseGuard;
    if (!ffb.begin())
    {
        return 4;
    }
    const std::uint8_t springEffect = ffb.addEffect(bridge::EffectType::Spring);
    const std::uint8_t damperEffect = ffb.addEffect(bridge::EffectType::Damper);
    if (springEffect == 0 || damperEffect == 0)
    {
        return 4;
    }

    const bool         stroking   = options.strokeAxis >= 0;
    const bridge::Axis strokeAxis = options.strokeAxis == static_cast<int>(bridge::Axis::Roll) ? bridge::Axis::Roll : bridge::Axis::Pitch;
    if (stroking && !hid)
    {
        std::fprintf(stderr, "--stroke needs the yoke position, and no HID interface could be opened.\n");
        return 4;
    }
    bridge::StrokeSettings strokeSettings;
    strokeSettings.force  = options.strokeForcePercent / 100.0 * options.fullScale;
    strokeSettings.cycles = options.strokeCycles;
    bridge::StrokeController stroke(strokeSettings);
    std::int16_t             strokeMagnitude = 0;
    double                   strokeLowest    = 1.0;
    double                   strokeHighest   = 0.0;

    const bool   pushing     = options.forceAxis >= 0 || stroking;
    std::uint8_t forceEffect = 0;
    if (pushing && (forceEffect = ffb.addEffect(bridge::EffectType::ConstantForce)) == 0)
    {
        return 4;
    }
    const bridge::ForceAxes forceAxes = bridge::forceAlong(
        stroking ? strokeAxis : options.forceAxis == static_cast<int>(bridge::Axis::Roll) ? bridge::Axis::Roll : bridge::Axis::Pitch);
    const double forceRaw = options.forceAmplitudePercent / 100.0 * options.fullScale;

    const bool         sweeping     = options.sweepAxis >= 0;
    const bridge::Axis sweepAxis    = sweeping ? static_cast<bridge::Axis>(options.sweepAxis) : bridge::Axis::Roll;
    const double       sweepRaw     = options.sweepAmplitudePercent / 100.0 * options.fullScale;
    const double       kTwoPi       = 6.283185307179586;
    double             elapsed      = 0.0;
    const auto         sweepCenter  = [&] {
        const double center = sweepRaw * std::sin(kTwoPi * elapsed / options.sweepPeriodSeconds);
        return static_cast<std::int16_t>(std::clamp(std::lround(center), -32767L, 32767L));
    };
    const auto forceMagnitude = [&] {
        if (stroking)
        {
            return strokeMagnitude;
        }
        const double magnitude = forceRaw * std::sin(kTwoPi * elapsed / options.forcePeriodSeconds);
        return static_cast<std::int16_t>(std::clamp(std::lround(magnitude), -32767L, 32767L));
    };
    const auto springFor = [&](bridge::Axis axis) {
        if (stroking && axis == strokeAxis)
        {
            return off; // a centering spring would fight the stroke; the damper stays on
        }
        bridge::Condition condition = (axis == bridge::Axis::Roll ? options.roll : options.pitch) ? spring : off;
        if (sweeping && axis == sweepAxis)
        {
            condition.center = sweepCenter();
        }
        return condition;
    };

    int        sendErrors = 0;
    const auto update     = [&](bool start) {
        bool ok = true;
        ok &= ffb.setCondition(springEffect, bridge::Axis::Roll, springFor(bridge::Axis::Roll));
        ok &= ffb.setCondition(springEffect, bridge::Axis::Pitch, springFor(bridge::Axis::Pitch));
        ok &= ffb.setCondition(damperEffect, bridge::Axis::Roll, options.roll ? damper : off);
        ok &= ffb.setCondition(damperEffect, bridge::Axis::Pitch, options.pitch ? damper : off);
        if (pushing)
        {
            ok &= ffb.setConstant(forceEffect, forceMagnitude(), forceAxes);
        }
        if (start)
        {
            ok &= ffb.start(springEffect);
            ok &= ffb.start(damperEffect);
            if (pushing)
            {
                ok &= ffb.start(forceEffect);
            }
        }
        sendErrors += ok ? 0 : 1;
        return ok;
    };

    if (!update(true))
    {
        return 4;
    }

    if (options.sineAxis >= 0)
    {
        // Started once with a long duration and left alone: re-sending Start would restart the waveform.
        const bridge::Axis  axis = options.sineAxis == static_cast<int>(bridge::Axis::Roll) ? bridge::Axis::Roll : bridge::Axis::Pitch;
        const std::uint8_t  sine = ffb.addEffect(bridge::EffectType::Sine);
        const std::int16_t  magnitude =
            static_cast<std::int16_t>(std::clamp(std::lround(options.sineAmplitudePercent / 100.0 * options.fullScale), 0L, 32767L));
        if (sine == 0 || !ffb.setPeriodic(sine, magnitude, static_cast<std::uint32_t>(options.sinePeriodMs), bridge::forceAlong(axis), 60000) ||
            !ffb.start(sine))
        {
            return 4;
        }
        std::printf("Sine on %s: magnitude %d (raw), period %d ms, for up to 60 s.\n",
                    axis == bridge::Axis::Roll ? "roll (X)" : "pitch (Y)", static_cast<int>(magnitude), options.sinePeriodMs);
    }

    std::printf("Spring %.0f%% (coefficient %d) + damper %.0f%% (coefficient %d), saturation %d, on %s.\n",
                options.springPercent, static_cast<int>(spring.coefficient), options.damperPercent,
                static_cast<int>(damper.coefficient), static_cast<int>(spring.saturation),
                options.roll && options.pitch ? "roll (X) and pitch (Y)" : options.roll ? "roll (X)" : "pitch (Y)");
    if (sweeping)
    {
        std::printf("Sweeping the %s spring center +/-%.0f (raw) every %.1f s. Keep clear of the yoke. Ctrl+C releases the forces.\n",
                    sweepAxis == bridge::Axis::Roll ? "roll (X)" : "pitch (Y)", sweepRaw, options.sweepPeriodSeconds);
    }
    if (stroking)
    {
        std::printf("Stroking %s end to end, %d time(s), at up to +/-%.0f (raw), then back to center. Keep clear of the yoke. "
                    "Ctrl+C releases the forces.\n",
                    strokeAxis == bridge::Axis::Roll ? "roll (X)" : "pitch (Y)", options.strokeCycles, strokeSettings.force);
    }
    else if (pushing)
    {
        std::printf("Pushing %s with a constant force of +/-%.0f (raw) every %.1f s. Keep clear of the yoke. Ctrl+C releases the forces.\n",
                    forceAxes.x ? "roll (X)" : "pitch (Y)", forceRaw, options.forcePeriodSeconds);
    }
    if (!sweeping && !pushing)
    {
        std::puts("Move the yoke off center: it should push back. Ctrl+C releases the forces.");
    }

    // A moving center is sent often enough to feel smooth; everything else only needs the refresh.
    const auto sweepInterval  = std::chrono::milliseconds(50);
    const auto statusInterval = std::chrono::milliseconds(sweeping || pushing || options.sineAxis >= 0 ? 250 : 1000);

    const auto start       = Clock::now();
    auto       nextRefresh = start + std::chrono::milliseconds(options.refreshMs);
    auto       nextSweep   = start + sweepInterval;
    auto       nextStatus  = start;
    // A stroke ends by itself; the cap only matters if position feedback goes wrong.
    const double timeLimit  = options.seconds > 0 ? options.seconds : stroking ? 120.0 : 0.0;
    auto         lastStroke = start;
    bool         timedOut   = false;
    while (!g_stop)
    {
        const auto now = Clock::now();
        elapsed        = std::chrono::duration<double>(now - start).count();
        if (timeLimit > 0 && elapsed >= timeLimit)
        {
            timedOut = stroking;
            break;
        }
        if (stroking)
        {
            if (stroke.finished())
            {
                break;
            }
            auto [positionErr, state] = hid.getInputState();
            if (positionErr != MOZA_ERR_NO_ERROR)
            {
                std::fprintf(stderr, "Lost the yoke position (%s); stopping the stroke.\n", bridge::errnoName(positionErr));
                break;
            }
            const double position = state.axes[strokeAxis == bridge::Axis::Roll ? MOZA_HA_X : MOZA_HA_Y].normalizedValue;
            strokeLowest          = std::min(strokeLowest, position);
            strokeHighest         = std::max(strokeHighest, position);
            strokeMagnitude       = stroke.update(position, std::chrono::duration<double>(now - lastStroke).count());
            lastStroke            = now;
        }
        if (now >= nextRefresh)
        {
            update(options.restart);
            nextRefresh = now + std::chrono::milliseconds(options.refreshMs);
            nextSweep   = now + sweepInterval;
        }
        else if ((sweeping || pushing) && now >= nextSweep)
        {
            bool ok = true;
            if (sweeping)
            {
                ok &= ffb.setCondition(springEffect, sweepAxis, springFor(sweepAxis));
                ok &= !options.restart || ffb.start(springEffect);
            }
            if (pushing)
            {
                ok &= ffb.setConstant(forceEffect, forceMagnitude(), forceAxes);
                ok &= !options.restart || ffb.start(forceEffect);
            }
            sendErrors += ok ? 0 : 1;
            nextSweep = now + sweepInterval;
        }
        if (now >= nextStatus)
        {
            std::string note;
            if (sweeping)
            {
                note += " | center " + std::to_string(sweepCenter());
            }
            if (pushing)
            {
                note += " | force " + std::to_string(forceMagnitude());
            }
            if (stroking)
            {
                note += std::string(" | ") + stroke.phaseName();
            }
            printStatus(elapsed, base, hid, sendErrors, note);
            nextStatus = now + statusInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    armExitWatchdog();
    shutdownStep("releasing forces");
    ffb.stop();
    if (stroking)
    {
        std::printf("Stroke %s: position ranged %.3f to %.3f; ends not reached before stalling: %d.\n",
                    stroke.finished() ? "complete" : timedOut ? "timed out" : "interrupted", strokeLowest, strokeHighest,
                    stroke.stalls());
    }
    std::printf("Forces released. Updates that failed to send: %d, commands the base rejected: %d.\n", sendErrors,
                g_asyncErrors.load());

    shutdownStep("closing the HID interface");
    if (hid)
    {
        hid.close();
    }
    shutdownStep("closing the device");
    base.close();
    return 0;
}

} // namespace

#ifdef BRIDGE_TRAY
const wchar_t* const kWindowTitle = L"MOZA FS25 link";

std::wstring widen(const std::string& text)
{
    const int    length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(std::wcslen(wide.c_str()));
    return wide;
}

/**
 * With no console, everything printed goes to a log file beside the program, started afresh each
 * run; except with @p keep, after the program restarted itself, when what led up to that stays.
 */
std::filesystem::path redirectOutputToLog(bool keep)
{
    const std::filesystem::path path = executableDirectory() / "moza_fs_bridge.log";
    if (!keep)
    {
        if (FILE* fresh = _wfopen(path.c_str(), L"w"))
        {
            std::fclose(fresh);
        }
    }
    // Both in append mode on the one file, so neither stream writes over the other.
    _wfreopen(path.c_str(), L"a", stdout);
    _wfreopen(path.c_str(), L"a", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return path;
}
#endif

#ifdef BRIDGE_TRAY
namespace {

/** What this program was started with, less what it adds when restarting itself. */
std::vector<std::wstring> g_ownArguments;

void rememberOwnArguments()
{
    int             count = 0;
    wchar_t** const all   = CommandLineToArgvW(GetCommandLineW(), &count);
    if (all == nullptr)
    {
        return;
    }
    for (int i = 1; i < count; ++i)
    {
        const std::wstring argument = all[i];
        // Its own two, and the test option that froze it: a copy started again must not freeze by design.
        if (argument == L"--restarted" || argument == L"--after-pid" || argument == L"--test-stall-after")
        {
            ++i;
            continue;
        }
        g_ownArguments.push_back(argument);
    }
    LocalFree(all);
}

/** @p argument as it has to be written on a command line to arrive unchanged. */
std::wstring quoted(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return argument;
    }
    std::wstring out = L"\"";
    std::size_t  backslashes = 0;
    for (const wchar_t c : argument)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        // Backslashes only need doubling where a quote follows them.
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

/** Starts this program again, to take over once this stuck copy has ended. Gives up after a few quick repeats. */
void restartAfterStall()
{
    const int next = Clock::now() - g_linkStarted < kSettledRun ? g_restarts + 1 : 1;
    if (next > kMostRestarts)
    {
        std::fprintf(stderr, "That is %d times in quick succession, so it is not being started again.\n", next);
        std::fflush(nullptr);
        MessageBoxW(nullptr,
                    L"The link keeps getting stuck inside MOZA's SDK, so it has stopped.\n\nRestarting MOZA Cockpit usually clears this. "
                    L"If \"Lights follow the engine\" is switched on, try with it off.",
                    kWindowTitle, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }

    wchar_t     self[1024];
    const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    if (length == 0 || length >= ARRAYSIZE(self))
    {
        std::fprintf(stderr, "It could not be started again: its own path is not known.\n");
        return;
    }
    std::wstring command = quoted(self);
    for (const std::wstring& argument : g_ownArguments)
    {
        command += L" " + quoted(argument);
    }
    command += L" --restarted " + std::to_wstring(next) + L" --after-pid " + std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, executableDirectory().c_str(), &startup, &process))
    {
        std::fprintf(stderr, "It could not be started again (error %lu).\n", GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::fprintf(stderr, "Started it again (restart %d of at most %d in a row).\n", next, kMostRestarts);
}

} // namespace

/** A newer release was found: into the menu, onto the settings page, and a bubble to say so. */
void offerUpdate(const bridge::ReleaseInfo& release, bool installable)
{
    const std::string version = release.version.text();
    // A "-preN" version says what it is; one that is only marked on GitHub needs it said.
    const std::string name    = (release.prerelease && release.version.pre == 0 ? "pre-release " : "version ") + version;
    {
        std::scoped_lock lock(g_updateMutex);
        g_newerVersion = name;
    }
    g_updateInstallable = installable;
    withTray([&](bridge::Tray& tray) {
        tray.setUpdate(name, installable);
        tray.notify((release.prerelease ? "Pre-release " : "Version ") + version + " is available",
                    installable ? "Right-click this icon and pick \"Install " + name + "\" when you are not driving."
                                : "Right-click this icon for the download page.");
    });
}

/** What was on offer no longer is: pre-releases were switched off, or the checks were. */
void withdrawUpdate()
{
    {
        std::scoped_lock lock(g_updateMutex);
        g_newerVersion.clear();
    }
    g_updateInstallable = false;
    withTray([](bridge::Tray& tray) { tray.setUpdate({}, false); });
}

/**
 * Keeps the mod in Farming Simulator's mods folder the same as the one shipped beside this
 * program, which an update of the program cannot do while the game has the file open.
 */
class ModKeeper
{
public:
    ModKeeper() : m_thread([this] { run(); }) {}
    ~ModKeeper()
    {
        m_stop = true;
        m_thread.join();
    }

private:
    void run() const
    {
        const std::filesystem::path shipped = executableDirectory() / "FS25_MozaYokeLink.zip";
        bridge::ModSync             last    = bridge::ModSync::UpToDate;
        for (int attempt = 0; !m_stop && attempt < 2000; ++attempt)
        {
            const bool game = !runningProcess({L"FarmingSimulator2025Game.exe", L"FarmingSimulator2025.exe"}).empty();
            const bridge::ModSync result = bridge::syncMod(shipped, bridge::defaultGameFolder(), game);
            if (attempt == 0 || result != last)
            {
                std::printf("Game mod: %s.\n", bridge::describe(result));
                if (result == bridge::ModSync::Updated)
                {
                    withTray([](bridge::Tray& tray) {
                        tray.notify("Farming Simulator mod updated", "The mod in the game's mods folder now matches this version of the link.");
                    });
                }
                else if (result == bridge::ModSync::GameRunning)
                {
                    withTray([](bridge::Tray& tray) {
                        tray.notify("The game's mod is from another version", "It carries on working and is replaced once Farming Simulator has closed.");
                    });
                }
            }
            last = result;
            // A failure is most likely the game still letting go of the file; anything else is final.
            if (result != bridge::ModSync::GameRunning && !(result == bridge::ModSync::Failed && attempt < 8))
            {
                return;
            }
            for (int i = 0; i < 150 && !m_stop; ++i)
            {
                Sleep(100);
            }
        }
    }

    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
};
#endif

/** Last stop before the process ends: in the windowless build, a failure is shown instead of printed. */
int finish(int result)
{
#ifdef BRIDGE_TRAY
    if (result != 0 || g_deviceLost)
    {
        const std::string text = g_deviceLost         ? "The yoke base or the MOZA device service went away, so the link has stopped."
                                 : !g_problem.empty() ? g_problem
                                                      : "The link stopped because of a problem. See moza_fs_bridge.log beside the program.";
        MessageBoxW(nullptr, widen(text).c_str(), kWindowTitle, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
#endif
    return result;
}

int main(int argc, char** argv)
{
    // Unbuffered, so output still arrives in order and in full when piped (e.g. over SSH).
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options options = parseOptions(argc, argv);
    g_verbose       = options.verbose;
    installStopHandler();

#ifdef BRIDGE_TRAY
    if (options.quit)
    {
        // Ask the instance that is running to release the yoke and exit.
        if (const HANDLE quitEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, bridge::Tray::quitEventName()))
        {
            SetEvent(quitEvent);
            CloseHandle(quitEvent);
        }
        return 0;
    }
    if (!isLinkMode(options))
    {
        MessageBoxW(nullptr, L"This is the windowless program for everyday use. The test options need moza_fs_bridge.exe, run from a terminal.",
                    kWindowTitle, MB_OK | MB_ICONINFORMATION);
        return 64;
    }
    rememberOwnArguments();
    g_restarts = options.restarted;
    if (options.afterPid != 0)
    {
        // Started by a copy of this program that had got stuck and is ending itself: let it go first.
        if (const HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(options.afterPid)))
        {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }
    // One at a time: a second copy would fight the first over the yoke. Starting it again just opens the settings.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\MozaFsLinkInstance");
    for (int attempt = 0; GetLastError() == ERROR_ALREADY_EXISTS && options.restarted > 0 && attempt < 20; ++attempt)
    {
        // The stuck copy is still on its way out.
        if (instance != nullptr)
        {
            CloseHandle(instance);
        }
        Sleep(250);
        instance = CreateMutexW(nullptr, FALSE, L"Local\\MozaFsLinkInstance");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        bridge::Tray::openInBrowser("http://127.0.0.1:" + std::to_string(options.uiPort) + "/");
        return 0;
    }
    if (!options.waitGiven)
    {
        // Started at login, this may be up before MOZA Cockpit has found the base.
        options.waitSeconds = 60.0;
    }
    // Only now, as the one running instance: a second start or --quit must not wipe the first one's log.
    const std::filesystem::path logPath = redirectOutputToLog(options.restarted > 0);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (options.restarted > 0)
    {
        std::printf("\n---- started again by itself after getting stuck (restart %d in a row) ----\n", options.restarted);
    }

    // Made before the icon and ended after it, so that the menu's entries always have something to call.
    std::unique_ptr<bridge::Updater> updater;
    if (const auto current = bridge::parseVersion(BRIDGE_VERSION))
    {
        bridge::Updater::Config updates;
        updates.repository       = kRepository;
        updates.current          = *current;
        updates.installDirectory = executableDirectory();
        updates.enabled            = [] { return g_checkUpdates.load(); };
        updates.includePrereleases = [] { return g_prereleases.load(); };
        updates.onAvailable        = &offerUpdate;
        updates.onWithdrawn        = &withdrawUpdate;
        updates.onMessage        = [](const std::string& title, const std::string& text) {
            withTray([&](bridge::Tray& tray) { tray.notify(title, text); });
        };
        updater   = std::make_unique<bridge::Updater>(std::move(updates));
        g_updater = updater.get();
    }

    bridge::Tray::Config trayConfig;
    if (updater)
    {
        trayConfig.checkForUpdates = [] { g_updater->checkNow(); };
        trayConfig.installUpdate   = [] { g_updateInstallable ? g_updater->installNow() : g_updater->openReleasePage(); };
        trayConfig.showUpdateNotes = [] { g_updater->openReleasePage(); };
    }
    trayConfig.logPath        = logPath.wstring();
    trayConfig.requestExit    = [] { g_stop = true; };
    trayConfig.waitForCleanup = [] {
        for (int i = 0; i < 400 && !g_cleanedUp; ++i)
        {
            Sleep(10);
        }
    };
    auto tray = std::make_unique<bridge::Tray>(std::move(trayConfig));
    g_tray    = tray.get();
    tray->setStatus("MOZA FS25 link: looking for the yoke");
#else
    if (options.quit)
    {
        std::puts("--quit is for the windowless program, moza_fs_link.exe; this one stops with Ctrl+C.");
        return 0;
    }
    if (options.checkUpdate)
    {
#ifdef _WIN32
        // Nothing here touches the yoke or the MOZA SDK.
        return bridge::printUpdateCheck(kRepository, bridge::parseVersion(BRIDGE_VERSION).value_or(bridge::Version{}), options.fetchUpdate,
                                        options.prereleases);
#else
        std::puts("Updates are only looked for on Windows.");
        return 64;
#endif
    }
#endif

    if (options.durationMs != 0 && options.refreshMs >= options.durationMs)
    {
        std::fprintf(stderr, "Warning: --refresh-ms (%d) is not shorter than --duration-ms (%d); forces will drop out between updates.\n",
                     options.refreshMs, options.durationMs);
    }

    // Checked before anything is sent to the base. --list only reads, so it is always allowed.
    if (!options.list && !options.allowFlightSim)
    {
        const std::string flightSim = runningFlightSim();
        if (!flightSim.empty())
        {
            problem("%s is running. The bridge resets the yoke's force effects when it starts, which would cut out that "
                    "simulator's forces, so it is not touching the yoke. Close the simulator first, or pass --allow-flight-sim.",
                    flightSim.c_str());
            g_cleanedUp = true;
            return finish(5);
        }
    }

    Moza_SetLogCallback(onLog, nullptr);

#ifdef BRIDGE_TRAY
    const char* const program = "moza_fs_link";
#else
    const char* const program = "moza_fs_bridge";
#endif
    std::printf("%s %s, MOZA SDK %s (built against %s)\n", program, BRIDGE_VERSION, moza::getSdkVersionString(),
                MOZA_SDK_VERSION_STRING);

    const moza::Errno initErr = moza::initialize();
    if (initErr != MOZA_ERR_NO_ERROR)
    {
        const bool service = initErr == MOZA_ERR_SERVICE_NOT_FOUND || initErr == MOZA_ERR_SERVICE_VERSION_MISMATCH;
        problem("MOZA SDK initialization failed: %s.%s", bridge::errnoName(initErr),
                service ? " The SDK talks to the background device service installed with MOZA Cockpit / Pit House; make sure it is "
                          "installed, running and up to date."
                        : "");
        g_cleanedUp = true;
        return finish(1);
    }
    Moza_SetEventCallback(onEvent, nullptr);

#ifdef BRIDGE_TRAY
    auto modKeeper = std::make_unique<ModKeeper>();
#endif

    const int result = run(options);

#ifdef BRIDGE_TRAY
    modKeeper.reset();
    if (updater)
    {
        updater->stop();
    }
    // The icon goes as soon as the yoke is released, not after the SDK's slow teardown.
    {
        std::scoped_lock lock(g_trayMutex);
        g_tray = nullptr;
    }
    tray.reset();
#endif

    // Forces are released and the device is closed by now, so a stall past this point is the SDK's
    // own teardown and does not change the outcome of the run.
    g_exitCode = result;
    armExitWatchdog();
    shutdownStep("shutting down the SDK");
    moza::shutdown();
    shutdownStep("exiting");
    g_cleanedUp = true;
    return finish(result);
}
