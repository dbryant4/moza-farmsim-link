// Checks the telemetry parser, the file reader and the force model. None of them touch the device.

#include "force_model.hpp"
#include "pitch_servo.hpp"
#include "telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bridge::ForceModel;
using bridge::ForceOutput;
using bridge::ForceSettings;
using bridge::Surface;
using bridge::Telemetry;
using bridge::TelemetryReader;
using bridge::TelemetryState;
using bridge::parseTelemetry;

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

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

// The example line from docs/telemetry.md.
const std::string kContractLine =
    "MOZAFS 1 seq=4821 t=913400 active=1 speed=5.42 steer=-0.250 mass=8200 rpm=1450 load=0.62 slip=0.04 rough=0.70 "
    "field=1 lowered=1 working=1 end=1\n";

/** A complete line with the given seq and middle tokens. */
std::string line(std::uint64_t seq, const std::string& tokens = "active=1 speed=5.42")
{
    return "MOZAFS 1 seq=" + std::to_string(seq) + " " + tokens + " end=1\n";
}

// ---------------------------------------------------------------------------------------------
// Parser

void testParsesTheContractExample()
{
    const auto sample = parseTelemetry(kContractLine);
    CHECK(sample.has_value());
    if (!sample)
    {
        return;
    }
    CHECK(sample->seq == 4821);
    CHECK(sample->timeMs == 913400);
    CHECK(sample->active);
    CHECK(sample->speed == 5.42);
    CHECK(sample->steer == -0.25);
    CHECK(sample->mass == 8200.0);
    CHECK(sample->rpm == 1450.0);
    CHECK(sample->load == 0.62);
    CHECK(sample->slip == 0.04);
    CHECK(sample->rough == 0.7);
    CHECK(sample->field);
    CHECK(sample->lowered);
    CHECK(sample->working);
}

void testRejectsFramingViolations()
{
    CHECK(!parseTelemetry(""));
    CHECK(!parseTelemetry("\n"));
    CHECK(!parseTelemetry("MOZAFS"));
    CHECK(!parseTelemetry("MOZAFS 1 end=1"));                 // framed, but seq is required
    CHECK(!parseTelemetry("MOZAFX 1 seq=1 active=1 end=1\n")); // wrong magic
    CHECK(!parseTelemetry("mozafs 1 seq=1 active=1 end=1\n"));
    CHECK(!parseTelemetry(" MOZAFS 1 seq=1 active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 2 seq=1 active=1 end=1\n")); // a version this reader does not know
    CHECK(!parseTelemetry("MOZAFS 10 seq=1 active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS seq=1 active=1 end=1\n"));   // no version
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1\n"));       // no end token
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 end=0\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 end=11\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 send=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 end=1 active=1\n")); // end token present but not last
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 end=1 \n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 end=1\n\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1 end=1\nMOZAFS 1 seq=2 active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1 active=1\tend=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1\tactive=1 end=1\n"));
    CHECK(!parseTelemetry(std::string_view("MOZAFS 1 seq=1 \0\0\0\0\0\0 end=1\n", 28))); // a hole left by a seek past the end

    CHECK(!parseTelemetry("MOZAFS 1 active=1 speed=3 end=1\n")); // seq missing
    CHECK(!parseTelemetry("MOZAFS 1 seq= active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=abc active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=-1 active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=1.5 active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=12x active=1 end=1\n"));
    CHECK(!parseTelemetry("MOZAFS 1 seq=99999999999999999999999 active=1 end=1\n"));
}

void testRejectsEveryPartialWrite()
{
    // Whatever prefix of the line a read catches, it must not pass for a sample. The only prefix
    // that may is the whole line short of its newline, which carries everything.
    const std::string_view full = kContractLine;
    for (std::size_t length = 0; length + 1 < full.size(); ++length)
    {
        CHECK(!parseTelemetry(full.substr(0, length)));
    }
    CHECK(parseTelemetry(full.substr(0, full.size() - 1)) == parseTelemetry(full));
}

void testIgnoresUnknownKeysAndDefaultsMissingOnes()
{
    Telemetry expected;
    expected.seq = 7;
    CHECK(parseTelemetry("MOZAFS 1 seq=7 end=1\n") == expected);

    // Keys from a later minor revision, a token with no '=' and a doubled space change nothing.
    expected.active = true;
    expected.speed  = 3.5;
    CHECK(parseTelemetry("MOZAFS 1 seq=7 gear=3 active=1 fuel=0.5 junk  speed=3.5 =9 name=a=b end=1\n") == expected);

    // active=0 with everything else omitted, as the mod writes while the player is on foot.
    const auto onFoot = parseTelemetry("MOZAFS 1 seq=8 active=0 end=1\n");
    CHECK(onFoot && !onFoot->active && onFoot->speed == 0.0 && onFoot->mass == 0.0 && !onFoot->working);

    // The engine keys added after the first release: absent means a combustion engine that is not starting.
    CHECK(onFoot && !onFoot->starting && !onFoot->electric);
    const auto cranking = parseTelemetry("MOZAFS 1 seq=8 active=1 starting=1 electric=0 end=1\n");
    CHECK(cranking && cranking->starting && !cranking->electric);
    const auto batteryPowered = parseTelemetry("MOZAFS 1 seq=8 active=1 electric=1 rpm=3000 end=1\n");
    CHECK(batteryPowered && batteryPowered->electric && !batteryPowered->starting && batteryPowered->rpm == 3000.0);

    const auto ranged = parseTelemetry("MOZAFS 1 seq=8 active=1 rpm=1400 rpmmin=850 rpmmax=1900 end=1\n");
    CHECK(ranged && ranged->rpmMin == 850.0 && ranged->rpmMax == 1900.0 && ranged->rpm == 1400.0);
    CHECK(onFoot && onFoot->rpmMin == 0.0 && onFoot->rpmMax == 0.0);

    const auto bumped = parseTelemetry("MOZAFS 1 seq=8 active=1 bumpl=0.25 bumpr=1.00 end=1\n");
    CHECK(bumped && bumped->bumpLeft == 0.25 && bumped->bumpRight == 1.0);
    CHECK(onFoot && onFoot->bumpLeft == 0.0 && onFoot->bumpRight == 0.0);

    // The kind of surface, added later still: a number from the contract's list, anything else unknown.
    CHECK(onFoot && onFoot->surface == Surface::Unknown);
    const auto surfaceOf = [](const std::string& value) {
        const auto parsed = parseTelemetry("MOZAFS 1 seq=8 active=1 rough=0.85 surface=" + value + " field=1 end=1\n");
        CHECK(parsed && parsed->rough == 0.85 && parsed->field);
        return parsed ? parsed->surface : Surface::Unknown;
    };
    CHECK(surfaceOf("1") == Surface::Road);
    CHECK(surfaceOf("2") == Surface::Gravel);
    CHECK(surfaceOf("3") == Surface::Grass);
    CHECK(surfaceOf("4") == Surface::Snow);
    CHECK(surfaceOf("5") == Surface::Heap);
    CHECK(surfaceOf("6") == Surface::Field);
    CHECK(surfaceOf("7") == Surface::Tilled);
    CHECK(surfaceOf("8") == Surface::Ridge);
    for (const char* unknown : {"0", "9", "-1", "250", "7.5", "tilled", "99999999999999999999"})
    {
        CHECK(surfaceOf(unknown) == Surface::Unknown);
    }

    // Order does not matter, and a repeated key takes its last value.
    const auto reordered = parseTelemetry("MOZAFS 1 speed=1 active=1 speed=2 seq=9 end=1\n");
    CHECK(reordered && reordered->seq == 9 && reordered->speed == 2.0 && reordered->active);
}

void testBadNumbersFallBackToDefaults()
{
    // Every optional value is malformed here; the sample survives with those keys at their defaults.
    const auto sample = parseTelemetry("MOZAFS 1 seq=5 t=12x active=yes speed=fast steer=1e3 mass=nan rpm=inf load=0,5 slip= "
                                       "rough=--1 field=? lowered=1 working=1 end=1\n");
    Telemetry expected;
    expected.seq     = 5;
    expected.lowered = true;
    expected.working = true;
    CHECK(sample == expected);

    // One bad value does not take its neighbours with it.
    const auto mixed = parseTelemetry("MOZAFS 1 seq=6 active=1 speed=4.5 steer=x mass=9000 end=1\n");
    CHECK(mixed && mixed->active && mixed->speed == 4.5 && mixed->steer == 0.0 && mixed->mass == 9000.0);

    // Out of the documented range is still a number; clamping is the force model's job.
    const auto wild = parseTelemetry("MOZAFS 1 seq=6 active=1 speed=-7.5 steer=3 slip=-2 end=1\n");
    CHECK(wild && wild->speed == -7.5 && wild->steer == 3.0 && wild->slip == -2.0);

    const auto huge = parseTelemetry("MOZAFS 1 seq=6 active=1 mass=" + std::string(400, '9') + " end=1\n");
    CHECK(huge && huge->mass == 0.0);
}

void testAcceptsLineEndings()
{
    const auto lf   = parseTelemetry("MOZAFS 1 seq=3 active=1 speed=2.5 end=1\n");
    const auto crlf = parseTelemetry("MOZAFS 1 seq=3 active=1 speed=2.5 end=1\r\n");
    const auto none = parseTelemetry("MOZAFS 1 seq=3 active=1 speed=2.5 end=1");
    CHECK(lf && lf->speed == 2.5);
    CHECK(crlf == lf);
    CHECK(none == lf);
}

/** Returns false if the machine has no comma-decimal locale to test with. */
bool testParsingIgnoresTheLocale()
{
    const char* const locales[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "German_Germany.1252", "de-DE"};
    bool              switched  = false;
    for (const char* name : locales)
    {
        // Only a locale under which the C library itself reads "1,5" as one and a half proves anything.
        if (std::setlocale(LC_ALL, name) != nullptr && std::strtod("1,5", nullptr) == 1.5)
        {
            switched = true;
            break;
        }
    }
    if (switched)
    {
        const auto sample = parseTelemetry(kContractLine);
        CHECK(sample && sample->speed == 5.42 && sample->steer == -0.25 && sample->load == 0.62);
        const auto comma = parseTelemetry("MOZAFS 1 seq=1 active=1 speed=5,42 end=1\n");
        CHECK(comma && comma->speed == 0.0);
    }
    std::setlocale(LC_ALL, "C");
    return switched;
}

// ---------------------------------------------------------------------------------------------
// Reader

/** A telemetry file in the temp directory, removed again on scope exit. */
struct TempFile
{
    TempFile()
        : path(std::filesystem::temp_directory_path() /
               ("moza_fs_bridge_test_" + std::to_string(std::random_device{}()) + "_telemetry.txt"))
    {
    }
    ~TempFile() { remove(); }

    void write(std::string_view content) const
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    void remove() const
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

using std::chrono::milliseconds;

// An arbitrary instant for the injected clock; only differences matter.
const TelemetryReader::Clock::time_point kStart{std::chrono::hours(1)};

void testReaderWithoutAFile()
{
    TempFile        file;
    TelemetryReader reader(file.path);

    CHECK(reader.poll(kStart).state == TelemetryState::NeverSeen);
    CHECK(reader.poll(kStart).fresh() == nullptr);
    CHECK(reader.view().sample == Telemetry{});

    file.write("");
    CHECK(reader.poll(kStart + milliseconds(16)).state == TelemetryState::NeverSeen);
    file.write("MOZAFS 1 seq=1 active=1 spe");
    CHECK(reader.poll(kStart + milliseconds(32)).state == TelemetryState::NeverSeen);
    file.write("this is not a telemetry file\n");
    CHECK(reader.poll(kStart + milliseconds(48)).state == TelemetryState::NeverSeen);

    // Nothing useful at an empty path either, which is what defaultTelemetryPath() can return.
    TelemetryReader nowhere{std::filesystem::path()};
    CHECK(nowhere.poll(kStart).state == TelemetryState::NeverSeen);
}

void testReaderWaitsForTheFileToMove()
{
    TempFile        file;
    TelemetryReader reader(file.path);

    // Left behind by an earlier session: a sample, but no evidence that the game is running.
    file.write(line(900, "active=1 speed=20"));
    const auto& first = reader.poll(kStart);
    CHECK(first.state == TelemetryState::Stale);
    CHECK(first.fresh() == nullptr);
    CHECK(first.sample.seq == 900 && first.sample.speed == 20.0);
    CHECK(reader.poll(kStart + milliseconds(100)).state == TelemetryState::Stale);
    CHECK(reader.poll(kStart + milliseconds(5000)).state == TelemetryState::Stale);

    file.write(line(901, "active=1 speed=3"));
    const auto& moved = reader.poll(kStart + milliseconds(5016));
    CHECK(moved.state == TelemetryState::Fresh);
    CHECK(moved.fresh() != nullptr && moved.fresh()->seq == 901 && moved.fresh()->speed == 3.0);
}

void testReaderGoesStaleAndRecovers()
{
    TempFile        file;
    TelemetryReader reader(file.path);

    file.write(line(1));
    reader.poll(kStart);
    file.write(line(2));
    CHECK(reader.poll(kStart + milliseconds(16)).state == TelemetryState::Fresh);

    // The same sample read again is not news.
    const auto changedAt = kStart + milliseconds(16);
    CHECK(reader.poll(changedAt + milliseconds(250)).state == TelemetryState::Fresh);
    CHECK(reader.poll(changedAt + milliseconds(499)).state == TelemetryState::Fresh);
    CHECK(reader.view().sinceChange == milliseconds(499));
    const auto& stale = reader.poll(changedAt + milliseconds(500));
    CHECK(stale.state == TelemetryState::Stale);
    CHECK(stale.fresh() == nullptr);
    CHECK(stale.sample.seq == 2); // still there to look at, just not to act on
    CHECK(reader.poll(changedAt + milliseconds(60000)).state == TelemetryState::Stale);

    // Rewriting the file without a new seq does not revive it.
    file.write(line(2, "active=1 speed=9"));
    CHECK(reader.poll(changedAt + milliseconds(60016)).state == TelemetryState::Stale);
    CHECK(reader.view().sample.speed == 5.42);

    file.write(line(3, "active=1 speed=9"));
    const auto& back = reader.poll(changedAt + milliseconds(60032));
    CHECK(back.state == TelemetryState::Fresh);
    CHECK(back.sample.seq == 3 && back.sample.speed == 9.0);
    CHECK(back.sinceChange == milliseconds(0));
}

void testReaderStaleThresholdIsConfigurable()
{
    TempFile        file;
    TelemetryReader reader(file.path, milliseconds(100));

    file.write(line(1));
    reader.poll(kStart);
    file.write(line(2));
    CHECK(reader.poll(kStart + milliseconds(10)).state == TelemetryState::Fresh);
    CHECK(reader.poll(kStart + milliseconds(109)).state == TelemetryState::Fresh);
    CHECK(reader.poll(kStart + milliseconds(110)).state == TelemetryState::Stale);
}

void testReaderKeepsTheLastSampleThroughBadReads()
{
    TempFile        file;
    TelemetryReader reader(file.path);

    file.write(line(10));
    reader.poll(kStart);
    file.write(line(11, "active=1 speed=4.25 steer=0.5"));
    CHECK(reader.poll(kStart + milliseconds(16)).state == TelemetryState::Fresh);
    const Telemetry good = reader.view().sample;
    CHECK(good.seq == 11 && good.speed == 4.25 && good.steer == 0.5);

    // Caught between the writer's truncate and its write, part way through the write, and with
    // the file briefly gone: each time the previous sample stands and stays fresh.
    file.write("");
    CHECK(reader.poll(kStart + milliseconds(32)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample == good);

    file.write("MOZAFS 1 seq=12 active=1 speed=99 steer=-1 mas");
    CHECK(reader.poll(kStart + milliseconds(48)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample == good);

    file.remove();
    CHECK(reader.poll(kStart + milliseconds(64)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample == good);

    // A file too long to be read whole is no better than a partial one, even when the 4096 bytes
    // the reader takes in happen to stop right behind an end token.
    file.write(line(12, "active=1 pad=" + std::string(8000, 'x')));
    CHECK(reader.poll(kStart + milliseconds(80)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample == good);

    const std::string head = "MOZAFS 1 seq=12 active=1 speed=99 pad=";
    const std::string tail = " end=1";
    const std::string cut  = head + std::string(4096 - head.size() - tail.size(), 'x') + tail;
    CHECK(parseTelemetry(cut).has_value());
    file.write(cut + " more=1 end=1\n");
    CHECK(reader.poll(kStart + milliseconds(96)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample == good);

    // Bad reads do not count as movement, so staleness is still measured from seq 11.
    CHECK(reader.poll(kStart + milliseconds(16 + 500)).state == TelemetryState::Stale);
    CHECK(reader.view().sample == good);

    file.write(line(12, "active=1 speed=6"));
    CHECK(reader.poll(kStart + milliseconds(600)).state == TelemetryState::Fresh);
    CHECK(reader.view().sample.seq == 12 && reader.view().sample.speed == 6.0);
}

void testReaderTreatsASeqRestartAsNew()
{
    TempFile        file;
    TelemetryReader reader(file.path);

    file.write(line(90000, "active=1 speed=8"));
    reader.poll(kStart);
    file.write(line(90001, "active=1 speed=8"));
    CHECK(reader.poll(kStart + milliseconds(16)).state == TelemetryState::Fresh);

    // The game was restarted: stale while it loads, then seq begins again from the bottom.
    CHECK(reader.poll(kStart + milliseconds(30000)).state == TelemetryState::Stale);
    file.write(line(1, "active=0"));
    const auto& restarted = reader.poll(kStart + milliseconds(30016));
    CHECK(restarted.state == TelemetryState::Fresh);
    CHECK(restarted.sample.seq == 1 && !restarted.sample.active && restarted.sample.speed == 0.0);

    // Going backwards without a pause in between is taken the same way.
    file.write(line(2, "active=1 speed=1"));
    CHECK(reader.poll(kStart + milliseconds(30032)).sample.seq == 2);
    file.write(line(1, "active=1 speed=7"));
    const auto& backwards = reader.poll(kStart + milliseconds(30048));
    CHECK(backwards.state == TelemetryState::Fresh && backwards.sample.seq == 1 && backwards.sample.speed == 7.0);
}

void testReaderNeverReturnsATornSample()
{
    // A second thread rewrites the file as fast as it can, in two flushed halves so that partial
    // content is really on disk part of the time, while this one polls. Every line is internally
    // consistent (speed and mass both derive from seq), so a sample assembled from a torn read
    // would show.
    constexpr std::uint64_t kWrites = 3000;

    TempFile          file;
    TelemetryReader   reader(file.path);
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (std::uint64_t seq = 1; seq <= kWrites; ++seq)
        {
            const std::string text = line(seq, "active=1 speed=" + std::to_string(seq) + ".5 mass=" + std::to_string(seq * 2));
            const auto        half = static_cast<std::streamsize>(text.size() / 2);
            std::ofstream     out(file.path, std::ios::binary | std::ios::trunc);
            out.write(text.data(), half);
            out.flush();
            out.write(text.data() + half, static_cast<std::streamsize>(text.size()) - half);
        }
        done = true;
    });

    auto          now      = kStart;
    std::uint64_t lastSeq  = 0;
    int           distinct = 0;
    while (!done)
    {
        now += milliseconds(1);
        const auto& view = reader.poll(now);
        if (view.state == TelemetryState::NeverSeen)
        {
            continue;
        }
        const Telemetry& sample = view.sample;
        CHECK(sample.seq >= lastSeq && sample.seq <= kWrites);
        CHECK(sample.active);
        CHECK(sample.speed == static_cast<double>(sample.seq) + 0.5);
        CHECK(sample.mass == static_cast<double>(sample.seq * 2));
        distinct += sample.seq != lastSeq ? 1 : 0;
        lastSeq = sample.seq;
    }
    writer.join();

    CHECK(distinct > 1);
    CHECK(reader.poll(now + milliseconds(1)).sample.seq == kWrites);
}

void setEnvironment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}

void testDefaultTelemetryPath()
{
    const char* const current = std::getenv("USERPROFILE");
    const bool        hadOne  = current != nullptr;
    const std::string saved   = hadOne ? current : "";

    setEnvironment("USERPROFILE", nullptr);
    CHECK(bridge::defaultTelemetryPath().empty());

#ifdef _WIN32
    const char* const profile = "C:\\Users\\Tester";
#else
    const char* const profile = "/home/tester";
#endif
    setEnvironment("USERPROFILE", profile);
    const auto path = bridge::defaultTelemetryPath();
    CHECK(path == std::filesystem::path(profile) / "Documents" / "My Games" / "FarmingSimulator2025" / "modSettings" /
                      "FS25_MozaYokeLink" / "telemetry.txt");
    CHECK(path.filename() == "telemetry.txt");

    setEnvironment("USERPROFILE", hadOne ? saved.c_str() : nullptr);
}

// ---------------------------------------------------------------------------------------------
// Force model

/** A mid-size tractor driving at @p speed on a smooth road with the wheels gripping. */
Telemetry driving(double speed, double steer = 0.0)
{
    Telemetry telemetry;
    telemetry.seq    = 1;
    telemetry.active = true;
    telemetry.speed  = speed;
    telemetry.steer  = steer;
    telemetry.mass   = 8200.0;
    telemetry.rpm    = 1450.0;
    telemetry.load   = 0.5;
    return telemetry;
}

/** The same tractor working a field with a lowered implement. */
Telemetry workingField(double speed, double steer = 0.0)
{
    Telemetry telemetry = driving(speed, steer);
    telemetry.rough     = 0.7;
    telemetry.field     = true;
    telemetry.lowered   = true;
    telemetry.working   = true;
    return telemetry;
}

/** Runs the model long enough for every output to arrive. */
ForceOutput settle(ForceModel& model, const Telemetry* telemetry, double seconds = 10.0)
{
    for (int i = 0; i < std::lround(seconds / 0.01); ++i)
    {
        model.update(telemetry, 0.01);
    }
    return model.output();
}

/** Where the default model ends up for @p telemetry. */
ForceOutput settled(const Telemetry& telemetry, const ForceSettings& settings = {})
{
    ForceModel model(settings);
    return settle(model, &telemetry);
}

std::vector<double> fields(const ForceOutput& out)
{
    return {out.rollSpring, out.rollDamper, out.rollForce, out.pitchSpring, out.pitchDamper, out.rumble, out.rumbleHz};
}

double largestDifference(const ForceOutput& a, const ForceOutput& b)
{
    double     largest = 0.0;
    const auto x       = fields(a);
    const auto y       = fields(b);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        largest = std::max(largest, std::abs(x[i] - y[i]));
    }
    return largest;
}

bool withinRanges(const ForceOutput& out, const ForceSettings& settings = {})
{
    const auto unit = [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; };
    return unit(out.rollSpring) && unit(out.rollDamper) && unit(out.pitchSpring) && unit(out.pitchDamper) && unit(out.rumble) &&
           std::isfinite(out.rollForce) && out.rollForce >= -1.0 && out.rollForce <= 1.0 && std::isfinite(out.rumbleHz) &&
           out.rumbleHz >= settings.rumbleMinHz && out.rumbleHz <= settings.rumbleMaxHz;
}

void testStartsFromNothing()
{
    const ForceSettings settings;
    ForceModel          model;
    const ForceOutput&  initial = model.output();
    CHECK(initial.rollSpring == 0.0 && initial.rollDamper == 0.0 && initial.rollForce == 0.0);
    CHECK(initial.pitchSpring == 0.0 && initial.pitchDamper == 0.0 && initial.rumble == 0.0);
    CHECK(initial.rumbleHz == settings.rumbleMinHz);
}

void testRollSpringScalesWithSpeed()
{
    const ForceSettings settings;

    // Parked: the wheel stays where it is left.
    CHECK(near(settled(driving(0.0)).rollSpring, 0.0, 1e-6));

    double previous = 0.0;
    for (double speed = 0.5; speed <= settings.rollSpringFullSpeed; speed += 0.5)
    {
        const double spring = settled(driving(speed)).rollSpring;
        CHECK(spring > previous);
        previous = spring;
    }
    CHECK(near(settled(driving(settings.rollSpringFullSpeed)).rollSpring, settings.rollSpringMax, 1e-6));
    CHECK(near(settled(driving(40.0)).rollSpring, settings.rollSpringMax, 1e-6));

    // Smooth from zero: creeping gives a trace of centering, not a jump to it.
    CHECK(settled(driving(0.05)).rollSpring < 0.01);
    // Field speeds already center noticeably.
    CHECK(settled(driving(3.0)).rollSpring > 0.2);

    CHECK(near(settled(driving(-6.0)).rollSpring, settled(driving(6.0)).rollSpring));
    CHECK(settled(driving(-6.0)).rollSpring > 0.3);
}

void testRollDamperIsTheWeight()
{
    const ForceSettings settings;
    const auto          damperAt = [](double speed, double mass) {
        Telemetry telemetry = driving(speed);
        telemetry.mass      = mass;
        return settled(telemetry).rollDamper;
    };

    // Rolling, with no mass reported: just the base.
    CHECK(near(damperAt(8.0, 0.0), settings.rollDamperBase, 1e-6));

    // Heavier is heavier, by less and less, and never beyond the configured ceiling.
    const double light  = damperAt(8.0, 5000.0);
    const double medium = damperAt(8.0, 15000.0);
    const double heavy  = damperAt(8.0, 25000.0);
    CHECK(light > settings.rollDamperBase);
    CHECK(medium > light && heavy > medium);
    CHECK(heavy - medium < medium - light);
    CHECK(near(damperAt(8.0, settings.rollDamperMassHalf), settings.rollDamperBase + 0.5 * settings.rollDamperMass, 1e-6));
    CHECK(damperAt(8.0, 1.0e9) <= settings.rollDamperBase + settings.rollDamperMass + 1e-6);
    CHECK(damperAt(8.0, 1.0e9) > settings.rollDamperBase + 0.99 * settings.rollDamperMass);

    // Scrubbing the tyres at standstill is heavier than steering while rolling.
    CHECK(near(damperAt(0.0, 8200.0) - damperAt(8.0, 8200.0), settings.rollDamperStandstill, 1e-6));
    CHECK(damperAt(0.5, 8200.0) < damperAt(0.0, 8200.0));
    CHECK(damperAt(0.5, 8200.0) > damperAt(8.0, 8200.0));
    CHECK(near(damperAt(settings.rollDamperStandstillSpeed, 8200.0), damperAt(8.0, 8200.0), 1e-6));
    CHECK(near(damperAt(-8.0, 8200.0), damperAt(8.0, 8200.0)));
}

void testRollForceSelfAligns()
{
    const ForceSettings settings;

    // Toward center: steering right is pushed left, and the other way round.
    const double right = settled(driving(10.0, 0.5)).rollForce;
    const double left  = settled(driving(10.0, -0.5)).rollForce;
    CHECK(right < 0.0);
    CHECK(left > 0.0);
    CHECK(near(right, -left));
    CHECK(near(settled(driving(10.0, 0.0)).rollForce, 0.0));
    // Reversing still pulls toward center.
    CHECK(settled(driving(-10.0, 0.5)).rollForce < 0.0);

    // Grows with deflection and with speed, from nothing at standstill.
    CHECK(near(settled(driving(0.0, 1.0)).rollForce, 0.0, 1e-6));
    CHECK(std::abs(settled(driving(10.0, 1.0)).rollForce) > std::abs(right));
    CHECK(std::abs(settled(driving(3.0, 0.5)).rollForce) < std::abs(right));
    CHECK(std::abs(settled(driving(3.0, 0.5)).rollForce) > 0.0);

    // Modest: full lock at speed is the configured pull, well short of the spring.
    const double fullLock = settled(driving(30.0, 1.0)).rollForce;
    CHECK(near(fullLock, -settings.selfAlign, 1e-6));
    CHECK(std::abs(fullLock) <= 0.2);

    // Collapses as the driven wheels let go.
    const auto withSlip = [](double slip) {
        Telemetry telemetry = driving(10.0, 1.0);
        telemetry.slip      = slip;
        return std::abs(settled(telemetry).rollForce);
    };
    CHECK(near(withSlip(0.05), withSlip(0.0)));
    CHECK(withSlip(0.35) < withSlip(0.05));
    CHECK(withSlip(0.35) > 0.0);
    CHECK(near(withSlip(settings.selfAlignSlipEnd), 0.0, 1e-6));
    CHECK(near(withSlip(1.0), 0.0, 1e-6));
}

void testRollForceAddsImplementDrag()
{
    const ForceSettings settings;

    const double plain   = std::abs(settled(driving(3.0, 0.6)).rollForce);
    const double working = std::abs(settled(workingField(3.0, 0.6)).rollForce);
    CHECK(working > plain);
    CHECK(near(working - plain, 0.6 * settings.implementDrag, 1e-6));
    CHECK(settled(workingField(3.0, 0.6)).rollForce < 0.0);
    CHECK(settled(workingField(3.0, -0.6)).rollForce > 0.0);

    // Lowered but idle, or running but raised, is not dragging through the ground.
    Telemetry loweredOnly   = workingField(3.0, 0.6);
    loweredOnly.working     = false;
    Telemetry workingRaised = workingField(3.0, 0.6);
    workingRaised.lowered   = false;
    CHECK(near(std::abs(settled(loweredOnly).rollForce), plain, 1e-6));
    CHECK(near(std::abs(settled(workingRaised).rollForce), plain, 1e-6));

    // Nothing to drag at standstill, and wheel slip does not take the implement's share away.
    CHECK(near(settled(workingField(0.0, 1.0)).rollForce, 0.0, 1e-6));
    Telemetry spinning = workingField(3.0, 1.0);
    spinning.slip      = 1.0;
    CHECK(near(settled(spinning).rollForce, -settings.implementDrag, 1e-6));

    // Everything at once stays under the cap.
    CHECK(std::abs(settled(workingField(30.0, 1.0)).rollForce) <= settings.rollForceLimit + 1e-12);
    ForceSettings strong;
    strong.selfAlign     = 0.9;
    strong.implementDrag = 0.9;
    CHECK(near(settled(workingField(30.0, 1.0), strong).rollForce, -strong.rollForceLimit, 1e-6));
}

void testRumbleFollowsSurfaceAndSpeed()
{
    const ForceSettings settings;
    const auto          rumbleAt = [](double speed, double rough, bool working = false) {
        Telemetry telemetry = driving(speed);
        telemetry.rough     = rough;
        telemetry.working   = working;
        telemetry.lowered   = working;
        return settled(telemetry);
    };

    // None when stopped, engine running or not, and none on smooth asphalt at any speed.
    CHECK(near(rumbleAt(0.0, 1.0).rumble, 0.0, 1e-6));
    CHECK(near(rumbleAt(0.0, 1.0, true).rumble, 0.0, 1e-6));
    CHECK(near(rumbleAt(15.0, 0.0).rumble, 0.0, 1e-6));

    // Rougher and faster are both stronger.
    CHECK(rumbleAt(4.0, 0.3).rumble > 0.0);
    CHECK(rumbleAt(4.0, 0.7).rumble > rumbleAt(4.0, 0.3).rumble);
    CHECK(rumbleAt(6.0, 0.7).rumble > rumbleAt(2.0, 0.7).rumble);
    CHECK(rumbleAt(-6.0, 0.7).rumble == rumbleAt(6.0, 0.7).rumble);
    CHECK(near(rumbleAt(settings.rumbleFullSpeed, 1.0).rumble, settings.rumbleSurface, 1e-6));

    // Working the ground adds to it, up to the cap.
    CHECK(near(rumbleAt(4.0, 0.7, true).rumble, rumbleAt(4.0, 0.7).rumble * (1.0 + settings.rumbleWorkingBoost), 1e-6));
    CHECK(rumbleAt(30.0, 1.0, true).rumble <= settings.rumbleMax + 1e-12);

    // Frequency climbs with speed and stays inside the band.
    CHECK(near(rumbleAt(0.0, 0.7).rumbleHz, settings.rumbleMinHz, 1e-6));
    CHECK(rumbleAt(3.0, 0.7).rumbleHz > settings.rumbleMinHz);
    CHECK(rumbleAt(8.0, 0.7).rumbleHz > rumbleAt(3.0, 0.7).rumbleHz);
    CHECK(near(rumbleAt(settings.rumbleHzFullSpeed, 0.7).rumbleHz, settings.rumbleMaxHz, 1e-6));
    CHECK(near(rumbleAt(60.0, 0.7).rumbleHz, settings.rumbleMaxHz, 1e-6));
}

/** The tractor on @p surface at @p speed, with the roughness the mod reports for that kind of ground. */
Telemetry drivingOn(Surface surface, double speed, double rough)
{
    Telemetry telemetry = driving(speed);
    telemetry.surface   = surface;
    telemetry.rough     = rough;
    telemetry.field     = surface == Surface::Field || surface == Surface::Tilled || surface == Surface::Ridge;
    return telemetry;
}

struct Driven
{
    double lowestTexture  = 1.0;
    double highestTexture = 0.0;
    double lowestKick     = 0.0;
    double highestKick    = 0.0;
    /** Times the texture fell through its midpoint. */
    int         dips = 0;
    ForceOutput last;
};

/** Drives @p telemetry for @p seconds after letting the forces settle, and reports what the texture and the tug did. */
Driven drive(const Telemetry& telemetry, const ForceSettings& settings = {}, double seconds = 20.0, double step = 0.01)
{
    ForceModel model(settings);
    settle(model, &telemetry, 3.0);
    Driven result;
    result.lowestKick = result.highestKick = model.output().rollKick;
    bool high = true;
    for (int i = 0; i < std::lround(seconds / step); ++i)
    {
        const ForceOutput out = model.update(&telemetry, step);
        result.lowestTexture  = std::min(result.lowestTexture, out.rumbleTexture);
        result.highestTexture = std::max(result.highestTexture, out.rumbleTexture);
        result.lowestKick     = std::min(result.lowestKick, out.rollKick);
        result.highestKick    = std::max(result.highestKick, out.rollKick);
        if (high && out.rumbleTexture < 0.5)
        {
            high = false;
            ++result.dips;
        }
        else if (!high && out.rumbleTexture > 0.7)
        {
            high = true;
        }
        result.last = out;
    }
    return result;
}

void testEachSurfaceHasItsOwnRumble()
{
    const ForceSettings settings;

    // A mod that does not say what the ground is, and patterns switched off, both give the plain
    // rumble there was before: steady, across the whole band, with no pull on the wheel.
    Telemetry unnamed = driving(6.0);
    unnamed.rough     = 0.85;
    const Driven plain = drive(unnamed);
    CHECK(plain.lowestTexture == 1.0 && plain.highestTexture == 1.0);
    CHECK(plain.lowestKick == 0.0 && plain.highestKick == 0.0);
    ForceSettings switchedOff;
    switchedOff.surfacePatterns = false;
    const Driven off = drive(drivingOn(Surface::Tilled, 6.0, 0.85), switchedOff);
    CHECK(off.lowestTexture == 1.0 && off.highestTexture == 1.0 && off.lowestKick == 0.0 && off.highestKick == 0.0);
    CHECK(near(off.last.rumble, plain.last.rumble, 1e-9) && near(off.last.rumbleHz, plain.last.rumbleHz, 1e-9));

    // Paved ground gives nothing unless asked to, and then a faint buzz at the fast end of the band.
    CHECK(near(settled(drivingOn(Surface::Road, 15.0, 0.0)).rumble, 0.0, 1e-9));
    ForceSettings buzzing;
    buzzing.surfaceRoad = 1.0;
    const ForceOutput road = settled(drivingOn(Surface::Road, 15.0, 0.0), buzzing);
    CHECK(road.rumble > 0.0 && road.rumble < 0.25 * settings.rumbleSurface);
    CHECK(road.rumbleHz > settings.rumbleMinHz + 0.9 * (settings.rumbleMaxHz - settings.rumbleMinHz));
    CHECK(road.rumbleTexture == 1.0);

    // At one speed, each kind of ground sits at its own pitch: gravel buzzes, a ploughed field thuds.
    const auto hzOn = [](Surface surface) { return settled(drivingOn(surface, 5.0, 0.7)).rumbleHz; };
    CHECK(hzOn(Surface::Gravel) > hzOn(Surface::Field) + 2.0);
    CHECK(hzOn(Surface::Field) > hzOn(Surface::Grass) + 1.0);
    CHECK(hzOn(Surface::Grass) > hzOn(Surface::Snow) + 0.5);
    CHECK(hzOn(Surface::Field) > hzOn(Surface::Tilled) + 2.0);
    CHECK(hzOn(Surface::Field) > hzOn(Surface::Ridge) + 1.0);
    // ... and still rises with speed on every one of them, inside the band.
    for (int i = 1; i < bridge::kSurfaceCount; ++i)
    {
        const auto surface = static_cast<Surface>(i);
        ForceSettings all;
        all.surfaceRoad         = 1.0;
        const ForceOutput slow  = settled(drivingOn(surface, 1.0, 0.7), all);
        const ForceOutput quick = settled(drivingOn(surface, 12.0, 0.7), all);
        CHECK(quick.rumbleHz > slow.rumbleHz);
        CHECK(withinRanges(slow) && withinRanges(quick));
    }

    // The strength per surface multiplies what the roughness gives.
    ForceSettings halfGravel;
    halfGravel.surfaceGravel = 0.5;
    CHECK(near(settled(drivingOn(Surface::Gravel, 6.0, 0.3), halfGravel).rumble, 0.5 * settled(drivingOn(Surface::Gravel, 6.0, 0.3)).rumble, 1e-9));
    CHECK(near(settled(drivingOn(Surface::Grass, 6.0, 0.5)).rumble, settings.surfaceGrass * settled(drivingOn(Surface::Unknown, 6.0, 0.5)).rumble, 1e-9));
    ForceSettings noSnow;
    noSnow.surfaceSnow = 0.0;
    CHECK(near(settled(drivingOn(Surface::Snow, 6.0, 0.4), noSnow).rumble, 0.0, 1e-9));
}

void testRoughGroundComesAndGoesUnderTheWheels()
{
    const ForceSettings settings;

    // A ploughed field is lumpy: the rumble swells and sinks, but never by more than the pattern allows.
    const Driven tilled = drive(drivingOn(Surface::Tilled, 4.0, 1.0));
    CHECK(tilled.highestTexture > 0.9 && tilled.highestTexture <= 1.0);
    CHECK(tilled.lowestTexture < 0.5 && tilled.lowestTexture >= 0.3 - 1e-9);
    // A sown field is nearly even, and gravel is in between.
    const Driven sown   = drive(drivingOn(Surface::Field, 4.0, 0.7));
    const Driven gravel = drive(drivingOn(Surface::Gravel, 4.0, 0.3));
    CHECK(sown.lowestTexture >= 0.8 - 1e-9 && sown.lowestTexture < 0.95);
    CHECK(gravel.lowestTexture >= 0.65 - 1e-9 && gravel.lowestTexture < sown.lowestTexture);

    // Ridges pass one by one, 0.75 m apart: at 1.5 m/s that is two a second.
    const Driven ridges = drive(drivingOn(Surface::Ridge, 1.5, 0.9), settings, 20.0);
    CHECK(ridges.dips >= 38 && ridges.dips <= 42);
    // Twice as fast, twice as many; and at road speed they stop short of a blur.
    CHECK(drive(drivingOn(Surface::Ridge, 3.0, 0.9), settings, 10.0).dips >= 38);
    const Driven flatOut = drive(drivingOn(Surface::Ridge, 15.0, 0.9), settings, 10.0);
    CHECK(flatOut.dips >= 55 && flatOut.dips <= 62);

    // The texture belongs to the ground. With the engine supplying half the rumble it is half as deep,
    // and standing still on the roughest field, engine running, there is none of it.
    ForceSettings withEngine;
    withEngine.engineRumble = 0.2;
    withEngine.rumbleMax    = 1.0;
    Telemetry loaded        = drivingOn(Surface::Tilled, 4.0, 1.0);
    loaded.load             = 1.0;
    ForceModel   probe(withEngine);
    const double surfaceShare = (settle(probe, &loaded).rumble - withEngine.engineRumble) / probe.output().rumble;
    CHECK(surfaceShare > 0.2 && surfaceShare < 0.8);
    const Driven shared = drive(loaded, withEngine);
    CHECK(near(1.0 - shared.lowestTexture, surfaceShare * (1.0 - tilled.lowestTexture), 0.02));
    const Driven parked = drive(drivingOn(Surface::Tilled, 0.0, 1.0), withEngine);
    CHECK(parked.last.rumble > 0.0 && parked.lowestTexture == 1.0 && parked.highestTexture == 1.0);

    // However the time is cut up, the same ground has passed.
    ForceModel fine;
    ForceModel coarse;
    const Telemetry field = drivingOn(Surface::Tilled, 4.0, 1.0);
    for (int i = 0; i < 500; ++i)
    {
        fine.update(&field, 0.01);
    }
    for (int i = 0; i < 100; ++i)
    {
        coarse.update(&field, 0.05);
    }
    CHECK(near(fine.output().rumbleTexture, coarse.output().rumbleTexture, 1e-6));
    CHECK(near(fine.output().rollKick, coarse.output().rollKick, 1e-3));

    // Paused, the ground stops passing.
    const double before = fine.output().rumbleTexture;
    fine.update(nullptr, 0.05);
    Telemetry onFoot = field;
    onFoot.active    = false;
    fine.update(&onFoot, 0.05);
    fine.update(&field, 1e-9);
    CHECK(near(fine.output().rumbleTexture, before, 1e-6));
}

void testBrokenGroundTugsAtTheWheel()
{
    const ForceSettings settings;
    const double        lightness = settings.bumpKickMassHalf / (settings.bumpKickMassHalf + 8200.0);

    // On a ploughed field the wheel is pulled now one way, now the other, within what the setting allows.
    const Driven tilled = drive(drivingOn(Surface::Tilled, 4.0, 1.0), settings, 60.0);
    const double limit  = settings.fieldTug * lightness;
    CHECK(tilled.highestKick > 0.3 * limit && tilled.highestKick <= limit + 1e-9);
    CHECK(tilled.lowestKick < -0.3 * limit && tilled.lowestKick >= -limit - 1e-9);

    // A sown field gives a share of it; paved ground, gravel, grass and snow none.
    const Driven sown = drive(drivingOn(Surface::Field, 4.0, 0.7), settings, 60.0);
    CHECK(sown.highestKick > 0.0 && sown.highestKick <= 0.3 * limit + 1e-9);
    for (const Surface smooth : {Surface::Road, Surface::Gravel, Surface::Grass, Surface::Snow})
    {
        const Driven none = drive(drivingOn(smooth, 4.0, 0.5));
        CHECK(none.lowestKick == 0.0 && none.highestKick == 0.0);
    }

    // None at a standstill, none when switched off, and less for a heavier machine.
    const Driven parked = drive(drivingOn(Surface::Tilled, 0.0, 1.0));
    CHECK(parked.lowestKick == 0.0 && parked.highestKick == 0.0);
    ForceSettings noTug;
    noTug.fieldTug = 0.0;
    const Driven without = drive(drivingOn(Surface::Tilled, 4.0, 1.0), noTug);
    CHECK(without.lowestKick == 0.0 && without.highestKick == 0.0);
    Telemetry heavy = drivingOn(Surface::Tilled, 4.0, 1.0);
    heavy.mass      = 30000.0;
    const Driven big = drive(heavy, settings, 60.0);
    CHECK(big.highestKick < 0.5 * tilled.highestKick && big.highestKick > 0.0);

    // It comes on top of a real bump's kick rather than replacing it.
    Telemetry hit  = drivingOn(Surface::Tilled, 4.0, 1.0);
    hit.bumpRight  = 1.0;
    const Driven bumped = drive(hit, settings, 60.0);
    CHECK(bumped.lowestKick >= settings.bumpKick * lightness - limit - 1e-9);
    CHECK(bumped.highestKick <= settings.bumpKick * lightness + limit + 1e-9);
    CHECK(bumped.highestKick > settings.bumpKick * lightness);
}

void testCrossingOntoNewGroundBlends()
{
    const ForceSettings settings;
    const Telemetry     track = drivingOn(Surface::Gravel, 5.0, 0.3);
    const Telemetry     field = drivingOn(Surface::Tilled, 5.0, 1.0);

    ForceModel model;
    CHECK(model.surface() == Surface::Unknown);
    settle(model, &track, 5.0);
    CHECK(model.surface() == Surface::Gravel);
    const ForceOutput onTrack = model.output();

    // One step onto the field changes very little: no jump in strength, pitch or texture.
    const ForceOutput first = model.update(&field, 0.016);
    CHECK(std::abs(first.rumble - onTrack.rumble) < 0.02);
    CHECK(std::abs(first.rumbleHz - onTrack.rumbleHz) < 1.0);
    CHECK(std::abs(first.rumbleTexture - onTrack.rumbleTexture) < 0.1);
    CHECK(std::abs(first.rollKick) < 0.1 * settings.fieldTug);
    CHECK(model.surface() == Surface::Gravel);

    // The field has taken over within about a second, and after a few it is all there is.
    double hzAfterOneSecond = 0.0;
    for (int i = 0; i < 300; ++i)
    {
        const ForceOutput out = model.update(&field, 0.016);
        if (i == 62)
        {
            hzAfterOneSecond = out.rumbleHz;
            CHECK(model.surface() == Surface::Tilled);
        }
    }
    const double settledHz = settled(field).rumbleHz;
    CHECK(std::abs(hzAfterOneSecond - settledHz) < 0.15 * std::abs(onTrack.rumbleHz - settledHz));
    CHECK(near(model.output().rumbleHz, settledHz, 0.05));

    // Turning the patterns off mid-drive goes back to the plain rumble the same gentle way.
    ForceSettings plain   = settings;
    plain.surfacePatterns = false;
    model.setSettings(plain);
    const ForceOutput next = model.update(&field, 0.016);
    CHECK(std::abs(next.rumbleHz - model.output().rumbleHz) < 1.0);
    settle(model, &field, 5.0);
    CHECK(model.surface() == Surface::Unknown);
    CHECK(near(model.output().rumbleTexture, 1.0, 1e-6) && near(model.output().rollKick, 0.0, 1e-6));
}

void testEngineRumbleIsOptIn()
{
    // Off by default, whatever the engine is doing.
    Telemetry revving = driving(0.0);
    revving.rpm       = 2200.0;
    revving.load      = 1.0;
    CHECK(near(settled(revving).rumble, 0.0, 1e-6));

    ForceSettings settings;
    settings.engineRumble = 0.10;

    Telemetry idling = driving(0.0);
    idling.rpm       = 900.0;
    idling.load      = 0.0;
    const ForceOutput idle = settled(idling, settings);
    CHECK(near(idle.rumble, 0.10 * settings.engineRumbleIdleShare, 1e-6));
    CHECK(near(idle.rumbleHz, 15.0, 1e-6)); // 900 rpm is 15 turns a second

    const ForceOutput loaded = settled(revving, settings);
    CHECK(near(loaded.rumble, 0.10, 1e-6));
    CHECK(near(loaded.rumbleHz, settings.rumbleMaxHz, 1e-6)); // 36.7 Hz, held to the band

    Telemetry stalled = driving(0.0);
    stalled.rpm       = 0.0;
    CHECK(near(settled(stalled, settings).rumble, 0.0, 1e-6));

    // With surface rumble as well, the two add and the frequency lies between their own.
    Telemetry rolling = driving(8.0);
    rolling.rough     = 1.0;
    rolling.rpm       = 900.0;
    rolling.load      = 0.0;
    const ForceOutput both = settled(rolling, settings);
    CHECK(near(both.rumble, settings.rumbleSurface + 0.05, 1e-6));
    CHECK(both.rumbleHz > 15.0 && both.rumbleHz < settled(rolling).rumbleHz);
}

/** Feeds @p telemetry for @p seconds in 10 ms steps and returns the strongest rumble seen. */
double peakRumble(ForceModel& model, const Telemetry& telemetry, double seconds)
{
    double peak = 0.0;
    for (int i = 0; i < std::lround(seconds / 0.01); ++i)
    {
        peak = std::max(peak, model.update(&telemetry, 0.01).rumble);
    }
    return peak;
}

void testEngineStartShakes()
{
    ForceSettings settings;
    settings.engineRumble = 0.10;

    Telemetry off = driving(0.0);
    off.rpm       = 0.0;
    off.load      = 0.0;
    Telemetry cranking = off;
    cranking.starting  = true;
    Telemetry idling   = off;
    idling.rpm         = 850.0;

    ForceModel model(settings);
    CHECK(near(settle(model, &off).rumble, 0.0, 1e-6));

    // While the start lasts, the shake holds at its own strength and its own slow frequency.
    const ForceOutput started = settle(model, &cranking, 2.0);
    CHECK(near(started.rumble, settings.engineStartShake, 1e-3));
    CHECK(near(started.rumbleHz, settings.engineStartHz, 0.05));
    CHECK(started.rumble > 3.0 * settings.engineRumble);

    // Once the engine runs, the shake dies away to the ordinary idle rumble rather than cutting off.
    const double justAfter = model.update(&idling, 0.01).rumble;
    CHECK(justAfter > 0.5 * settings.engineStartShake);
    const ForceOutput running = settle(model, &idling, 5.0);
    CHECK(near(running.rumble, settings.engineRumble * settings.engineRumbleIdleShare, 1e-4));
    CHECK(near(running.rumbleHz, 850.0 / 60.0, 0.05));

    // It stands out even with running vibration switched off and the rumble limit turned right down.
    ForceSettings quiet;
    quiet.rumbleMax = 0.0;
    ForceModel capped(quiet);
    CHECK(near(settle(capped, &cranking, 2.0).rumble, quiet.engineStartShake, 1e-3));
    CHECK(near(settle(capped, &idling, 5.0).rumble, 0.0, 1e-4));

    // Turned off, a start is no different from an engine that is simply off.
    ForceSettings none;
    none.engineStartShake = 0.0;
    ForceModel silent(none);
    CHECK(near(peakRumble(silent, cranking, 2.0), 0.0, 1e-9));

    // Leaving the vehicle mid-start drops it; it does not come back on re-entry.
    ForceModel   left(settings);
    Telemetry    onFoot;
    onFoot.seq = 2;
    settle(left, &cranking, 2.0);
    settle(left, &onFoot, 3.0);
    CHECK(near(peakRumble(left, off, 1.0), 0.0, 1e-6));

    // Hostile values stay in range.
    ForceSettings wild;
    wild.engineStartShake       = 50.0;
    wild.engineStartHz          = -3.0;
    wild.engineStartFadeSeconds = -1.0;
    ForceModel hostile(wild);
    CHECK(withinRanges(settle(hostile, &cranking, 2.0), wild));
    CHECK(withinRanges(settle(hostile, &idling, 1.0), wild));
}

void testElectricVehiclesHaveNoEngineVibration()
{
    ForceSettings settings;
    settings.engineRumble = 0.30;

    // Parked with the motor on: a diesel rumbles, an electric vehicle is still.
    Telemetry diesel = driving(0.0);
    diesel.rpm       = 900.0;
    Telemetry electric = diesel;
    electric.electric  = true;
    CHECK(settled(diesel, settings).rumble > 0.1);
    CHECK(near(settled(electric, settings).rumble, 0.0, 1e-6));

    // A start flag on an electric vehicle, which the mod should never send, still shakes nothing.
    Telemetry bogus = electric;
    bogus.starting  = true;
    ForceModel model(settings);
    CHECK(near(peakRumble(model, bogus, 2.0), 0.0, 1e-9));

    // Moving over rough ground it rumbles exactly as much as the ground alone gives a diesel.
    ForceSettings groundOnly;
    Telemetry     dieselOnField = driving(8.0);
    dieselOnField.rough         = 1.0;
    Telemetry electricOnField   = dieselOnField;
    electricOnField.electric    = true;
    const ForceOutput ground    = settled(dieselOnField, groundOnly);
    const ForceOutput quietRide = settled(electricOnField, settings);
    CHECK(ground.rumble > 0.1);
    CHECK(near(quietRide.rumble, ground.rumble, 1e-6));
    CHECK(near(quietRide.rumbleHz, ground.rumbleHz, 1e-6));
    CHECK(settled(dieselOnField, settings).rumble > quietRide.rumble);

    // Everything that is not vibration is the same for both.
    const ForceOutput a = settled(dieselOnField, settings);
    CHECK(near(a.rollSpring, quietRide.rollSpring) && near(a.rollDamper, quietRide.rollDamper) && near(a.rollForce, quietRide.rollForce));
}

void testAWheelHittingSomethingKicksTowardThatSide()
{
    // Mass scaling is checked separately below; here the kick is at its nominal strength.
    ForceSettings settings;
    settings.bumpKickMassHalf = 0.0;

    Telemetry rightHit = driving(3.0);
    rightHit.bumpRight = 1.0;
    Telemetry leftHit = driving(3.0);
    leftHit.bumpLeft  = 1.0;

    // Positive is toward steering right, the same convention as the pull.
    CHECK(near(settled(rightHit, settings).rollKick, settings.bumpKick, 1e-6));
    CHECK(near(settled(leftHit, settings).rollKick, -settings.bumpKick, 1e-6));
    CHECK(near(settled(driving(3.0), settings).rollKick, 0.0, 1e-9));

    // Both wheels at once is a thump, not a yank: nothing sideways.
    Telemetry both = rightHit;
    both.bumpLeft  = 1.0;
    CHECK(near(settled(both, settings).rollKick, 0.0, 1e-9));

    // A half-strength hit kicks half as hard, and the kick leaves the steady pull alone.
    Telemetry half = driving(3.0, 0.4);
    half.bumpRight = 0.5;
    CHECK(near(settled(half, settings).rollKick, 0.5 * settings.bumpKick, 1e-6));
    CHECK(near(settled(half, settings).rollForce, settled(driving(3.0, 0.4), settings).rollForce, 1e-9));

    // It is a jolt: nearly all of it arrives within 50 ms, and it is gone as fast when the bump ends.
    ForceModel model(settings);
    Telemetry  rolling = driving(3.0);
    settle(model, &rolling);
    for (int i = 0; i < 5; ++i)
    {
        model.update(&rightHit, 0.01);
    }
    CHECK(model.output().rollKick > 0.9 * settings.bumpKick);
    for (int i = 0; i < 5; ++i)
    {
        model.update(&rolling, 0.01);
    }
    CHECK(model.output().rollKick < 0.1 * settings.bumpKick);

    // It may exceed the cap on the steady pull, but not the device's range, whatever comes in.
    ForceSettings strong = settings;
    strong.bumpKick       = 1.0;
    strong.rollForceLimit = 0.1;
    CHECK(near(settled(rightHit, strong).rollKick, 1.0, 1e-6));
    Telemetry garbage = driving(3.0);
    garbage.bumpRight = 1e9;
    garbage.bumpLeft  = std::nan("");
    garbage.mass      = std::nan("");
    const ForceOutput out = settled(garbage, strong);
    CHECK(out.rollKick >= -1.0 && out.rollKick <= 1.0 && std::isfinite(out.rollKick));

    // On foot or without telemetry there is nothing to kick.
    Telemetry onFoot;
    onFoot.seq       = 3;
    onFoot.bumpRight = 1.0;
    CHECK(near(settled(onFoot, settings).rollKick, 0.0, 1e-9));
}

void testBothWheelsTogetherShakeInsteadOfKicking()
{
    ForceSettings settings;
    settings.bumpKickMassHalf = 0.0;

    // Rail tracks met square on: both wheels, the same amount.
    Telemetry tracks = driving(3.0);
    tracks.bumpLeft  = 0.8;
    tracks.bumpRight = 0.8;
    const ForceOutput crossing = settled(tracks, settings);
    CHECK(near(crossing.rollKick, 0.0, 1e-9));
    CHECK(near(crossing.pitchThump, 0.8 * settings.bumpThump, 1e-6));
    CHECK(near(crossing.rollWiggle, 0.8 * settings.bumpWiggle, 1e-6));
    CHECK(near(crossing.bumpHz, settings.bumpShakeHz, 1e-9));

    // A log under one wheel only is all kick and no shake.
    Telemetry log  = driving(3.0);
    log.bumpRight  = 0.8;
    const ForceOutput oneSided = settled(log, settings);
    CHECK(oneSided.rollKick > 0.0);
    CHECK(near(oneSided.pitchThump, 0.0, 1e-9));
    CHECK(near(oneSided.rollWiggle, 0.0, 1e-9));

    // Tracks met at an angle: the shared part shakes, the difference kicks toward the harder hit.
    Telemetry skewed = driving(3.0);
    skewed.bumpLeft  = 0.3;
    skewed.bumpRight = 0.9;
    const ForceOutput angled = settled(skewed, settings);
    CHECK(near(angled.pitchThump, 0.3 * settings.bumpThump, 1e-6));
    CHECK(near(angled.rollKick, 0.6 * settings.bumpKick, 1e-6));

    // The shakes are as quick as the kick, and nothing is left on smooth ground or on foot.
    ForceModel model(settings);
    Telemetry  rolling = driving(3.0);
    settle(model, &rolling);
    for (int i = 0; i < 5; ++i)
    {
        model.update(&tracks, 0.01);
    }
    CHECK(model.output().pitchThump > 0.9 * 0.8 * settings.bumpThump);
    for (int i = 0; i < 5; ++i)
    {
        model.update(&rolling, 0.01);
    }
    CHECK(model.output().pitchThump < 0.1 * settings.bumpThump && model.output().rollWiggle < 0.1 * settings.bumpWiggle);
    Telemetry onFoot;
    onFoot.seq       = 4;
    onFoot.bumpLeft  = 1.0;
    onFoot.bumpRight = 1.0;
    CHECK(near(settled(onFoot, settings).pitchThump, 0.0, 1e-9) && near(settled(onFoot, settings).rollWiggle, 0.0, 1e-9));

    // The wiggle shrinks with mass like the kick; the thump is the whole cab moving and does not.
    const ForceSettings scaled;
    Telemetry           heavy = tracks;
    heavy.mass                = 30000.0;
    Telemetry light           = tracks;
    light.mass                = 2500.0;
    CHECK(settled(heavy, scaled).rollWiggle < 0.5 * settled(light, scaled).rollWiggle);
    CHECK(near(settled(heavy, scaled).pitchThump, settled(light, scaled).pitchThump, 1e-9));

    // Hostile settings and values stay in range.
    ForceSettings wild;
    wild.bumpThump   = 9.0;
    wild.bumpWiggle  = -4.0;
    wild.bumpShakeHz = 0.0;
    Telemetry garbage = tracks;
    garbage.bumpLeft  = 1e12;
    garbage.bumpRight = 1e12;
    const ForceOutput out = settled(garbage, wild);
    CHECK(out.pitchThump >= 0.0 && out.pitchThump <= 1.0 && out.rollWiggle >= 0.0 && out.rollWiggle <= 1.0);
    CHECK(out.bumpHz >= 2.0 && out.bumpHz <= 40.0);
}

void testTheKickShrinksAsTheMachineGetsHeavier()
{
    const ForceSettings settings;

    const auto kickAt = [&](double mass) {
        Telemetry telemetry = driving(3.0);
        telemetry.mass      = mass;
        telemetry.bumpRight = 1.0;
        return settled(telemetry, settings).rollKick;
    };

    // Half at the configured mass, nearly all of it for something very light, little for a giant.
    CHECK(near(kickAt(settings.bumpKickMassHalf), 0.5 * settings.bumpKick, 1e-6));
    CHECK(kickAt(300.0) > 0.9 * settings.bumpKick);
    CHECK(kickAt(40000.0) < 0.2 * settings.bumpKick);

    // Strictly less with every step up in size: skid steer, small tractor, big tractor, with a loaded trailer.
    double previous = settings.bumpKick + 1.0;
    for (const double mass : {2500.0, 5000.0, 11000.0, 30000.0})
    {
        const double kick = kickAt(mass);
        CHECK(kick > 0.0 && kick < previous);
        previous = kick;
    }

    // Both directions scale alike.
    Telemetry leftHit = driving(3.0);
    leftHit.mass      = 11000.0;
    leftHit.bumpLeft  = 1.0;
    CHECK(near(settled(leftHit, settings).rollKick, -kickAt(11000.0), 1e-9));
}

/** Feeds the model a vehicle whose speed changes at @p accel m/s^2 for @p seconds, as the game would write it at 60 Hz. */
ForceOutput accelerate(ForceModel& model, double fromSpeed, double accel, double seconds, std::uint64_t& seq, std::int64_t& timeMs)
{
    Telemetry telemetry = driving(fromSpeed);
    for (int frame = 0; frame < std::lround(seconds * 60.0); ++frame)
    {
        telemetry.seq    = ++seq;
        telemetry.timeMs = (timeMs += 17);
        // Two decimals, as the mod writes it.
        telemetry.speed = std::round((fromSpeed + accel * (frame + 1) * 0.017) * 100.0) / 100.0;
        // The bridge updates faster than the game writes: the same sample is seen more than once.
        model.update(&telemetry, 0.008);
        model.update(&telemetry, 0.009);
    }
    return model.output();
}

void testPitchLeansWithAcceleration()
{
    const ForceSettings settings;
    std::uint64_t       seq    = 100;
    std::int64_t        timeMs = 50000;

    // Steady speed: no lean, however fast.
    ForceModel cruising(settings);
    CHECK(std::abs(accelerate(cruising, 8.0, 0.0, 3.0, seq, timeMs).pitchTarget) < 0.005);
    CHECK(std::abs(cruising.acceleration()) < 0.05);

    // Pulling away at 1.5 m/s^2: the estimate finds it despite the coarse speed steps, and pitch leans in proportion.
    ForceModel pulling(settings);
    const ForceOutput away = accelerate(pulling, 0.0, 1.5, 3.0, seq, timeMs);
    CHECK(near(pulling.acceleration(), 1.5, 0.15));
    CHECK(near(away.pitchTarget, settings.accelSway * 1.5 / settings.accelSwayFullAccel, 0.01));
    CHECK(away.pitchTarget > 0.0);

    // Braking leans the other way, and no further than the full sway however hard.
    ForceModel braking(settings);
    accelerate(braking, 12.0, 0.0, 1.0, seq, timeMs);
    const ForceOutput stop = accelerate(braking, 12.0, -8.0, 1.5, seq, timeMs);
    CHECK(stop.pitchTarget < 0.0);
    CHECK(stop.pitchTarget >= -settings.accelSway - 1e-9);
    CHECK(near(stop.pitchTarget, -settings.accelSway, 0.01));

    // Back at a steady speed it returns to center on its own.
    CHECK(std::abs(accelerate(braking, 0.0, 0.0, 4.0, seq, timeMs).pitchTarget) < 0.005);

    // It moves as a lean: never faster than its slew limit, even when the acceleration appears at once.
    ForceModel sudden(settings);
    accelerate(sudden, 5.0, 0.0, 1.0, seq, timeMs);
    double    previous  = sudden.output().pitchTarget;
    double    fastest   = 0.0;
    Telemetry telemetry = driving(5.0);
    for (int frame = 0; frame < 120; ++frame)
    {
        telemetry.seq    = ++seq;
        telemetry.timeMs = (timeMs += 17);
        telemetry.speed  = 5.0 + 6.0 * (frame + 1) * 0.017;
        const double now = sudden.update(&telemetry, 0.017).pitchTarget;
        fastest          = std::max(fastest, std::abs(now - previous) / 0.017);
        previous         = now;
    }
    CHECK(fastest <= settings.swaySmoothing.perSecond + 1e-6);

    // Switching to another vehicle is a jump in speed, not an acceleration: nothing leans.
    ForceModel switching(settings);
    accelerate(switching, 0.0, 0.0, 1.0, seq, timeMs);
    Telemetry other = driving(15.0);
    other.mass      = 3100.0;
    double peak     = 0.0;
    for (int frame = 0; frame < 120; ++frame)
    {
        other.seq    = ++seq;
        other.timeMs = (timeMs += 17);
        peak         = std::max(peak, std::abs(switching.update(&other, 0.017).pitchTarget));
    }
    CHECK(peak < 0.005);

    // So is the game resuming after a pause, and leaving the vehicle clears it.
    ForceModel paused(settings);
    accelerate(paused, 2.0, 0.0, 1.0, seq, timeMs);
    Telemetry resumed = driving(9.0);
    resumed.seq       = ++seq;
    resumed.timeMs    = (timeMs += 30000);
    paused.update(&resumed, 0.017);
    CHECK(std::abs(paused.acceleration()) < 1e-9);
    Telemetry onFoot;
    onFoot.seq = ++seq;
    accelerate(pulling, 4.5, 1.5, 0.5, seq, timeMs);
    settle(pulling, &onFoot, 3.0);
    CHECK(std::abs(pulling.output().pitchTarget) < 1e-6 && pulling.acceleration() == 0.0);

    // Off means off, and hostile numbers stay in range.
    ForceSettings none;
    none.accelSway = 0.0;
    ForceModel off(none);
    CHECK(std::abs(accelerate(off, 0.0, 2.0, 2.0, seq, timeMs).pitchTarget) < 1e-12);
    ForceSettings wild;
    wild.accelSway          = 40.0;
    wild.accelSwayFullAccel = -1.0;
    wild.accelFilterSeconds = -3.0;
    ForceModel hostile(wild);
    Telemetry  garbage = driving(1e9);
    for (int frame = 0; frame < 60; ++frame)
    {
        garbage.seq    = ++seq;
        garbage.timeMs = (timeMs += 17);
        garbage.speed  = frame % 2 ? 1e9 : -1e9;
        const double target = hostile.update(&garbage, 0.017).pitchTarget;
        CHECK(std::isfinite(target) && target >= -1.0 && target <= 1.0);
    }
}

/** A pitch axis with the AY210's character: it does not move until pushed harder than its friction. */
struct StickyAxis
{
    double position = 0.0;
    double speed    = 0.0;

    void step(double push, double dt)
    {
        const double stiction = 0.37; // share of full force needed to break free, as measured
        const double sliding  = 0.30;
        if (std::abs(speed) < 1e-4 && std::abs(push) <= stiction)
        {
            speed = 0.0;
            return;
        }
        const double drag  = (std::abs(speed) < 1e-4 ? (push > 0 ? 1.0 : -1.0) : (speed > 0 ? 1.0 : -1.0)) * sliding;
        const double accel = 25.0 * (push - drag) - 10.0 * speed;
        const double next  = speed + accel * dt;
        // Friction stops the axis rather than reversing it.
        speed = (speed != 0.0 && next * speed < 0.0) ? 0.0 : next;
        position = std::clamp(position + speed * dt, -1.0, 1.0);
    }
};

void testPitchServoMovesAStickyAxisWithoutHunting()
{
    const bridge::PitchServoSettings settings;
    const double                     dt = 0.016;

    bridge::PitchServo servo(settings);
    StickyAxis         axis;

    // Nothing to do at the target.
    for (int i = 0; i < 100; ++i)
    {
        axis.step(servo.update(0.0, axis.position, dt), dt);
    }
    CHECK(axis.position == 0.0 && servo.push() == 0.0);

    // A small lean: it gets there, stops within the deadband, and then leaves the axis alone.
    double peakPush = 0.0;
    double fastestChange = 0.0;
    double previousPush  = 0.0;
    for (int i = 0; i < 250; ++i)
    {
        const double push = servo.update(0.12, axis.position, dt);
        peakPush          = std::max(peakPush, std::abs(push));
        fastestChange     = std::max(fastestChange, std::abs(push - previousPush) / dt);
        previousPush      = push;
        axis.step(push, dt);
    }
    CHECK(std::abs(axis.position - 0.12) <= settings.deadband + 0.02);
    CHECK(peakPush <= settings.maxPush + 1e-9);
    CHECK(fastestChange <= settings.slewPerSecond + 1e-6);

    int    reversals = 0;
    double lastSign  = 0.0;
    double moved     = 0.0;
    double resting   = axis.position;
    for (int i = 0; i < 250; ++i)
    {
        const double push = servo.update(0.12, axis.position, dt);
        if (push != 0.0 && lastSign != 0.0 && (push > 0) != (lastSign > 0))
        {
            ++reversals;
        }
        lastSign = push != 0.0 ? push : lastSign;
        axis.step(push, dt);
        moved = std::max(moved, std::abs(axis.position - resting));
    }
    CHECK(reversals == 0);
    CHECK(moved < 0.01);

    // Back to center, and over to the other side.
    for (int i = 0; i < 300; ++i)
    {
        axis.step(servo.update(0.0, axis.position, dt), dt);
    }
    CHECK(std::abs(axis.position) <= settings.deadband + 0.02);
    for (int i = 0; i < 300; ++i)
    {
        axis.step(servo.update(-0.10, axis.position, dt), dt);
    }
    CHECK(std::abs(axis.position + 0.10) <= settings.deadband + 0.02);

    // An axis that cannot move at all gets the maximum push and no more; reset lets go at once.
    bridge::PitchServo blocked(settings);
    for (int i = 0; i < 300; ++i)
    {
        blocked.update(0.3, 0.0, dt);
    }
    CHECK(near(blocked.push(), settings.maxPush, 1e-9));
    blocked.reset();
    CHECK(blocked.push() == 0.0);

    // Nonsense in, nothing new out.
    CHECK(servo.update(std::nan(""), 0.0, dt) == servo.push());
    CHECK(servo.update(0.1, 0.0, 0.0) == servo.push());
}

void testPitchIsHeldRegardlessOfTelemetry()
{
    const ForceSettings settings;
    for (const Telemetry& telemetry : {driving(0.0), driving(15.0, 1.0), driving(-4.0, -0.5), workingField(3.0, 0.3)})
    {
        const ForceOutput out = settled(telemetry);
        CHECK(near(out.pitchSpring, settings.pitchSpring, 1e-6));
        CHECK(near(out.pitchDamper, settings.pitchDamper, 1e-6));
    }

    ForceSettings firm;
    firm.pitchSpring = 1.0;
    firm.pitchDamper = 0.5;
    CHECK(near(settled(driving(5.0), firm).pitchSpring, 1.0, 1e-6));
    CHECK(near(settled(driving(5.0), firm).pitchDamper, 0.5, 1e-6));
}

void testFadesToIdle()
{
    const ForceSettings settings;
    const auto          isIdle = [&](const ForceOutput& out) {
        return near(out.rollSpring, settings.idleSpring, 1e-6) && near(out.rollDamper, settings.idleDamper, 1e-6) &&
               near(out.pitchSpring, settings.idleSpring, 1e-6) && near(out.pitchDamper, settings.idleDamper, 1e-6) &&
               near(out.rollForce, 0.0, 1e-6) && near(out.rumble, 0.0, 1e-6);
    };

    // No telemetry at all, from start-up.
    ForceModel never;
    CHECK(isIdle(settle(never, nullptr)));

    // On foot: active=0, even if the rest of the sample still describes a moving vehicle.
    Telemetry onFoot = workingField(12.0, 1.0);
    onFoot.active    = false;
    CHECK(isIdle(settled(onFoot)));

    // From driving hard to no fresh sample: a fade, each output moving one way toward idle, and
    // the rumble keeping its frequency while it dies away instead of sliding down the band.
    const Telemetry busy = workingField(12.0, 1.0);
    ForceModel      model;
    const auto      before = settle(model, &busy);
    CHECK(before.rumble > 0.1 && before.rollForce < -0.1 && before.rollSpring > 0.5);

    ForceOutput previous = before;
    for (int i = 0; i < 300; ++i)
    {
        const ForceOutput out = model.update(nullptr, 0.01);
        CHECK(out.rollSpring <= previous.rollSpring && out.rollSpring >= settings.idleSpring - 1e-12);
        CHECK(out.pitchSpring <= previous.pitchSpring && out.pitchSpring >= settings.idleSpring - 1e-12);
        CHECK(out.rollForce >= previous.rollForce && out.rollForce <= 0.0);
        CHECK(out.rumble <= previous.rumble && out.rumble >= 0.0);
        CHECK(out.rumbleHz == before.rumbleHz);
        previous = out;
    }
    // The first frame of the fade is nearly where it started.
    ForceModel again;
    settle(again, &busy);
    CHECK(largestDifference(again.update(nullptr, 0.01), before) < 0.03);
    CHECK(isIdle(settle(model, nullptr)));
}

void testTargetIsWhereTheOutputsSettle()
{
    const Telemetry telemetry = workingField(5.0, -0.4);
    ForceModel      model;
    const auto      goal = model.target(&telemetry);
    CHECK(largestDifference(settle(model, &telemetry), goal) < 1e-6);
    // Asking for the target does not advance anything.
    const ForceOutput held = model.output();
    model.target(nullptr);
    CHECK(largestDifference(model.output(), held) == 0.0);
}

void testDegenerateTimeSteps()
{
    const Telemetry telemetry = workingField(12.0, 1.0);
    ForceModel      model;
    settle(model, nullptr);
    const ForceOutput idle = model.output();

    // No time, negative time and NaN time all change nothing.
    CHECK(largestDifference(model.update(&telemetry, 0.0), idle) == 0.0);
    CHECK(largestDifference(model.update(&telemetry, -1.0), idle) == 0.0);
    CHECK(largestDifference(model.update(&telemetry, std::numeric_limits<double>::quiet_NaN()), idle) == 0.0);
    CHECK(largestDifference(model.update(&telemetry, -std::numeric_limits<double>::infinity()), idle) == 0.0);

    // A tiny step makes a tiny change, and a great many of them add up to the same as one step.
    const ForceOutput nudged = model.update(&telemetry, 1e-9);
    CHECK(largestDifference(nudged, idle) > 0.0);
    CHECK(largestDifference(nudged, idle) < 1e-7);

    ForceModel fine;
    ForceModel coarse;
    settle(fine, nullptr);
    settle(coarse, nullptr);
    for (int i = 0; i < 20000; ++i)
    {
        fine.update(&telemetry, 1e-5);
    }
    coarse.update(&telemetry, 0.1);
    coarse.update(&telemetry, 0.1);
    CHECK(largestDifference(fine.output(), coarse.output()) < 1e-6);
}

void testAStallDoesNotBecomeAJump()
{
    const ForceSettings settings;
    const Telemetry     telemetry = workingField(12.0, 1.0);
    const ForceOutput   goal      = settled(telemetry);

    // The process hung for half a minute while the player drove off; the next update may only
    // move as far as maxStepSeconds allows. An infinite dt is held to the same.
    for (const double stall : {30.0, 1.0e12, std::numeric_limits<double>::infinity()})
    {
        ForceModel model;
        settle(model, nullptr);
        const ForceOutput idle  = model.output();
        const ForceOutput after = model.update(&telemetry, stall);
        CHECK(withinRanges(after));
        CHECK(std::abs(after.rollSpring - idle.rollSpring) <= settings.springSmoothing.perSecond * settings.maxStepSeconds + 1e-12);
        CHECK(std::abs(after.rollForce - idle.rollForce) <= settings.forceSmoothing.perSecond * settings.maxStepSeconds + 1e-12);
        CHECK(std::abs(after.rumble - idle.rumble) <= settings.rumbleSmoothing.perSecond * settings.maxStepSeconds + 1e-12);
        CHECK(std::abs(after.rollSpring - goal.rollSpring) > 0.1);

        // It is exactly what a single update of maxStepSeconds would have done.
        ForceModel reference;
        settle(reference, nullptr);
        CHECK(largestDifference(reference.update(&telemetry, settings.maxStepSeconds), after) < 1e-12);
    }
}

/** The outputs @p seconds after telemetry steps from @p before to @p after, integrated in steps of @p dt. */
ForceOutput stepResponse(const Telemetry* before, const Telemetry* after, double dt, double seconds)
{
    ForceModel model;
    settle(model, before);
    for (int i = 0; i < std::lround(seconds / dt); ++i)
    {
        model.update(after, dt);
    }
    return model.output();
}

void testSmoothingIsFrameRateIndependent()
{
    const Telemetry parked = driving(0.0);
    const Telemetry busy   = workingField(12.0, 1.0);
    const Telemetry other  = driving(4.0, -1.0);

    struct Step
    {
        const Telemetry* before;
        const Telemetry* after;
    };
    // Entering a moving vehicle, a dropout, coming back from one, and a change while driving.
    for (const Step& step : {Step{nullptr, &busy}, Step{&busy, nullptr}, Step{&parked, &busy}, Step{&busy, &other}})
    {
        // Sampled part way through the transition, where a step-size dependence would show.
        for (const double seconds : {0.1, 0.25, 0.6})
        {
            const ForceOutput at1ms  = stepResponse(step.before, step.after, 0.001, seconds);
            const ForceOutput at10ms = stepResponse(step.before, step.after, 0.010, seconds);
            const ForceOutput at50ms = stepResponse(step.before, step.after, 0.050, seconds);
            CHECK(largestDifference(at10ms, at50ms) < 1e-6);
            CHECK(largestDifference(at1ms, at50ms) < 1e-6);

            CHECK(largestDifference(at10ms, stepResponse(step.before, step.after, 0.010, 10.0)) > 0.01);
        }
    }

    // Telemetry that keeps changing is sampled at different instants by different step sizes, so
    // the match is only approximate there: an acceleration from rest to 10 m/s over two seconds
    // while the wheel goes from center to full lock, compared one second in.
    const auto accelerate = [](double dt) {
        ForceModel model;
        settle(model, nullptr);
        for (int i = 0; i < std::lround(1.0 / dt); ++i)
        {
            const double    time      = i * dt;
            const Telemetry telemetry = workingField(5.0 * time, 0.5 * time);
            model.update(&telemetry, dt);
        }
        return model.output();
    };
    const ForceOutput slow = accelerate(0.050);
    const ForceOutput fast = accelerate(0.010);
    CHECK(std::abs(slow.rumbleHz - fast.rumbleHz) < 0.5);
    ForceOutput slowUnit = slow;
    slowUnit.rumbleHz    = fast.rumbleHz;
    CHECK(largestDifference(slowUnit, fast) < 0.02);
}

void testNoStepExceedsTheSlewLimit()
{
    const ForceSettings settings;

    // Entering a vehicle that is already at speed with the wheel hard over: one 60 Hz frame later
    // the forces have barely begun to change.
    {
        const Telemetry busy = workingField(12.0, 1.0);
        ForceModel      model;
        const auto      idle  = settle(model, nullptr);
        const auto      first = model.update(&busy, 1.0 / 60.0);
        CHECK(std::abs(first.rollSpring - idle.rollSpring) <= settings.springSmoothing.perSecond / 60.0 + 1e-12);
        CHECK(std::abs(first.rollForce - idle.rollForce) <= settings.forceSmoothing.perSecond / 60.0 + 1e-12);
        CHECK(largestDifference(first, idle) < 0.7); // rumbleHz, in Hz, is the largest mover
    }

    // The worst sequence there is: telemetry jumping between extremes on every update, at step
    // sizes from a fast loop to a long stall.
    Telemetry rightLock = workingField(40.0, 1.0);
    rightLock.rough     = 1.0;
    rightLock.mass      = 60000.0;
    Telemetry leftLock  = rightLock;
    leftLock.steer      = -1.0;
    Telemetry onFoot    = rightLock;
    onFoot.active       = false;
    const Telemetry parked = driving(0.0);

    const Telemetry* const inputs[] = {nullptr, &rightLock, &leftLock, &parked, &onFoot, &rightLock, nullptr, &leftLock};
    const double           steps[]  = {0.001, 0.005, 1.0 / 60.0, 0.02, 0.05, 0.1, 0.2, 5.0};

    ForceModel   model;
    ForceOutput  previous = model.output();
    std::mt19937 random(12345);
    for (int i = 0; i < 20000; ++i)
    {
        const Telemetry* const input = inputs[random() % std::size(inputs)];
        const double           dt    = steps[random() % std::size(steps)];
        const double           limit = std::min(dt, settings.maxStepSeconds);
        const ForceOutput      out   = model.update(input, dt);

        CHECK(withinRanges(out));
        CHECK(std::abs(out.rollSpring - previous.rollSpring) <= settings.springSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.pitchSpring - previous.pitchSpring) <= settings.springSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.rollDamper - previous.rollDamper) <= settings.damperSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.pitchDamper - previous.pitchDamper) <= settings.damperSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.rollForce - previous.rollForce) <= settings.forceSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.rumble - previous.rumble) <= settings.rumbleSmoothing.perSecond * limit + 1e-12);
        CHECK(std::abs(out.rumbleHz - previous.rumbleHz) <= settings.rumbleHzSmoothing.perSecond * limit + 1e-9);
        previous = out;
    }
}

void testSmoothingModes()
{
    const Telemetry telemetry = driving(12.0);

    // Slew limit alone: a straight ramp that stops dead on the target.
    ForceSettings ramp;
    ramp.springSmoothing = {0.0, 1.0};
    ForceModel ramped(ramp);
    CHECK(near(ramped.update(&telemetry, 0.05).rollSpring, 0.05));
    CHECK(near(ramped.update(&telemetry, 0.05).rollSpring, 0.10));
    CHECK(near(settle(ramped, &telemetry, 1.0).rollSpring, ramp.rollSpringMax, 1e-12));

    // Low-pass alone: after one time constant, 1 - 1/e of the way.
    ForceSettings lowPass;
    lowPass.springSmoothing = {0.5, 0.0};
    lowPass.maxStepSeconds  = 0.0;
    ForceModel filtered(lowPass);
    CHECK(near(filtered.update(&telemetry, 0.5).rollSpring, lowPass.rollSpringMax * (1.0 - std::exp(-1.0)), 1e-12));

    // Neither: the output is the target.
    ForceSettings direct;
    direct.springSmoothing = {0.0, 0.0};
    ForceModel immediate(direct);
    CHECK(near(immediate.update(&telemetry, 0.001).rollSpring, direct.rollSpringMax, 1e-12));
}

void testOutputsStayInRangeUnderHostileInput()
{
    const double nan      = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const double values[] = {nan,  infinity, -infinity, std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                             1e30, -1e30,    -1.0,      -0.0,                               std::numeric_limits<double>::denorm_min(),
                             0.5,  7.0,      1e6};
    double Telemetry::* const numbers[] = {&Telemetry::speed, &Telemetry::steer, &Telemetry::mass, &Telemetry::rpm,
                                           &Telemetry::load,  &Telemetry::slip,  &Telemetry::rough};

    ForceSettings loud;
    loud.engineRumble = 0.2;

    for (const ForceSettings& settings : {ForceSettings{}, loud})
    {
        const auto survives = [&](const Telemetry& telemetry) {
            ForceModel model(settings);
            CHECK(withinRanges(model.target(&telemetry), settings));
            for (const double dt : {0.016, 0.0, 1e-12, 0.05, 1e9, 0.016, infinity, nan, -3.0, 0.016})
            {
                CHECK(withinRanges(model.update(&telemetry, dt), settings));
            }
            CHECK(withinRanges(settle(model, &telemetry, 3.0), settings));
            // And it comes back to sane values once the input does.
            const Telemetry sane = driving(6.0, 0.3);
            ForceModel      reference(settings);
            CHECK(largestDifference(settle(model, &sane), settle(reference, &sane)) < 1e-6);
        };

        // Each field on its own against an otherwise ordinary sample, then all of them at once.
        for (const double value : values)
        {
            for (const auto member : numbers)
            {
                Telemetry telemetry = workingField(6.0, 0.4);
                telemetry.*member   = value;
                survives(telemetry);
            }

            Telemetry everything = workingField(6.0, 0.4);
            for (const auto member : numbers)
            {
                everything.*member = value;
            }
            survives(everything);
            everything.lowered = false;
            everything.working = false;
            survives(everything);
        }
    }

    // Spot checks of what the clamping makes of the worst of it.
    Telemetry wild = workingField(infinity, infinity);
    wild.mass      = infinity;
    wild.rough     = infinity;
    const ForceSettings settings;
    const ForceOutput   out = settled(wild);
    CHECK(near(out.rollSpring, settings.rollSpringMax, 1e-6));
    CHECK(near(out.rollForce, -(settings.selfAlign + settings.implementDrag), 1e-6));
    CHECK(near(out.rumble, settings.rumbleMax, 1e-6));

    Telemetry broken = driving(nan, nan);
    broken.mass      = nan;
    broken.rough     = nan;
    const ForceOutput nothing = settled(broken);
    CHECK(near(nothing.rollSpring, 0.0, 1e-6));
    CHECK(near(nothing.rollForce, 0.0, 1e-6));
    CHECK(near(nothing.rumble, 0.0, 1e-6));
}

} // namespace

int main()
{
    testParsesTheContractExample();
    testRejectsFramingViolations();
    testRejectsEveryPartialWrite();
    testIgnoresUnknownKeysAndDefaultsMissingOnes();
    testBadNumbersFallBackToDefaults();
    testAcceptsLineEndings();
    const bool localeTested = testParsingIgnoresTheLocale();

    testReaderWithoutAFile();
    testReaderWaitsForTheFileToMove();
    testReaderGoesStaleAndRecovers();
    testReaderStaleThresholdIsConfigurable();
    testReaderKeepsTheLastSampleThroughBadReads();
    testReaderTreatsASeqRestartAsNew();
    testReaderNeverReturnsATornSample();
    testDefaultTelemetryPath();

    testStartsFromNothing();
    testRollSpringScalesWithSpeed();
    testRollDamperIsTheWeight();
    testRollForceSelfAligns();
    testRollForceAddsImplementDrag();
    testRumbleFollowsSurfaceAndSpeed();
    testEachSurfaceHasItsOwnRumble();
    testRoughGroundComesAndGoesUnderTheWheels();
    testBrokenGroundTugsAtTheWheel();
    testCrossingOntoNewGroundBlends();
    testEngineRumbleIsOptIn();
    testEngineStartShakes();
    testElectricVehiclesHaveNoEngineVibration();
    testAWheelHittingSomethingKicksTowardThatSide();
    testTheKickShrinksAsTheMachineGetsHeavier();
    testBothWheelsTogetherShakeInsteadOfKicking();
    testPitchLeansWithAcceleration();
    testPitchServoMovesAStickyAxisWithoutHunting();
    testPitchIsHeldRegardlessOfTelemetry();
    testFadesToIdle();
    testTargetIsWhereTheOutputsSettle();
    testDegenerateTimeSteps();
    testAStallDoesNotBecomeAJump();
    testSmoothingIsFrameRateIndependent();
    testNoStepExceedsTheSlewLimit();
    testSmoothingModes();
    testOutputsStayInRangeUnderHostileInput();

    if (!localeTested)
    {
        std::puts("note: no comma-decimal locale installed; locale independence was not exercised");
    }
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
