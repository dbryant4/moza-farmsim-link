#include "telemetry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

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
        else if (key == "slip") sample.slip = parseReal(value).value_or(defaults.slip);
        else if (key == "rough") sample.rough = parseReal(value).value_or(defaults.rough);
        else if (key == "field") sample.field = parseFlag(value, defaults.field);
        else if (key == "lowered") sample.lowered = parseFlag(value, defaults.lowered);
        else if (key == "working") sample.working = parseFlag(value, defaults.working);
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

std::optional<Telemetry> TelemetryReader::read() const
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
    std::ifstream file(m_path, std::ios::binary);
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
    // A read racing the writer sees an empty or truncated line, which the parser's framing rejects.
    return parseTelemetry(std::string_view(buffer, size));
}

std::filesystem::path defaultTelemetryPath()
{
#ifdef _WIN32
    // The wide environment, so a profile name outside the ANSI code page survives.
    const wchar_t* const profile = _wgetenv(L"USERPROFILE");
#else
    const char* const profile = std::getenv("USERPROFILE");
#endif
    if (profile == nullptr || *profile == 0)
    {
        return {};
    }
    return std::filesystem::path(profile) / "Documents" / "My Games" / "FarmingSimulator2025" / "modSettings" /
           "FS25_MozaYokeLink" / "telemetry.txt";
}

} // namespace bridge
