// Checks the telemetry parser, the file reader and the force model. None of them touch the device.

#include "force_model.hpp"
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
    testEngineRumbleIsOptIn();
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
