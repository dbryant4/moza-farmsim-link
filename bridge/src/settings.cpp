#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

namespace bridge {

namespace {

bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && isBlank(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

bool isValidKey(std::string_view key)
{
    if (key.empty())
    {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

std::string lowered(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

/** Makes registered text safe to put on one comment line. */
std::string oneLine(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        if (static_cast<unsigned char>(c) < 0x20)
        {
            c = ' ';
        }
    }
    return result;
}

std::string valueText(const SettingInfo& info, double value)
{
    if (info.kind == SettingInfo::Kind::Toggle)
    {
        return value != 0.0 ? "true" : "false";
    }
    return formatNumber(value);
}

std::string fileText(const std::filesystem::path& path)
{
    // u8string() cannot fail on a name the narrow code page has no character for; string() can.
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

} // namespace

std::string formatNumber(double value)
{
    if (value == 0.0)
    {
        return "0";
    }
    // Plain decimals wherever they stay short: to_chars would otherwise pick "1e+05" over "100000".
    const double magnitude = std::abs(value);
    const auto   format    = magnitude >= 1e-5 && magnitude < 1e15 ? std::chars_format::fixed : std::chars_format::scientific;
    char         buffer[64];
    const auto   result = std::to_chars(buffer, buffer + sizeof buffer, value, format);
    return std::string(buffer, result.ptr);
}

std::optional<double> parseNumber(std::string_view text)
{
    text = trim(text);
    if (!text.empty() && text.front() == '+')
    {
        // from_chars takes no leading plus sign.
        text.remove_prefix(1);
        if (!text.empty() && text.front() == '-')
        {
            return std::nullopt;
        }
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    double     value  = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

namespace {

/** A toggle takes the usual words for on and off; anything else is a plain number. */
std::optional<double> parseValue(SettingInfo::Kind kind, std::string_view text)
{
    if (kind != SettingInfo::Kind::Toggle)
    {
        return parseNumber(text);
    }
    const std::string word = lowered(trim(text));
    if (word == "true" || word == "on" || word == "yes" || word == "1")
    {
        return 1.0;
    }
    if (word == "false" || word == "off" || word == "no" || word == "0")
    {
        return 0.0;
    }
    return std::nullopt;
}

} // namespace

bool Settings::addNumber(std::string key, std::string group, std::string label, std::string description, std::string unit,
                         double minimum, double maximum, double step, double defaultValue)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step) || !std::isfinite(defaultValue) ||
        minimum > maximum || step <= 0.0)
    {
        return false;
    }
    SettingInfo info;
    info.key          = std::move(key);
    info.group        = std::move(group);
    info.label        = std::move(label);
    info.description  = std::move(description);
    info.unit         = std::move(unit);
    info.kind         = SettingInfo::Kind::Number;
    info.minimum      = minimum;
    info.maximum      = maximum;
    info.step         = step;
    info.defaultValue = std::clamp(defaultValue, minimum, maximum);
    return add(std::move(info));
}

bool Settings::addToggle(std::string key, std::string group, std::string label, std::string description, bool defaultValue)
{
    SettingInfo info;
    info.key          = std::move(key);
    info.group        = std::move(group);
    info.label        = std::move(label);
    info.description  = std::move(description);
    info.kind         = SettingInfo::Kind::Toggle;
    info.minimum      = 0.0;
    info.maximum      = 1.0;
    info.step         = 1.0;
    info.defaultValue = defaultValue ? 1.0 : 0.0;
    return add(std::move(info));
}

bool Settings::add(SettingInfo info)
{
    if (!isValidKey(info.key))
    {
        return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_index.find(info.key) != m_index.end())
    {
        return false;
    }
    m_index.emplace(info.key, m_entries.size());
    Entry entry;
    entry.value = info.defaultValue;
    entry.saved = info.defaultValue;
    entry.info  = std::move(info);
    m_entries.push_back(std::move(entry));
    return true;
}

const Settings::Entry* Settings::find(std::string_view key) const
{
    const auto found = m_index.find(key);
    return found == m_index.end() ? nullptr : &m_entries[found->second];
}

double Settings::clamped(const SettingInfo& info, double value)
{
    if (info.kind == SettingInfo::Kind::Toggle)
    {
        return value != 0.0 ? 1.0 : 0.0;
    }
    return std::clamp(value, info.minimum, info.maximum);
}

bool Settings::contains(std::string_view key) const
{
    std::lock_guard lock(m_mutex);
    return find(key) != nullptr;
}

double Settings::get(std::string_view key) const
{
    std::lock_guard lock(m_mutex);
    const Entry*    entry = find(key);
    return entry ? entry->value : 0.0;
}

bool Settings::set(std::string_view key, double value)
{
    if (!std::isfinite(value))
    {
        return false;
    }
    std::lock_guard lock(m_mutex);
    const auto      found = m_index.find(key);
    if (found == m_index.end())
    {
        return false;
    }
    Entry&       entry = m_entries[found->second];
    const double next  = clamped(entry.info, value);
    if (next != entry.value)
    {
        entry.value = next;
        m_revision.fetch_add(1, std::memory_order_release);
    }
    return true;
}

std::optional<double> Settings::parse(std::string_view key, std::string_view text) const
{
    SettingInfo::Kind kind;
    {
        std::lock_guard lock(m_mutex);
        const Entry*    entry = find(key);
        if (!entry)
        {
            return std::nullopt;
        }
        kind = entry->info.kind;
    }
    return parseValue(kind, text);
}

void Settings::resetToDefaults()
{
    std::lock_guard lock(m_mutex);
    bool            changed = false;
    for (Entry& entry : m_entries)
    {
        changed     = changed || entry.value != entry.info.defaultValue;
        entry.value = entry.info.defaultValue;
    }
    if (changed)
    {
        m_revision.fetch_add(1, std::memory_order_release);
    }
}

bool Settings::dirty() const
{
    std::lock_guard lock(m_mutex);
    return std::any_of(m_entries.begin(), m_entries.end(), [](const Entry& entry) { return entry.value != entry.saved; });
}

std::vector<SettingState> Settings::snapshot() const
{
    std::lock_guard           lock(m_mutex);
    std::vector<SettingState> result;
    result.reserve(m_entries.size());
    for (const Entry& entry : m_entries)
    {
        result.push_back({entry.info, entry.value, entry.saved});
    }
    return result;
}

Settings::LoadResult Settings::load(const std::filesystem::path& path)
{
    std::lock_guard fileLock(m_fileMutex);
    LoadResult      result;

    std::vector<std::string> lines;
    {
        std::error_code ec;
        std::ifstream   in;
        if (!std::filesystem::is_directory(path, ec))
        {
            in.open(path);
        }
        if (!in.is_open())
        {
            if (!std::filesystem::exists(path, ec) && !ec)
            {
                // No file yet is the normal first run: everything stays, or goes back to, its default.
                result.fileFound = false;
            }
            else
            {
                result.ok = false;
                result.warnings.push_back("cannot open " + fileText(path));
                return result;
            }
        }
        else
        {
            result.fileFound = true;
            std::string line;
            while (std::getline(in, line))
            {
                lines.push_back(std::move(line));
            }
            if (in.bad())
            {
                result.ok = false;
                result.warnings.push_back("cannot read " + fileText(path));
                return result;
            }
        }
    }

    std::lock_guard     lock(m_mutex);
    std::vector<double> values;
    values.reserve(m_entries.size());
    for (const Entry& entry : m_entries)
    {
        values.push_back(entry.info.defaultValue);
    }

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        std::string_view line = lines[i];
        if (i == 0 && line.substr(0, 3) == "\xEF\xBB\xBF")
        {
            // Notepad's UTF-8 byte order mark.
            line.remove_prefix(3);
        }
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';')
        {
            continue;
        }
        const std::string where  = "line " + std::to_string(i + 1) + ": ";
        const auto        equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            result.warnings.push_back(where + "expected 'key = value', skipped");
            continue;
        }
        const std::string_view key  = trim(line.substr(0, equals));
        std::string_view       text = line.substr(equals + 1);
        text                        = trim(text.substr(0, text.find('#')));

        const auto found = m_index.find(key);
        if (found == m_index.end())
        {
            result.warnings.push_back(where + "unknown setting '" + std::string(key) + "', skipped");
            continue;
        }
        const SettingInfo& info = m_entries[found->second].info;

        const std::optional<double> value = parseValue(info.kind, text);
        if (!value)
        {
            result.warnings.push_back(where + "cannot read '" + std::string(text) + "' as a value for " + info.key + ", skipped");
            continue;
        }
        const double kept = clamped(info, *value);
        if (kept != *value)
        {
            result.warnings.push_back(where + info.key + " = " + formatNumber(*value) + " is outside " + formatNumber(info.minimum) +
                                      " to " + formatNumber(info.maximum) + ", using " + formatNumber(kept));
        }
        values[found->second] = kept;
    }

    bool changed = false;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        changed            = changed || m_entries[i].value != values[i];
        m_entries[i].value = values[i];
        m_entries[i].saved = values[i];
    }
    if (changed)
    {
        m_revision.fetch_add(1, std::memory_order_release);
    }
    return result;
}

bool Settings::save(const std::filesystem::path& path, std::string* error)
{
    std::lock_guard fileLock(m_fileMutex);

    const auto fail = [&](const std::string& reason) {
        if (error)
        {
            *error = reason;
        }
        return false;
    };

    const std::vector<SettingState> states = snapshot();

    std::string text = "# moza_fs_bridge settings. Loaded when the bridge starts and rewritten by Save in the web UI.\n"
                       "# Safe to edit by hand while the bridge is not running: one 'key = value' per line,\n"
                       "# lines starting with # are comments, and anything missing or unreadable falls back to its default.\n";
    const std::string* group = nullptr;
    for (const SettingState& state : states)
    {
        const SettingInfo& info = state.info;
        if (!group || *group != info.group)
        {
            group = &info.group;
            text += "\n# ---- " + oneLine(info.group) + " ----\n";
        }
        text += "\n# " + oneLine(info.label) + "\n";
        if (!info.description.empty())
        {
            text += "# " + oneLine(info.description) + "\n";
        }
        if (info.kind == SettingInfo::Kind::Toggle)
        {
            text += "# true or false, default " + valueText(info, info.defaultValue) + "\n";
        }
        else
        {
            text += "# " + formatNumber(info.minimum) + " to " + formatNumber(info.maximum);
            if (!info.unit.empty())
            {
                text += " " + oneLine(info.unit);
            }
            text += ", default " + formatNumber(info.defaultValue) + "\n";
        }
        text += info.key + " = " + valueText(info, state.value) + "\n";
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::out | std::ios::trunc);
        if (!out)
        {
            return fail("cannot create " + fileText(temporary));
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        const bool written = out.good();
        out.close();
        if (!written || out.fail())
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail("cannot write " + fileText(temporary));
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        // std::filesystem::rename replaces an existing file on every runtime this is built with,
        // but a bare Windows rename does not. If it was refused with the target in the way, move
        // the old file aside first, so that at every instant either it or the new one is complete.
        std::error_code probe;
        if (std::filesystem::is_regular_file(path, probe))
        {
            std::filesystem::path backup = path;
            backup += ".bak";
            std::error_code step;
            std::filesystem::remove(backup, step);
            std::filesystem::rename(path, backup, step);
            if (!step)
            {
                std::filesystem::rename(temporary, path, ec);
                if (ec)
                {
                    std::filesystem::rename(backup, path, step);
                }
                else
                {
                    std::filesystem::remove(backup, step);
                }
            }
        }
    }
    if (ec)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail("cannot replace " + fileText(path) + ": " + ec.message());
    }

    std::lock_guard lock(m_mutex);
    for (std::size_t i = 0; i < m_entries.size() && i < states.size(); ++i)
    {
        m_entries[i].saved = states[i].value;
    }
    return true;
}

} // namespace bridge
