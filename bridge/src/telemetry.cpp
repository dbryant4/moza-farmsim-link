#include "telemetry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace bridge {

namespace {

constexpr std::string_view kPrefix = "MOZAFS 1 ";
constexpr std::string_view kSuffix = " end=1";

// A version 1 line is about 150 bytes; the rest is room for keys added later.
constexpr std::size_t kMaxFileBytes = 4096;

// std::from_chars rather than strtod/stod: those read the decimal separator from the C locale, and
// under a comma-decimal locale would stop at the '.' the contract prescribes.
template <typename Integer>
std::optional<Integer> parseInteger(std::string_view text)
{
    Integer           value = 0;
    const char* const end   = text.data() + text.size();
    const auto        result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc() || result.ptr != end)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseReal(std::string_view text)
{
    double            value = 0.0;
    const char* const end   = text.data() + text.size();
    // fixed: the contract rules out exponent notation, so "1e3" is a malformed value, not 1000.
    const auto result = std::from_chars(text.data(), end, value, std::chars_format::fixed);
    // from_chars also accepts "inf" and "nan", which nothing downstream should have to handle.
    if (result.ec != std::errc() || result.ptr != end || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

bool parseFlag(std::string_view text, bool fallback)
{
    const auto value = parseReal(text);
    return value ? *value != 0.0 : fallback;
}

/** A number this version does not know is a surface it has no pattern for, not an error. */
Surface parseSurface(std::string_view text)
{
    const auto code = parseInteger<int>(text);
    return code && *code > 0 && *code < kSurfaceCount ? static_cast<Surface>(*code) : Surface::Unknown;
}

} // namespace

std::optional<Telemetry> parseTelemetry(std::string_view line)
{
    if (line.ends_with('\n'))
    {
        line.remove_suffix(1);
    }
    if (line.ends_with('\r'))
    {
        line.remove_suffix(1);
    }

    // The writer is not atomic, so a line that does not end in the end token is a write caught
    // half way, however plausible the tokens before it look.
    if (!line.starts_with(kPrefix) || !line.ends_with(kSuffix))
    {
        return std::nullopt;
    }
    line.remove_prefix(kPrefix.size());
    // The file is one line of plain text. A second line, or the NUL bytes a file can show between a
    // truncate and a write, mean the read caught something other than one whole write.
    if (std::any_of(line.begin(), line.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    {
        return std::nullopt;
    }

    const Telemetry defaults;
    Telemetry       sample;
    bool            haveSeq = false;
    while (!line.empty())
    {
        const auto space = line.find(' ');
        const auto token = line.substr(0, space);
        line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);

        const auto equals = token.find('=');
        if (equals == std::string_view::npos)
        {
            continue;
        }
        const auto key   = token.substr(0, equals);
        const auto value = token.substr(equals + 1);

        if (key == "seq")
        {
            // Without a trustworthy seq the sample cannot be told from the one before it.
            const auto seq = parseInteger<std::uint64_t>(value);
            if (!seq)
            {
                return std::nullopt;
            }
            sample.seq = *seq;
            haveSeq    = true;
        }
        else if (key == "t") sample.timeMs = parseInteger<std::int64_t>(value).value_or(defaults.timeMs);
        else if (key == "active") sample.active = parseFlag(value, defaults.active);
        else if (key == "speed") sample.speed = parseReal(value).value_or(defaults.speed);
        else if (key == "steer") sample.steer = parseReal(value).value_or(defaults.steer);
        else if (key == "mass") sample.mass = parseReal(value).value_or(defaults.mass);
        else if (key == "rpm") sample.rpm = parseReal(value).value_or(defaults.rpm);
        else if (key == "load") sample.load = parseReal(value).value_or(defaults.load);
        else if (key == "rpmmin") sample.rpmMin = parseReal(value).value_or(defaults.rpmMin);
        else if (key == "rpmmax") sample.rpmMax = parseReal(value).value_or(defaults.rpmMax);
        else if (key == "slip") sample.slip = parseReal(value).value_or(defaults.slip);
        else if (key == "rough") sample.rough = parseReal(value).value_or(defaults.rough);
        else if (key == "surface") sample.surface = parseSurface(value);
        else if (key == "field") sample.field = parseFlag(value, defaults.field);
        else if (key == "lowered") sample.lowered = parseFlag(value, defaults.lowered);
        else if (key == "working") sample.working = parseFlag(value, defaults.working);
        else if (key == "starting") sample.starting = parseFlag(value, defaults.starting);
        else if (key == "electric") sample.electric = parseFlag(value, defaults.electric);
        else if (key == "bumpl") sample.bumpLeft = parseReal(value).value_or(defaults.bumpLeft);
        else if (key == "bumpr") sample.bumpRight = parseReal(value).value_or(defaults.bumpRight);
        else if (key == "rollrate") sample.rollRate = parseReal(value).value_or(defaults.rollRate);
        else if (key == "pitchrate") sample.pitchRate = parseReal(value).value_or(defaults.pitchRate);
        else if (key == "heave") sample.heave = parseReal(value).value_or(defaults.heave);
        else if (key == "startms") sample.startMs = parseReal(value).value_or(defaults.startMs);
    }

    if (!haveSeq)
    {
        return std::nullopt;
    }
    return sample;
}

TelemetryReader::TelemetryReader(std::filesystem::path path, Clock::duration staleAfter)
    : m_path(std::move(path)), m_staleAfter(staleAfter)
{
}

const TelemetryView& TelemetryReader::poll(Clock::time_point now)
{
    if (const auto sample = read())
    {
        if (!m_seen)
        {
            m_seen        = true;
            m_view.sample = *sample;
            m_lastChange  = now;
        }
        else if (sample->seq != m_view.sample.seq)
        {
            // Any change counts, not only an increase: the mod starts again from a low seq when the
            // game is restarted, and waiting for it to pass the old value would take hours.
            m_view.sample = *sample;
            m_lastChange  = now;
            m_moving      = true;
        }
    }

    if (!m_seen)
    {
        m_view.state = TelemetryState::NeverSeen;
        return m_view;
    }
    m_view.sinceChange = now - m_lastChange;
    m_view.state       = m_moving && m_view.sinceChange < m_staleAfter ? TelemetryState::Fresh : TelemetryState::Stale;
    return m_view;
}

namespace {

/** The file's bytes as of one open-read-close, or nothing if it cannot be opened or is too long to be ours. */
std::optional<std::string> readOnce(const std::filesystem::path& path)
{
    // Standard streams only, so the same code runs on Windows and in the macOS tests. On Windows
    // both libstdc++ (mingw-w64) and the MSVC STL open through the C runtime's _wfopen/_wfsopen
    // with _SH_DENYNO, which reaches CreateFileW as FILE_SHARE_READ | FILE_SHARE_WRITE: the game can
    // open the file for writing, truncate and rewrite it while this handle exists. FILE_SHARE_DELETE
    // is not granted, so a writer that replaced the file by rename would be refused meanwhile; the
    // contract has the mod rewrite in place, and the handle only lives for the one read below.
    // If the writer holds the file exclusively the open fails and the previous sample stands.
    //
    // Binary mode keeps the runtime from translating line endings or treating Ctrl+Z as end of file.
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }

    char buffer[kMaxFileBytes];
    file.read(buffer, sizeof buffer);
    const auto size = static_cast<std::size_t>(file.gcount());
    if (size == sizeof buffer)
    {
        // Possibly cut short by the buffer, so its ending proves nothing.
        return std::nullopt;
    }
    return std::string(buffer, size);
}

} // namespace

std::optional<Telemetry> TelemetryReader::read() const
{
    // A read racing the writer usually sees an empty or truncated line, which the parser's framing
    // rejects. But the file is rewritten in place, and one read can also come back as the head of
    // one line joined to the tail of the next, with both frame markers intact and a number in the
    // middle that neither line contained. So read twice and take the sample only if both reads
    // agree: a write landing during either one makes them differ, and the next poll gets the new line.
    const auto first = readOnce(m_path);
    if (!first)
    {
        return std::nullopt;
    }
    const auto second = readOnce(m_path);
    if (!second || *second != *first)
    {
        return std::nullopt;
    }
    return parseTelemetry(*first);
}

std::filesystem::path defaultGameFolder()
{
#ifdef _WIN32
    // The game keeps its folder under the user's Documents, wherever Windows has that: with
    // OneDrive's folder backup on, it is not the Documents folder inside the user profile.
    std::filesystem::path documents;
    PWSTR                 known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &known)) && known != nullptr)
    {
        documents = known;
    }
    CoTaskMemFree(known);
    if (documents.empty())
    {
        // The wide environment, so a profile name outside the ANSI code page survives.
        const wchar_t* const profile = _wgetenv(L"USERPROFILE");
        if (profile == nullptr || *profile == 0)
        {
            return {};
        }
        documents = std::filesystem::path(profile) / "Documents";
    }
#else
    const char* const profile = std::getenv("USERPROFILE");
    if (profile == nullptr || *profile == 0)
    {
        return {};
    }
    const std::filesystem::path documents = std::filesystem::path(profile) / "Documents";
#endif
    return documents / "My Games" / "FarmingSimulator2025";
}

std::filesystem::path defaultTelemetryPath()
{
    const std::filesystem::path game = defaultGameFolder();
    return game.empty() ? game : game / "modSettings" / "FS25_MozaYokeLink" / "telemetry.txt";
}

} // namespace bridge
