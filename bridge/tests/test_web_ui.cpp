// Checks the settings registry, the status board, and the web UI's HTTP server over a real
// loopback socket on a port the system picks.

#include "settings.hpp"
#include "status_board.hpp"
#include "web_ui.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

int g_failures = 0;

#define CHECK(condition)                                                           \
    do                                                                             \
    {                                                                              \
        if (!(condition))                                                          \
        {                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (false)

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/** A scratch directory that is removed again when the test ends. */
struct TempDir
{
    TempDir()
    {
        std::random_device random;
        path = fs::temp_directory_path() / ("moza_web_ui_test_" + std::to_string(random()) + std::to_string(random()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    std::vector<std::string> files() const
    {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(path))
        {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    fs::path path;
};

std::string readFile(const fs::path& path)
{
    std::ifstream     in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeFile(const fs::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/** The settings most tests use: two numbers and a toggle in two groups. */
void registerSample(bridge::Settings& settings)
{
    CHECK(settings.addNumber("roll.spring", "Steering", "Centering", "How hard the yoke centers.", "%", 0, 100, 1, 30));
    CHECK(settings.addNumber("roll.damper", "Steering", "Weight", "Resistance to fast movement.", "%", 0, 100, 0.5, 20));
    CHECK(settings.addToggle("rumble.on", "Surface", "Rumble", "Shake on rough ground.", true));
}

// ---- Settings ----

void testRegistration()
{
    bridge::Settings settings;
    registerSample(settings);

    CHECK(!settings.addNumber("roll.spring", "Steering", "Again", "", "", 0, 1, 1, 0));
    CHECK(!settings.addToggle("roll.spring", "Steering", "Again", "", false));
    CHECK(!settings.addNumber("", "G", "No key", "", "", 0, 1, 1, 0));
    CHECK(!settings.addNumber("has space", "G", "Bad key", "", "", 0, 1, 1, 0));
    CHECK(!settings.addNumber("a=b", "G", "Bad key", "", "", 0, 1, 1, 0));
    CHECK(!settings.addNumber("line\nbreak", "G", "Bad key", "", "", 0, 1, 1, 0));
    CHECK(!settings.addNumber("inverted", "G", "Bad range", "", "", 5, 1, 1, 2));
    CHECK(!settings.addNumber("nostep", "G", "Bad step", "", "", 0, 1, 0, 0));
    CHECK(!settings.addNumber("nan", "G", "Bad range", "", "", 0, std::nan(""), 1, 0));
    CHECK(settings.addNumber("clamped.default", "Other", "Default out of range", "", "", 0, 10, 1, 50));
    CHECK(settings.get("clamped.default") == 10);

    CHECK(settings.contains("roll.spring"));
    CHECK(!settings.contains("roll"));
    CHECK(!settings.contains("inverted"));

    // Registration order is display order.
    const auto states = settings.snapshot();
    CHECK(states.size() == 4);
    CHECK(states[0].info.key == "roll.spring");
    CHECK(states[1].info.key == "roll.damper");
    CHECK(states[2].info.key == "rumble.on");
    CHECK(states[2].info.kind == bridge::SettingInfo::Kind::Toggle);
    CHECK(states[0].info.group == "Steering");
    CHECK(states[0].info.unit == "%");
    CHECK(states[0].info.minimum == 0 && states[0].info.maximum == 100 && states[0].info.step == 1);
    CHECK(states[0].info.defaultValue == 30 && states[0].value == 30 && states[0].saved == 30);
    CHECK(states[1].info.step == 0.5);
}

void testGetSetAndClamping()
{
    bridge::Settings settings;
    registerSample(settings);

    CHECK(settings.get("roll.spring") == 30);
    CHECK(settings.set("roll.spring", 42.5));
    CHECK(settings.get("roll.spring") == 42.5);
    CHECK(settings.set("roll.spring", 250));
    CHECK(settings.get("roll.spring") == 100);
    CHECK(settings.set("roll.spring", -1e9));
    CHECK(settings.get("roll.spring") == 0);

    CHECK(!settings.set("no.such.key", 1));
    CHECK(settings.get("no.such.key") == 0);

    CHECK(settings.set("roll.spring", 55));
    CHECK(!settings.set("roll.spring", std::nan("")));
    CHECK(!settings.set("roll.spring", std::numeric_limits<double>::infinity()));
    CHECK(!settings.set("roll.spring", -std::numeric_limits<double>::infinity()));
    CHECK(settings.get("roll.spring") == 55);

    CHECK(settings.enabled("rumble.on"));
    CHECK(settings.set("rumble.on", 0));
    CHECK(!settings.enabled("rumble.on"));
    CHECK(settings.set("rumble.on", 7));
    CHECK(settings.get("rumble.on") == 1);
    CHECK(settings.set("rumble.on", -0.25));
    CHECK(settings.get("rumble.on") == 1);

    CHECK(settings.parse("roll.spring", " 12.5 ") == 12.5);
    CHECK(settings.parse("roll.spring", "999") == 999.0); // not clamped until set
    CHECK(!settings.parse("roll.spring", "true"));
    CHECK(!settings.parse("roll.spring", "12 volts"));
    CHECK(!settings.parse("no.such.key", "1"));
    CHECK(settings.parse("rumble.on", "On") == 1.0);
    CHECK(settings.parse("rumble.on", "false") == 0.0);
    CHECK(settings.parse("rumble.on", "0") == 0.0);
    CHECK(!settings.parse("rumble.on", "maybe"));
    CHECK(!settings.parse("rumble.on", "0.5"));

    settings.resetToDefaults();
    CHECK(settings.get("roll.spring") == 30);
    CHECK(settings.get("roll.damper") == 20);
    CHECK(settings.enabled("rumble.on"));
}

void testChangeCounter()
{
    bridge::Settings settings;
    registerSample(settings);

    const auto start = settings.revision();
    CHECK(settings.set("roll.spring", 31));
    CHECK(settings.revision() == start + 1);
    // Only real changes count: the same value again, a rejected value and an unknown key do not.
    CHECK(settings.set("roll.spring", 31));
    CHECK(!settings.set("roll.spring", std::nan("")));
    CHECK(!settings.set("no.such.key", 1));
    CHECK(settings.revision() == start + 1);
    CHECK(settings.set("roll.spring", 500));
    CHECK(settings.revision() == start + 2);
    CHECK(settings.set("roll.spring", 900)); // clamps to the 100 it already is
    CHECK(settings.revision() == start + 2);

    settings.resetToDefaults();
    CHECK(settings.revision() == start + 3);
    settings.resetToDefaults();
    CHECK(settings.revision() == start + 3);

    // A load that changes something counts too, and the counter never goes back.
    TempDir dir;
    writeFile(dir.path / "s.ini", "roll.spring = 77\n");
    CHECK(settings.load(dir.path / "s.ini").ok);
    CHECK(settings.revision() == start + 4);
    CHECK(settings.load(dir.path / "s.ini").ok);
    CHECK(settings.revision() == start + 4);
}

void testDirtyFlag()
{
    TempDir          dir;
    const fs::path   file = dir.path / "settings.ini";
    bridge::Settings settings;
    registerSample(settings);

    CHECK(!settings.dirty());
    CHECK(settings.set("roll.spring", 40));
    CHECK(settings.dirty());
    CHECK(settings.set("roll.spring", 30));
    CHECK(!settings.dirty());

    CHECK(settings.set("roll.spring", 40));
    CHECK(settings.save(file));
    CHECK(!settings.dirty());
    CHECK(settings.snapshot()[0].saved == 40);

    CHECK(settings.set("rumble.on", 0));
    CHECK(settings.dirty());
    CHECK(settings.load(file).ok);
    CHECK(!settings.dirty());
    CHECK(settings.enabled("rumble.on"));
    CHECK(settings.get("roll.spring") == 40);

    // Defaults differ from what is on disk now, so going back to them is an unsaved change.
    settings.resetToDefaults();
    CHECK(settings.dirty());
    CHECK(settings.snapshot()[0].saved == 40);
}

void testSaveLoadRoundTrip()
{
    TempDir        dir;
    const fs::path file = dir.path / "settings.ini";

    bridge::Settings written;
    registerSample(written);
    CHECK(written.addNumber("tiny", "Other", "Tiny", "A very small number.", "s", 0, 1, 1e-9, 0));
    CHECK(written.addNumber("big", "Other", "Big", "A very large number.", "", 0, 1e30, 1, 0));
    CHECK(written.set("roll.spring", 100.0 / 3.0));
    CHECK(written.set("roll.damper", 0.1 + 0.2));
    CHECK(written.set("rumble.on", 0));
    CHECK(written.set("tiny", 1.5e-7));
    CHECK(written.set("big", 1.25e21));
    std::string error = "untouched";
    CHECK(written.save(file, &error));
    CHECK(error == "untouched");

    const std::string text = readFile(file);
    CHECK(contains(text, "roll.spring = 33.333333333333336"));
    CHECK(contains(text, "roll.damper = 0.30000000000000004"));
    CHECK(contains(text, "rumble.on = false"));
    // Hand-editable: each value sits under its label, description and range.
    CHECK(contains(text, "# ---- Steering ----"));
    CHECK(contains(text, "# ---- Surface ----"));
    CHECK(contains(text, "# Centering\n") || contains(text, "# Centering\r\n"));
    CHECK(contains(text, "# How hard the yoke centers."));
    CHECK(contains(text, "# 0 to 100 %, default 30"));
    CHECK(contains(text, "# true or false, default true"));
    CHECK(text.find("# 0 to 100 %, default 30") < text.find("roll.spring = "));

    bridge::Settings read;
    registerSample(read);
    CHECK(read.addNumber("tiny", "Other", "Tiny", "", "s", 0, 1, 1e-9, 0));
    CHECK(read.addNumber("big", "Other", "Big", "", "", 0, 1e30, 1, 0));
    const auto loaded = read.load(file);
    CHECK(loaded.ok);
    CHECK(loaded.fileFound);
    CHECK(loaded.warnings.empty());
    // Exactly the same doubles, not merely close.
    CHECK(read.get("roll.spring") == 100.0 / 3.0);
    CHECK(read.get("roll.damper") == 0.1 + 0.2);
    CHECK(!read.enabled("rumble.on"));
    CHECK(read.get("tiny") == 1.5e-7);
    CHECK(read.get("big") == 1.25e21);
    CHECK(!read.dirty());
}

void testExportAndImport()
{
    TempDir        dir;
    const fs::path file = dir.path / "settings.ini";

    // What is exported is what Save writes, at the current values whether saved or not.
    bridge::Settings source;
    registerSample(source);
    CHECK(source.set("roll.spring", 64));
    CHECK(source.set("rumble.on", 0));
    const std::string exported = source.exportText();
    CHECK(contains(exported, "roll.spring = 64") && contains(exported, "roll.damper = 20") && contains(exported, "rumble.on = false"));
    CHECK(source.dirty());
    CHECK(source.save(file));
    CHECK(readFile(file) == exported);

    // Importing that on another machine reproduces it, as unsaved changes.
    bridge::Settings target;
    registerSample(target);
    CHECK(target.set("roll.damper", 55));
    CHECK(target.save(file));
    const auto before = target.revision();
    auto       result = target.importText(exported);
    CHECK(result.applied == 3 && result.changed == 3 && result.warnings.empty());
    CHECK(target.get("roll.spring") == 64 && target.get("roll.damper") == 20 && !target.enabled("rumble.on"));
    CHECK(target.revision() == before + 1);
    CHECK(target.dirty());
    for (const auto& state : target.snapshot())
    {
        // Nothing was written: a restart, or Revert, still gives what was saved before.
        CHECK(state.saved == (state.info.key == "roll.damper" ? 55 : state.info.defaultValue));
    }
    CHECK(target.load(file).ok && target.get("roll.damper") == 55 && target.get("roll.spring") == 30);

    // The same again changes nothing and is not counted as a change.
    CHECK(target.importText(exported).changed == 3);
    const auto settledAt = target.revision();
    result                = target.importText(exported);
    CHECK(result.applied == 3 && result.changed == 0 && target.revision() == settledAt);

    // A setting the file does not mention is left as it is: a file from an older version, or part of one.
    CHECK(target.set("roll.spring", 11));
    result = target.importText("roll.damper = 70\n");
    CHECK(result.applied == 1 && result.changed == 1);
    CHECK(target.get("roll.spring") == 11 && target.get("roll.damper") == 70 && !target.enabled("rumble.on"));

    // As lenient as loading: Windows line endings, a byte order mark, comments, values out of range,
    // settings this version does not have, and lines that are not settings at all.
    result = target.importText("\xEF\xBB\xBF# from a friend\r\nroll.spring = 250   # too much\r\n; note\r\nfuture.setting = 3\r\n"
                               "roll.damper = heavy\r\nrumble.on = yes\r\nnot a setting\r\n");
    CHECK(result.applied == 2 && result.changed == 2);
    CHECK(target.get("roll.spring") == 100 && target.get("roll.damper") == 70 && target.enabled("rumble.on"));
    CHECK(result.warnings.size() == 4);
    CHECK(contains(result.warnings[0], "line 2") && contains(result.warnings[0], "using 100"));
    CHECK(contains(result.warnings[1], "line 4") && contains(result.warnings[1], "unknown setting"));
    CHECK(contains(result.warnings[2], "line 5") && contains(result.warnings[2], "roll.damper"));
    CHECK(contains(result.warnings[3], "line 7"));

    // Something that is not a settings file touches nothing.
    const auto untouched = target.revision();
    for (const std::string& other : {std::string(), std::string("\n\n# only a comment\n"), std::string("<html><body>hello</body></html>"),
                                     std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR=", 18), std::string("some.other.program = 1\nvalue = 2\n")})
    {
        result = target.importText(other);
        CHECK(result.applied == 0 && result.changed == 0);
    }
    CHECK(target.revision() == untouched && target.get("roll.spring") == 100);
}

void testTolerantLoad()
{
    TempDir        dir;
    const fs::path file = dir.path / "edited.ini";
    writeFile(file, "\xEF\xBB\xBF# edited in Notepad\r\n"
                    "\r\n"
                    "; another comment style\r\n"
                    "   roll.spring   =   45.5   \r\n"
                    "roll.damper = 250   # too much\r\n"
                    "rumble.on = Off\r\n"
                    "old.removed.setting = 3\r\n"
                    "this line is not a setting\r\n"
                    "roll.damper = heavy\r\n"
                    "= 5\r\n");

    bridge::Settings settings;
    registerSample(settings);
    const auto before = settings.revision();
    const auto loaded = settings.load(file);
    CHECK(loaded.ok);
    CHECK(loaded.fileFound);
    CHECK(settings.get("roll.spring") == 45.5);
    CHECK(settings.get("roll.damper") == 100); // clamped, and the unreadable later line did not undo it
    CHECK(!settings.enabled("rumble.on"));
    CHECK(!settings.dirty());
    CHECK(settings.revision() == before + 1);

    CHECK(loaded.warnings.size() == 5);
    if (loaded.warnings.size() == 5)
    {
        CHECK(contains(loaded.warnings[0], "line 5") && contains(loaded.warnings[0], "roll.damper") && contains(loaded.warnings[0], "100"));
        CHECK(contains(loaded.warnings[1], "line 7") && contains(loaded.warnings[1], "old.removed.setting"));
        CHECK(contains(loaded.warnings[2], "line 8"));
        CHECK(contains(loaded.warnings[3], "line 9") && contains(loaded.warnings[3], "heavy"));
        CHECK(contains(loaded.warnings[4], "line 10"));
    }

    // A key the file does not mention comes up at its default, as it would after a restart.
    writeFile(file, "roll.damper = 10\n");
    CHECK(settings.set("roll.spring", 99));
    const auto again = settings.load(file);
    CHECK(again.ok && again.warnings.empty());
    CHECK(settings.get("roll.spring") == 30);
    CHECK(settings.get("roll.damper") == 10);
    CHECK(settings.enabled("rumble.on"));
}

void testMissingAndUnreadableFile()
{
    TempDir          dir;
    bridge::Settings settings;
    registerSample(settings);

    CHECK(settings.set("roll.spring", 80));
    const auto missing = settings.load(dir.path / "never_written.ini");
    CHECK(missing.ok);
    CHECK(!missing.fileFound);
    CHECK(missing.warnings.empty());
    CHECK(settings.get("roll.spring") == 30);
    CHECK(!settings.dirty());

    // Something that is there but cannot be read as a file is an error, and changes nothing.
    CHECK(settings.set("roll.spring", 80));
    const auto unreadable = settings.load(dir.path);
    CHECK(!unreadable.ok);
    CHECK(unreadable.warnings.size() == 1);
    CHECK(settings.get("roll.spring") == 80);
    CHECK(settings.dirty());
}

void testAtomicSave()
{
    TempDir          dir;
    const fs::path   file = dir.path / "settings.ini";
    bridge::Settings settings;
    registerSample(settings);

    CHECK(settings.save(file));
    CHECK(dir.files() == std::vector<std::string>{"settings.ini"});

    // Saving over an existing file replaces it and still leaves nothing else behind.
    CHECK(settings.set("roll.spring", 64));
    CHECK(settings.save(file));
    CHECK(dir.files() == std::vector<std::string>{"settings.ini"});
    CHECK(contains(readFile(file), "roll.spring = 64"));

    // A leftover temporary file from a crashed save does not get in the way.
    writeFile(dir.path / "settings.ini.tmp", "half a fi");
    CHECK(settings.set("roll.spring", 65));
    CHECK(settings.save(file));
    CHECK(dir.files() == std::vector<std::string>{"settings.ini"});
    CHECK(contains(readFile(file), "roll.spring = 65"));

    // A save that cannot happen says why and leaves the unsaved state as it was.
    CHECK(settings.set("roll.spring", 66));
    std::string error;
    CHECK(!settings.save(dir.path / "no_such_directory" / "settings.ini", &error));
    CHECK(!error.empty());
    CHECK(settings.dirty());
    CHECK(!settings.save(dir.path / "no_such_directory" / "settings.ini"));
    CHECK(dir.files() == std::vector<std::string>{"settings.ini"});
    CHECK(contains(readFile(file), "roll.spring = 65"));
}

void testNumberText()
{
    CHECK(bridge::formatNumber(0) == "0");
    CHECK(bridge::formatNumber(-0.0) == "0");
    CHECK(bridge::formatNumber(60) == "60");
    CHECK(bridge::formatNumber(-12.5) == "-12.5");
    CHECK(bridge::formatNumber(0.1) == "0.1");
    CHECK(bridge::formatNumber(100000) == "100000");
    CHECK(bridge::formatNumber(0.0001) == "0.0001");
    CHECK(bridge::formatNumber(1.25e21) == "1.25e+21");

    CHECK(bridge::parseNumber("12") == 12.0);
    CHECK(bridge::parseNumber("  -0.75\t") == -0.75);
    CHECK(bridge::parseNumber("+3") == 3.0);
    CHECK(bridge::parseNumber("1e3") == 1000.0);
    CHECK(bridge::parseNumber(".5") == 0.5);
    CHECK(!bridge::parseNumber(""));
    CHECK(!bridge::parseNumber("   "));
    CHECK(!bridge::parseNumber("abc"));
    CHECK(!bridge::parseNumber("12abc"));
    CHECK(!bridge::parseNumber("1 2"));
    CHECK(!bridge::parseNumber("+-1"));
    CHECK(!bridge::parseNumber("0x10"));
    CHECK(!bridge::parseNumber("nan"));
    CHECK(!bridge::parseNumber("inf"));
    CHECK(!bridge::parseNumber("-infinity"));
    CHECK(!bridge::parseNumber("1e999"));

    for (const double value : {0.1 + 0.2, 100.0 / 3.0, 1e-7, 123456789.125, -4.9e-12, 6.02e23})
    {
        CHECK(bridge::parseNumber(bridge::formatNumber(value)) == value);
    }
}

void testLocaleIndependence()
{
    // A locale that writes one half as "0,5". printf follows it; the settings code must not.
    const char* selected = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE", "German_Germany.1252", "fr_FR.UTF-8", "fr_FR"})
    {
        if ((selected = std::setlocale(LC_ALL, name)) != nullptr)
        {
            break;
        }
    }
    char probe[32];
    std::snprintf(probe, sizeof probe, "%.1f", 0.5);
    if (!selected || std::string(probe) != "0,5")
    {
        std::setlocale(LC_ALL, "C");
        std::puts("note: no comma-decimal locale installed, locale check skipped");
        return;
    }

    CHECK(bridge::formatNumber(0.5) == "0.5");
    CHECK(bridge::formatNumber(1234.25) == "1234.25");
    CHECK(bridge::parseNumber("0.5") == 0.5);
    CHECK(!bridge::parseNumber("0,5"));

    TempDir        dir;
    const fs::path file = dir.path / "settings.ini";
    {
        bridge::Settings settings;
        registerSample(settings);
        CHECK(settings.set("roll.spring", 12.75));
        CHECK(settings.set("roll.damper", 0.5));
        CHECK(settings.save(file));
    }
    CHECK(contains(readFile(file), "roll.spring = 12.75"));
    CHECK(contains(readFile(file), "roll.damper = 0.5"));
    CHECK(!contains(readFile(file), "12,75"));
    {
        bridge::Settings settings;
        registerSample(settings);
        const auto loaded = settings.load(file);
        CHECK(loaded.ok && loaded.warnings.empty());
        CHECK(settings.get("roll.spring") == 12.75);
        CHECK(settings.get("roll.damper") == 0.5);
    }
    std::setlocale(LC_ALL, "C");
}

void testSettingsFromSeveralThreads()
{
    TempDir          dir;
    bridge::Settings settings;
    registerSample(settings);

    std::atomic<bool>        go{false};
    std::atomic<int>         outOfRange{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n)
    {
        threads.emplace_back([&, n] {
            while (!go.load())
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < 2000; ++i)
            {
                settings.set("roll.spring", (i * 7 + n) % 101);
                settings.set("rumble.on", i % 2);
                const double value = settings.get("roll.spring");
                if (value < 0 || value > 100)
                {
                    ++outOfRange;
                }
                if (i % 500 == 0)
                {
                    settings.save(dir.path / "settings.ini");
                    settings.snapshot();
                    settings.dirty();
                }
            }
        });
    }
    const auto before = settings.revision();
    go.store(true);
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK(outOfRange.load() == 0);
    CHECK(settings.revision() > before);
    CHECK(dir.files() == std::vector<std::string>{"settings.ini"});
    bridge::Settings read;
    registerSample(read);
    const auto loaded = read.load(dir.path / "settings.ini");
    CHECK(loaded.ok && loaded.warnings.empty());
}

// ---- Status board ----

void testStatusBoard()
{
    bridge::StatusBoard status;
    CHECK(status.add("telemetry", "Game", "Telemetry"));
    CHECK(status.addRanged("speed", "Game", "Speed", "km/h", 0, 60, 1));
    CHECK(status.addRanged("steering", "Game", "Steering", "", -1, 1));
    CHECK(!status.add("speed", "Game", "Speed again"));
    CHECK(!status.add("", "Game", "No key"));
    CHECK(!status.addRanged("flat", "Game", "No range", "", 1, 1));
    CHECK(!status.addRanged("backwards", "Game", "Inverted", "", 1, 0));

    auto snapshot = status.snapshot();
    CHECK(snapshot.ageSeconds < 0);
    CHECK(snapshot.items.size() == 3);
    CHECK(snapshot.items[0].key == "telemetry" && !snapshot.items[0].hasRange);
    CHECK(snapshot.items[1].key == "speed" && snapshot.items[1].hasRange);
    CHECK(snapshot.items[1].minimum == 0 && snapshot.items[1].maximum == 60 && snapshot.items[1].decimals == 1);
    CHECK(snapshot.items[1].unit == "km/h" && snapshot.items[1].group == "Game" && snapshot.items[1].label == "Speed");
    CHECK(snapshot.items[2].minimum == -1 && snapshot.items[2].decimals == 2);
    for (const auto& item : snapshot.items)
    {
        CHECK(item.kind == bridge::StatusItem::Kind::Empty);
    }

    CHECK(status.set("speed", 23.4));
    CHECK(status.setText("telemetry", "connected"));
    CHECK(!status.set("no.such.item", 1));
    CHECK(!status.setText("no.such.item", "x"));
    snapshot = status.snapshot();
    CHECK(snapshot.ageSeconds >= 0 && snapshot.ageSeconds < 5);
    CHECK(snapshot.items[0].kind == bridge::StatusItem::Kind::Text && snapshot.items[0].text == "connected");
    CHECK(snapshot.items[1].kind == bridge::StatusItem::Kind::Number && snapshot.items[1].number == 23.4);
    CHECK(snapshot.items[2].kind == bridge::StatusItem::Kind::Empty);

    // Either kind can replace the other.
    CHECK(status.setText("speed", "n/a"));
    CHECK(status.set("telemetry", 3));
    snapshot = status.snapshot();
    CHECK(snapshot.items[1].kind == bridge::StatusItem::Kind::Text && snapshot.items[1].text == "n/a");
    CHECK(snapshot.items[0].kind == bridge::StatusItem::Kind::Number && snapshot.items[0].number == 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(status.snapshot().ageSeconds >= 0.05);
    CHECK(status.set("speed", 1));
    CHECK(status.snapshot().ageSeconds < 0.05);

    CHECK(status.forcesEnabled());
    status.setForcesEnabled(false);
    CHECK(!status.forcesEnabled());
    status.setForcesEnabled(true);
    CHECK(status.forcesEnabled());

    CHECK(status.releaseRequests() == 0);
    status.requestRelease();
    status.requestRelease();
    CHECK(status.releaseRequests() == 2);
}

// ---- A minimal HTTP client ----

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void         closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void         closeSocket(Socket socket) { ::close(socket); }
#endif

Socket connectTo(std::uint16_t port)
{
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNoSocket)
    {
        return kNoSocket;
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
    {
        closeSocket(socket);
        return kNoSocket;
    }
    return socket;
}

/** Sends as much as the server will take; it may stop reading once it has decided to refuse. */
void sendAll(Socket socket, std::string_view data)
{
    while (!data.empty())
    {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const auto sent = ::send(socket, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 16384)), flags);
        if (sent <= 0)
        {
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

bool waitReadable(Socket socket, double seconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout;
    timeout.tv_sec  = static_cast<long>(seconds);
    timeout.tv_usec = static_cast<int>((seconds - std::floor(seconds)) * 1e6);
    return ::select(static_cast<int>(socket) + 1, &readable, nullptr, nullptr, &timeout) > 0;
}

/** Everything the server sends until it closes the connection, or what arrived within the time allowed. */
std::string readAll(Socket socket, double seconds, bool* closedByServer = nullptr)
{
    const auto  start = Clock::now();
    std::string received;
    char        buffer[4096];
    if (closedByServer)
    {
        *closedByServer = false;
    }
    while (secondsSince(start) < seconds)
    {
        if (!waitReadable(socket, std::max(0.01, seconds - secondsSince(start))))
        {
            continue;
        }
        const auto count = ::recv(socket, buffer, sizeof buffer, 0);
        if (count <= 0)
        {
            if (closedByServer)
            {
                *closedByServer = true;
            }
            break;
        }
        received.append(buffer, static_cast<std::size_t>(count));
    }
    return received;
}

struct Reply
{
    bool        answered = false;
    int         status   = 0;
    std::string head;
    std::string body;

    bool hasHeader(std::string_view line) const { return contains(head, std::string(line) + "\r\n"); }
};

Reply parseReply(const std::string& raw)
{
    Reply      reply;
    const auto split = raw.find("\r\n\r\n");
    if (raw.compare(0, 9, "HTTP/1.1 ") != 0 || split == std::string::npos)
    {
        return reply;
    }
    reply.answered = true;
    reply.status   = std::atoi(raw.c_str() + 9);
    reply.head     = raw.substr(0, split + 2);
    reply.body     = raw.substr(split + 4);
    return reply;
}

/** Sends raw bytes on a fresh connection and reads the whole answer. */
Reply sendRaw(std::uint16_t port, std::string_view raw)
{
    const Socket socket = connectTo(port);
    if (socket == kNoSocket)
    {
        return {};
    }
    sendAll(socket, raw);
    const Reply reply = parseReply(readAll(socket, 5.0));
    closeSocket(socket);
    return reply;
}

std::string hostLine(std::uint16_t port)
{
    return "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
}

Reply get(std::uint16_t port, const std::string& path)
{
    return sendRaw(port, "GET " + path + " HTTP/1.1\r\n" + hostLine(port) + "\r\n");
}

Reply post(std::uint16_t port, const std::string& path, const std::string& body, const std::string& extraHeaders = {})
{
    return sendRaw(port, "POST " + path + " HTTP/1.1\r\n" + hostLine(port) + extraHeaders + "Content-Type: text/plain\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body);
}

// ---- Just enough JSON to check that what the server writes is well formed ----

class JsonChecker
{
public:
    explicit JsonChecker(std::string_view text) : m_text(text) {}

    bool valid()
    {
        skipBlanks();
        if (!value())
        {
            return false;
        }
        skipBlanks();
        return m_at == m_text.size();
    }

private:
    char peek() const { return m_at < m_text.size() ? m_text[m_at] : '\0'; }
    bool take(char c)
    {
        if (peek() != c)
        {
            return false;
        }
        ++m_at;
        return true;
    }
    void skipBlanks()
    {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t')
        {
            ++m_at;
        }
    }
    bool literal(std::string_view word)
    {
        if (m_text.substr(m_at, word.size()) != word)
        {
            return false;
        }
        m_at += word.size();
        return true;
    }
    bool string()
    {
        if (!take('"'))
        {
            return false;
        }
        while (m_at < m_text.size())
        {
            const auto c = static_cast<unsigned char>(m_text[m_at++]);
            if (c == '"')
            {
                return true;
            }
            if (c < 0x20)
            {
                return false;
            }
            if (c == '\\')
            {
                const char escape = peek();
                ++m_at;
                if (escape == 'u')
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        if (!std::isxdigit(static_cast<unsigned char>(peek())))
                        {
                            return false;
                        }
                        ++m_at;
                    }
                }
                else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
                {
                    return false;
                }
            }
        }
        return false;
    }
    bool number()
    {
        const auto start = m_at;
        take('-');
        if (!std::isdigit(static_cast<unsigned char>(peek())))
        {
            return false;
        }
        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' || peek() == 'e' || peek() == 'E' || peek() == '+' ||
               peek() == '-')
        {
            ++m_at;
        }
        return bridge::parseNumber(m_text.substr(start, m_at - start)).has_value();
    }
    bool members(char close, bool keyed)
    {
        skipBlanks();
        if (take(close))
        {
            return true;
        }
        for (;;)
        {
            skipBlanks();
            if (keyed)
            {
                if (!string())
                {
                    return false;
                }
                skipBlanks();
                if (!take(':'))
                {
                    return false;
                }
                skipBlanks();
            }
            if (!value())
            {
                return false;
            }
            skipBlanks();
            if (take(close))
            {
                return true;
            }
            if (!take(','))
            {
                return false;
            }
        }
    }
    bool value()
    {
        switch (peek())
        {
        case '{': ++m_at; return members('}', true);
        case '[': ++m_at; return members(']', false);
        case '"': return string();
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return number();
        }
    }

    std::string_view m_text;
    std::size_t      m_at = 0;
};

bool isJson(std::string_view text)
{
    return JsonChecker(text).valid();
}

/** The raw text of @p field inside the object whose "key" is @p key. Relies on "key" coming first. */
std::string fieldOf(const std::string& json, const std::string& key, const std::string& field)
{
    const auto object = json.find("{\"key\":\"" + key + "\"");
    if (object == std::string::npos)
    {
        return "<no such key>";
    }
    const auto end   = json.find('}', object);
    const auto start = json.find("\"" + field + "\":", object);
    if (start == std::string::npos || start > end)
    {
        return "<no such field>";
    }
    const auto from = start + field.size() + 3;
    return json.substr(from, json.find_first_of(",}", from) - from);
}

// ---- Web UI ----

/** Settings, a status board, a scratch settings file and a server on a free port. */
struct Server
{
    Server() : ui(settings, status, dir.path / "settings.ini")
    {
        registerSample(settings);
        CHECK(status.add("telemetry", "Game", "Telemetry"));
        CHECK(status.addRanged("speed", "Game", "Speed", "km/h", 0, 60, 1));
        CHECK(status.addRanged("steering", "Yoke", "Steering", "", -1, 1));
        std::string error;
        CHECK(ui.start(0, &error));
        CHECK(error.empty());
        port = ui.port();
        CHECK(port != 0);
    }

    TempDir             dir;
    bridge::Settings    settings;
    bridge::StatusBoard status;
    bridge::WebUi       ui;
    std::uint16_t       port = 0;
};

void testPage()
{
    Server server;
    for (const char* path : {"/", "/index.html", "/?from=bookmark"})
    {
        const Reply reply = get(server.port, path);
        CHECK(reply.answered);
        CHECK(reply.status == 200);
        CHECK(reply.hasHeader("Content-Type: text/html; charset=utf-8"));
        CHECK(reply.hasHeader("Content-Length: " + std::to_string(reply.body.size())));
        CHECK(reply.hasHeader("Connection: close"));
        CHECK(reply.hasHeader("Cache-Control: no-store"));
        CHECK(contains(reply.head, "Content-Security-Policy: default-src 'none'"));
        CHECK(contains(reply.head, "frame-ancestors 'none'"));
        CHECK(!contains(reply.head, "Access-Control"));
        CHECK(reply.body == bridge::webUiPage());
    }

    const std::string& page = bridge::webUiPage();
    CHECK(page.compare(0, 15, "<!doctype html>") == 0);
    CHECK(contains(page, "</html>"));
    CHECK(contains(page, "prefers-color-scheme: dark"));
    CHECK(contains(page, "/api/state"));
    // Self-contained: nothing is fetched from anywhere but this server.
    for (const char* external : {"http://", "https://", "src=\"", "@import", "url("})
    {
        CHECK(!contains(page, external));
    }
    CHECK(std::all_of(page.begin(), page.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
}

void testState()
{
    Server server;
    // Text that needs every kind of JSON escaping.
    CHECK(server.settings.addNumber("odd", "Qu\"ote", "Back\\slash", "Line\nbreak\ttab \x01 bell", "N\xC2\xB7m", -1, 1, 0.25, 0.5));

    Reply reply = get(server.port, "/api/state");
    CHECK(reply.status == 200);
    CHECK(reply.hasHeader("Content-Type: application/json; charset=utf-8"));
    CHECK(reply.hasHeader("Content-Length: " + std::to_string(reply.body.size())));
    CHECK(!contains(reply.head, "Access-Control"));
    CHECK(isJson(reply.body));
    const std::string& json = reply.body;

    CHECK(contains(json, "\"dirty\":false"));
    CHECK(contains(json, "\"forcesEnabled\":true"));
    CHECK(contains(json, "\"releaseRequests\":0"));
    CHECK(contains(json, "\"settingsFile\":\""));
    CHECK(contains(json, "settings.ini\""));
    CHECK(!contains(json, "\"result\""));

    CHECK(fieldOf(json, "roll.spring", "group") == "\"Steering\"");
    CHECK(fieldOf(json, "roll.spring", "label") == "\"Centering\"");
    CHECK(fieldOf(json, "roll.spring", "description") == "\"How hard the yoke centers.\"");
    CHECK(fieldOf(json, "roll.spring", "unit") == "\"%\"");
    CHECK(fieldOf(json, "roll.spring", "type") == "\"number\"");
    CHECK(fieldOf(json, "roll.spring", "min") == "0");
    CHECK(fieldOf(json, "roll.spring", "max") == "100");
    CHECK(fieldOf(json, "roll.spring", "step") == "1");
    CHECK(fieldOf(json, "roll.spring", "default") == "30");
    CHECK(fieldOf(json, "roll.spring", "value") == "30");
    CHECK(fieldOf(json, "roll.spring", "saved") == "30");
    CHECK(fieldOf(json, "roll.damper", "step") == "0.5");
    CHECK(fieldOf(json, "rumble.on", "type") == "\"toggle\"");
    CHECK(fieldOf(json, "rumble.on", "value") == "1");
    // In registration order.
    CHECK(json.find("\"roll.spring\"") < json.find("\"roll.damper\""));
    CHECK(json.find("\"roll.damper\"") < json.find("\"rumble.on\""));

    CHECK(contains(json, "\"group\":\"Qu\\\"ote\""));
    CHECK(contains(json, "\"label\":\"Back\\\\slash\""));
    CHECK(contains(json, "\"description\":\"Line\\nbreak\\ttab \\u0001 bell\""));
    CHECK(contains(json, "\"unit\":\"N\xC2\xB7m\""));

    // Nothing published yet: no age, and items with neither a value nor text.
    CHECK(contains(json, "\"status\":{\"age\":null,"));
    CHECK(fieldOf(json, "speed", "min") == "0");
    CHECK(fieldOf(json, "speed", "max") == "60");
    CHECK(fieldOf(json, "speed", "decimals") == "1");
    CHECK(fieldOf(json, "speed", "value") == "<no such field>");
    CHECK(fieldOf(json, "telemetry", "min") == "<no such field>");
    CHECK(fieldOf(json, "telemetry", "text") == "<no such field>");

    server.status.set("speed", 23.5);
    server.status.set("steering", std::nan(""));
    server.status.setText("telemetry", "connected \"FS25\"");
    server.settings.set("roll.spring", 41);
    server.status.setForcesEnabled(false);
    reply = get(server.port, "/api/state");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(!contains(reply.body, "\"age\":null"));
    CHECK(fieldOf(reply.body, "speed", "value") == "23.5");
    CHECK(fieldOf(reply.body, "steering", "value") == "null");
    CHECK(fieldOf(reply.body, "telemetry", "text") == "\"connected \\\"FS25\\\"\"");
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "41");
    CHECK(fieldOf(reply.body, "roll.spring", "saved") == "30");
    CHECK(contains(reply.body, "\"dirty\":true"));
    CHECK(contains(reply.body, "\"forcesEnabled\":false"));
}

void testSetEndpoint()
{
    Server server;
    const auto before = server.settings.revision();

    Reply reply = post(server.port, "/api/set", "roll.spring=42");
    CHECK(reply.status == 200);
    CHECK(reply.hasHeader("Content-Type: application/json; charset=utf-8"));
    CHECK(isJson(reply.body));
    CHECK(server.settings.get("roll.spring") == 42);
    CHECK(server.settings.revision() == before + 1);
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "42");
    CHECK(contains(reply.body, "\"dirty\":true"));
    CHECK(contains(reply.body, "\"result\":{\"message\":\"1 setting applied\",\"warnings\":[]}"));
    CHECK(fieldOf(get(server.port, "/api/state").body, "roll.spring", "value") == "42");

    // Several at once, with the spacing and line endings a person might send, clamped to range.
    reply = post(server.port, "/api/set", "roll.spring = 500\r\n\r\n  roll.damper=12.5\r\nrumble.on = off\r\n");
    CHECK(reply.status == 200);
    CHECK(server.settings.get("roll.spring") == 100);
    CHECK(server.settings.get("roll.damper") == 12.5);
    CHECK(!server.settings.enabled("rumble.on"));
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "100");
    CHECK(fieldOf(reply.body, "rumble.on", "value") == "0");
    CHECK(contains(reply.body, "3 settings applied"));

    // One bad line refuses the whole request, so a half-applied set of values never goes live.
    const auto settled = server.settings.revision();
    for (const char* body : {"roll.spring=10\nno.such.key=1", "roll.spring=10\nroll.damper=heavy", "roll.spring=10\nroll.damper=nan",
                             "roll.spring=10\nroll.damper=inf", "roll.spring=10\nrumble.on=2", "roll.spring=10\njust words", "roll.spring=",
                             "", "\n\n"})
    {
        reply = post(server.port, "/api/set", body);
        CHECK(reply.status == 400);
        CHECK(reply.hasHeader("Content-Type: text/plain; charset=utf-8"));
        CHECK(contains(reply.body, "nothing changed"));
    }
    CHECK(server.settings.revision() == settled);
    CHECK(server.settings.get("roll.spring") == 100);
    // The reply never repeats what the client sent as a key, only where it was.
    reply = post(server.port, "/api/set", "<script>alert(1)</script>=1");
    CHECK(reply.status == 400);
    CHECK(!contains(reply.body, "script"));
    CHECK(contains(reply.body, "line 1"));
}

void testActions()
{
    Server         server;
    const fs::path file = server.dir.path / "settings.ini";

    // Save.
    CHECK(post(server.port, "/api/set", "roll.spring=55").status == 200);
    CHECK(server.settings.dirty());
    Reply reply = post(server.port, "/api/save", "");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(contains(reply.body, "\"dirty\":false"));
    CHECK(contains(reply.body, "\"message\":\"Saved to "));
    CHECK(fieldOf(reply.body, "roll.spring", "saved") == "55");
    CHECK(!server.settings.dirty());
    CHECK(contains(readFile(file), "roll.spring = 55"));
    CHECK(server.dir.files() == std::vector<std::string>{"settings.ini"});

    // Revert goes back to the file.
    CHECK(post(server.port, "/api/set", "roll.spring=70\nrumble.on=false").status == 200);
    CHECK(server.settings.dirty());
    reply = post(server.port, "/api/revert", "");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(server.settings.get("roll.spring") == 55);
    CHECK(server.settings.enabled("rumble.on"));
    CHECK(!server.settings.dirty());
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "55");
    CHECK(contains(reply.body, "\"dirty\":false"));
    CHECK(contains(reply.body, "\"warnings\":[]"));

    // A hand-edited file with a problem reverts as far as it can and says what it skipped.
    writeFile(file, "roll.spring = 61\nmystery = 1\n");
    reply = post(server.port, "/api/revert", "");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(server.settings.get("roll.spring") == 61);
    CHECK(contains(reply.body, "\"warnings\":[\"line 2: unknown setting 'mystery', skipped\"]"));

    // Reset to defaults is live but not saved.
    reply = post(server.port, "/api/reset", "");
    CHECK(reply.status == 200);
    CHECK(server.settings.get("roll.spring") == 30);
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "30");
    CHECK(fieldOf(reply.body, "roll.spring", "saved") == "61");
    CHECK(contains(reply.body, "\"dirty\":true"));
    CHECK(contains(readFile(file), "roll.spring = 61"));

    // Master switch.
    CHECK(server.status.forcesEnabled());
    reply = post(server.port, "/api/forces", "off");
    CHECK(reply.status == 200);
    CHECK(!server.status.forcesEnabled());
    CHECK(contains(reply.body, "\"forcesEnabled\":false"));
    reply = post(server.port, "/api/forces", " ON\r\n");
    CHECK(reply.status == 200);
    CHECK(server.status.forcesEnabled());
    CHECK(contains(reply.body, "\"forcesEnabled\":true"));
    for (const char* body : {"", "maybe", "on off", "enabled=true"})
    {
        CHECK(post(server.port, "/api/forces", body).status == 400);
    }
    CHECK(server.status.forcesEnabled());

    // Release: the request is counted and forces stay off until switched back on.
    CHECK(server.status.releaseRequests() == 0);
    reply = post(server.port, "/api/release", "");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(server.status.releaseRequests() == 1);
    CHECK(!server.status.forcesEnabled());
    CHECK(contains(reply.body, "\"releaseRequests\":1"));
    CHECK(contains(reply.body, "\"forcesEnabled\":false"));
    CHECK(post(server.port, "/api/release", "").status == 200);
    CHECK(server.status.releaseRequests() == 2);

    // Revert with no file at all means defaults.
    fs::remove(file);
    CHECK(post(server.port, "/api/set", "roll.spring=5").status == 200);
    reply = post(server.port, "/api/revert", "");
    CHECK(reply.status == 200);
    CHECK(server.settings.get("roll.spring") == 30);
    CHECK(!server.settings.dirty());
}

void testExportAndImportEndpoints()
{
    Server server;
    CHECK(post(server.port, "/api/set", "roll.spring=64\nrumble.on=off").status == 200);

    // Export: the current values as a file to download, unsaved ones included.
    Reply reply = get(server.port, "/api/export");
    CHECK(reply.status == 200);
    CHECK(reply.hasHeader("Content-Type: text/plain; charset=utf-8"));
    CHECK(reply.hasHeader("Content-Disposition: attachment; filename=\"moza-fs-link-settings.ini\""));
    CHECK(reply.body == server.settings.exportText());
    CHECK(contains(reply.body, "roll.spring = 64") && contains(reply.body, "rumble.on = false"));
    const std::string exported = reply.body;
    CHECK(post(server.port, "/api/export", "").status == 405);
    CHECK(get(server.port, "/api/import").status == 405);

    // Import: back to those values from anywhere else, in effect but not saved.
    CHECK(post(server.port, "/api/reset", "").status == 200);
    CHECK(post(server.port, "/api/save", "").status == 200);
    CHECK(server.settings.get("roll.spring") == 30 && !server.settings.dirty());
    reply = post(server.port, "/api/import", exported);
    CHECK(reply.status == 200 && isJson(reply.body));
    CHECK(server.settings.get("roll.spring") == 64 && !server.settings.enabled("rumble.on"));
    CHECK(fieldOf(reply.body, "roll.spring", "value") == "64" && fieldOf(reply.body, "roll.spring", "saved") == "30");
    CHECK(contains(reply.body, "\"dirty\":true"));
    CHECK(contains(reply.body, "Imported 3 settings, 2 changed") && contains(reply.body, "\"warnings\":[]"));
    CHECK(contains(post(server.port, "/api/import", exported).body, "Imported 3 settings: all the same as now"));

    // What the file gets wrong is reported and the rest still applies; a flood of it is cut short.
    reply = post(server.port, "/api/import", "roll.spring = 12\nroll.damper = 900\nold.setting = 1\n");
    CHECK(reply.status == 200 && server.settings.get("roll.spring") == 12 && server.settings.get("roll.damper") == 100);
    CHECK(contains(reply.body, "using 100") && contains(reply.body, "unknown setting"));
    std::string noisy = "roll.spring = 13\n";
    for (int i = 0; i < 200; ++i)
    {
        noisy += "<img src=x onerror=alert(" + std::to_string(i) + ")> = \"quoted\"\n";
    }
    reply = post(server.port, "/api/import", noisy);
    CHECK(reply.status == 200 && isJson(reply.body) && server.settings.get("roll.spring") == 13);
    CHECK(contains(reply.body, "and 192 more") && reply.body.size() < 8000);

    // A file that is something else changes nothing and says so.
    const auto settled = server.settings.revision();
    for (const std::string& other : {std::string(), std::string("just some text\n"), std::string(3000, '\xff'), std::string("a = 1\nb = 2\n")})
    {
        reply = post(server.port, "/api/import", other);
        CHECK(reply.status == 400 && contains(reply.body, "not a settings file"));
    }
    CHECK(post(server.port, "/api/import", exported + std::string(70000, '#')).status == 413);
    CHECK(server.settings.revision() == settled);

    // Like every other change, only this server's own page may ask for it.
    CHECK(post(server.port, "/api/import", exported, "Origin: http://evil.example\r\n").status == 403);
    CHECK(server.settings.revision() == settled);

    // The page offers both.
    const std::string page = get(server.port, "/").body;
    CHECK(contains(page, "href=\"/api/export\"") && contains(page, "id=\"import\"") && contains(page, "'/api/import'"));
}

void testSaveFailureIsReported()
{
    TempDir             dir;
    bridge::Settings    settings;
    bridge::StatusBoard status;
    registerSample(settings);
    bridge::WebUi ui(settings, status, dir.path / "no_such_directory" / "settings.ini");
    CHECK(ui.start(0));

    CHECK(post(ui.port(), "/api/set", "roll.spring=55").status == 200);
    const Reply reply = post(ui.port(), "/api/save", "");
    CHECK(reply.status == 500);
    CHECK(reply.hasHeader("Content-Type: text/plain; charset=utf-8"));
    CHECK(contains(reply.body, "could not save settings"));
    CHECK(settings.dirty());
    CHECK(get(ui.port(), "/api/state").status == 200);
}

void testRejections()
{
    Server            server;
    const auto        port = server.port;
    const std::string host = hostLine(port);

    // Paths and methods.
    CHECK(get(port, "/nope").status == 404);
    CHECK(get(port, "/api").status == 404);
    CHECK(get(port, "/api/state/").status == 404);
    CHECK(get(port, "/../etc/passwd").status == 404);
    CHECK(post(port, "/api/nope", "").status == 404);
    Reply reply = get(port, "/api/set");
    CHECK(reply.status == 405);
    CHECK(reply.hasHeader("Allow: POST"));
    reply = post(port, "/", "x");
    CHECK(reply.status == 405);
    CHECK(reply.hasHeader("Allow: GET"));
    CHECK(post(port, "/api/state", "").status == 405);
    for (const char* method : {"DELETE", "PUT", "HEAD", "OPTIONS", "PATCH"})
    {
        CHECK(sendRaw(port, std::string(method) + " /api/state HTTP/1.1\r\n" + host + "\r\n").status == 405);
        CHECK(sendRaw(port, std::string(method) + " /api/save HTTP/1.1\r\n" + host + "\r\n").status == 405);
    }

    // Host: only this machine's own names, so a hostile page cannot reach the API by pointing a
    // DNS name of its own at 127.0.0.1.
    for (const char* good : {"127.0.0.1", "127.0.0.1:8777", "localhost", "localhost:1", "LocalHost:8777"})
    {
        CHECK(sendRaw(port, std::string("GET /api/state HTTP/1.1\r\nHost: ") + good + "\r\n\r\n").status == 200);
    }
    for (const char* bad : {"evil.example", "evil.example:8777", "127.0.0.1.evil.example", "localhost.evil.example", "127.0.0.2",
                            "192.168.1.20:8777", "[::1]:8777", "localhost:", "localhost:80:80", "localhost:http", ""})
    {
        reply = sendRaw(port, std::string("GET /api/state HTTP/1.1\r\nHost: ") + bad + "\r\n\r\n");
        CHECK(reply.status == 403);
        CHECK(!contains(reply.body, "roll.spring"));
        CHECK(sendRaw(port, std::string("GET / HTTP/1.1\r\nHost: ") + bad + "\r\n\r\n").status == 403);
    }
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\n" + host + host + "\r\n").status == 400);

    // Origin: a page from anywhere else may not post, even though the browser would send it.
    const auto before = server.settings.revision();
    for (const char* origin : {"http://evil.example", "https://127.0.0.1", "http://localhost:3000", "null"})
    {
        reply = post(port, "/api/set", "roll.spring=99", std::string("Origin: ") + origin + "\r\n");
        CHECK(reply.status == 403);
        CHECK(post(port, "/api/release", "", std::string("Origin: ") + origin + "\r\n").status == 403);
    }
    CHECK(server.settings.revision() == before);
    CHECK(server.status.releaseRequests() == 0);
    CHECK(server.status.forcesEnabled());
    CHECK(post(port, "/api/set", "roll.spring=33", "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n").status == 200);
    CHECK(server.settings.get("roll.spring") == 33);

    // Things that are not HTTP at all.
    CHECK(sendRaw(port, std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) + std::string(200, '\xa7')).status == 400);
    CHECK(sendRaw(port, std::string(64, '\0')).status == 400);
    CHECK(sendRaw(port, "\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "hello\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "GET\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "GET /api/state\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "get /api/state HTTP/1.1\r\n" + host + "\r\n").status == 400);
    CHECK(sendRaw(port, "GET api/state HTTP/1.1\r\n" + host + "\r\n").status == 400);
    CHECK(sendRaw(port, "GET /api/state HTTP/3.0\r\n" + host + "\r\n").status == 400);
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\n" + host + "this is not a header\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\n" + host + " folded: header\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Content-Length: -5\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Content-Length: 12abc\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Content-Length: 99999999999999999999\r\n\r\n").status == 400);
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Content-Length: 3\r\nContent-Length: 3\r\n\r\nabc").status == 400);
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Transfer-Encoding: chunked\r\n\r\n5\r\nabcde\r\n0\r\n\r\n").status == 411);

    // Too much of anything.
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\n" + host + "X-Padding: " + std::string(9000, 'a') + "\r\n\r\n").status == 431);
    CHECK(sendRaw(port, "GET /" + std::string(20000, 'a')).status == 431);
    CHECK(sendRaw(port, std::string(100000, 'A')).status == 431);
    // Refused on the declared length alone, and again with the whole oversized body really sent.
    CHECK(sendRaw(port, "POST /api/set HTTP/1.1\r\n" + host + "Content-Length: 100000\r\n\r\n").status == 413);
    CHECK(post(port, "/api/set", "roll.spring=1\n" + std::string(200000, 'x')).status == 413);
    // Far past what the server will read: it answers, then hangs up on the rest. Whether the
    // answer survives the reset that follows is up to the operating system.
    reply = post(port, "/api/set", std::string(5 * 1024 * 1024, 'x'));
    CHECK(reply.status == 413 || !reply.answered);
    // Just inside the limit is still read in full and judged on its content.
    CHECK(post(port, "/api/set", "roll.spring=34\n" + std::string(16000, '\n')).status == 200);
    CHECK(server.settings.get("roll.spring") == 34);

    // Lenient where it costs nothing: HTTP/1.0, bare line feeds, a client that half-closes.
    CHECK(sendRaw(port, "GET /api/state HTTP/1.0\r\n" + host + "\r\n").status == 200);
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\nHost: localhost\n\n").status == 200);
    CHECK(sendRaw(port, "GET /api/state HTTP/1.1\r\nhOsT:localhost\r\nAccept: */*\r\n\r\n").status == 200);
    {
        const Socket socket = connectTo(port);
        CHECK(socket != kNoSocket);
        sendAll(socket, "GET /api/state HTTP/1.1\r\n" + host + "\r\n");
#ifdef _WIN32
        shutdown(socket, SD_SEND);
#else
        ::shutdown(socket, SHUT_WR);
#endif
        CHECK(parseReply(readAll(socket, 5.0)).status == 200);
        closeSocket(socket);
    }

    // After all of that the server is still serving, and nothing leaked into the settings.
    reply = get(port, "/api/state");
    CHECK(reply.status == 200);
    CHECK(isJson(reply.body));
    CHECK(server.settings.get("roll.spring") == 34);
    CHECK(server.ui.running());
}

void testSlowAndIdleClients()
{
    Server     server;
    const auto port = server.port;

    // One client connects and says nothing, another stops halfway through its request.
    const auto   start   = Clock::now();
    const Socket idle    = connectTo(port);
    const Socket stalled = connectTo(port);
    CHECK(idle != kNoSocket);
    CHECK(stalled != kNoSocket);
    sendAll(stalled, "POST /api/set HTTP/1.1\r\n" + hostLine(port) + "Content-Length: 50\r\n\r\nroll.spr");

    // Neither holds anyone else up.
    for (int i = 0; i < 5; ++i)
    {
        const auto asked = Clock::now();
        CHECK(get(port, "/api/state").status == 200);
        CHECK(secondsSince(asked) < 0.5);
    }

    // The stalled one is told it ran out of time; the silent one is simply dropped. Both after
    // about two seconds, not straight away and not never.
    bool        closed = false;
    const Reply late   = parseReply(readAll(stalled, 6.0, &closed));
    const double waited = secondsSince(start);
    CHECK(late.status == 408);
    CHECK(closed);
    CHECK(waited > 1.5 && waited < 4.5);
    CHECK(server.settings.get("roll.spring") == 30);

    closed = false;
    CHECK(readAll(idle, 3.0, &closed).empty());
    CHECK(closed);
    closeSocket(idle);
    closeSocket(stalled);

    // More silent clients than the server keeps: the oldest make room, and real requests still get through.
    std::vector<Socket> crowd;
    for (int i = 0; i < 48; ++i)
    {
        crowd.push_back(connectTo(port));
        CHECK(crowd.back() != kNoSocket);
    }
    const auto asked = Clock::now();
    CHECK(get(port, "/api/state").status == 200);
    CHECK(secondsSince(asked) < 1.0);
    for (const Socket socket : crowd)
    {
        if (socket != kNoSocket)
        {
            closeSocket(socket);
        }
    }
    CHECK(get(port, "/api/state").status == 200);

    // Stopping does not wait for clients: one idle, one mid-request, both still connected.
    const Socket lingering = connectTo(port);
    const Socket halfway   = connectTo(port);
    CHECK(lingering != kNoSocket);
    CHECK(halfway != kNoSocket);
    sendAll(halfway, "GET /api/sta");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto stopping = Clock::now();
    server.ui.stop();
    CHECK(secondsSince(stopping) < 0.5);
    CHECK(!server.ui.running());
    closed = false;
    CHECK(readAll(lingering, 2.0, &closed).empty());
    CHECK(closed);
    closeSocket(lingering);
    closeSocket(halfway);
}

void testStartAndStop()
{
    TempDir             dir;
    bridge::Settings    settings;
    bridge::StatusBoard status;
    registerSample(settings);

    bridge::WebUi ui(settings, status, dir.path / "settings.ini");
    CHECK(!ui.running());
    CHECK(ui.port() == 0);
    ui.stop(); // never started: nothing to do

    CHECK(ui.start(0));
    CHECK(ui.running());
    const std::uint16_t first = ui.port();
    CHECK(first != 0);
    CHECK(get(first, "/api/state").status == 200);

    // Already running, and a port that is taken, are both refused with a reason.
    std::string error;
    CHECK(!ui.start(0, &error));
    CHECK(contains(error, "already running"));
    CHECK(ui.port() == first);
    {
        bridge::WebUi second(settings, status, dir.path / "settings.ini");
        error.clear();
        CHECK(!second.start(first, &error));
        CHECK(contains(error, "127.0.0.1:" + std::to_string(first)));
        CHECK(error.size() > std::string("cannot listen on 127.0.0.1:65535: ").size());
        CHECK(!second.running());
        CHECK(second.port() == 0);
        CHECK(!second.start(first));
    }
    CHECK(get(first, "/api/state").status == 200);

    auto stopping = Clock::now();
    ui.stop();
    CHECK(secondsSince(stopping) < 0.5);
    CHECK(!ui.running());
    CHECK(ui.port() == 0);
    CHECK(connectTo(first) == kNoSocket);
    ui.stop();

    // The same object starts again, here on the port it just gave up.
    error.clear();
    CHECK(ui.start(first, &error));
    CHECK(error.empty());
    CHECK(ui.port() == first);
    CHECK(get(first, "/").status == 200);
    stopping = Clock::now();
    ui.stop();
    CHECK(secondsSince(stopping) < 0.5);
    CHECK(connectTo(first) == kNoSocket);

    // Destroying a running server stops it.
    std::uint16_t scoped = 0;
    {
        bridge::WebUi temporary(settings, status, dir.path / "settings.ini");
        CHECK(temporary.start(0));
        scoped = temporary.port();
        CHECK(get(scoped, "/api/state").status == 200);
    }
    CHECK(connectTo(scoped) == kNoSocket);
}

void testRequestsFromSeveralThreads()
{
    Server                   server;
    std::atomic<int>         bad{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n)
    {
        threads.emplace_back([&, n] {
            for (int i = 0; i < 25; ++i)
            {
                const Reply reply = i % 2 ? get(server.port, "/api/state")
                                          : post(server.port, "/api/set", "roll.spring=" + std::to_string((n * 25 + i) % 100));
                if (reply.status != 200 || !isJson(reply.body))
                {
                    ++bad;
                }
                server.status.set("speed", i);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK(bad.load() == 0);
}

} // namespace

int main()
{
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#else
    // A server that closes on us mid-send must show up as a failed send, not end the test run.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    testRegistration();
    testGetSetAndClamping();
    testChangeCounter();
    testDirtyFlag();
    testSaveLoadRoundTrip();
    testExportAndImport();
    testTolerantLoad();
    testMissingAndUnreadableFile();
    testAtomicSave();
    testNumberText();
    testLocaleIndependence();
    testSettingsFromSeveralThreads();
    testStatusBoard();
    testPage();
    testState();
    testSetEndpoint();
    testActions();
    testExportAndImportEndpoints();
    testSaveFailureIsReported();
    testRejections();
    testSlowAndIdleClients();
    testStartAndStop();
    testRequestsFromSeveralThreads();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
