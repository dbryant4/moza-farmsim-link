#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

/** Shortest text that reads back as exactly @p value. Always uses '.', whatever the process locale. */
std::string formatNumber(double value);

/** Reads a whole string as a finite decimal number, ignoring the process locale and surrounding blanks. */
std::optional<double> parseNumber(std::string_view text);

/** Everything a UI needs to draw one setting. A toggle is stored as 0 or 1. */
struct SettingInfo
{
    enum class Kind
    {
        Number,
        Toggle,
    };

    std::string key;
    std::string group;
    std::string label;
    /** One sentence shown as help next to the control. */
    std::string description;
    std::string unit;
    Kind        kind         = Kind::Number;
    double      minimum      = 0.0;
    double      maximum      = 1.0;
    double      step         = 1.0;
    double      defaultValue = 0.0;
};

struct SettingState
{
    SettingInfo info;
    double      value = 0.0;
    /** The value a restart would come up with: what was last loaded or saved, else the default. */
    double saved = 0.0;
};

/**
 * The program's tunable values, registered at startup and then read and written from any thread.
 *
 * Register everything before other threads use the object. After that every member is safe to call
 * concurrently. Registration order is display order, and groups appear in order of first use.
 */
class Settings
{
public:
    /**
     * Registers a numeric setting. @p key is the stable identifier used in the settings file and
     * by the UI: letters, digits, '.', '_' and '-' only. Returns false, registering nothing, for
     * an empty, malformed or duplicate key, a non-finite or inverted range, or a step that is not
     * positive. The default is clamped into the range.
     */
    bool addNumber(std::string key, std::string group, std::string label, std::string description, std::string unit,
                   double minimum, double maximum, double step, double defaultValue);

    /** Registers an on/off setting, read back with enabled(). Same key rules as addNumber(). */
    bool addToggle(std::string key, std::string group, std::string label, std::string description, bool defaultValue);

    bool contains(std::string_view key) const;

    /** Current value, or 0 for a key that was never registered. */
    double get(std::string_view key) const;

    /** get() for toggles. */
    bool enabled(std::string_view key) const { return get(key) != 0.0; }

    /**
     * Stores @p value clamped to the setting's range (for a toggle, anything other than 0 is on).
     * Returns false and changes nothing for an unknown key or a value that is not finite.
     */
    bool set(std::string_view key, double value);

    /**
     * What @p text means for @p key, not yet clamped: a number, or for a toggle one of
     * true/false, on/off, yes/no, 1/0. Empty for an unknown key or text that does not parse.
     */
    std::optional<double> parse(std::string_view key, std::string_view text) const;

    void resetToDefaults();

    /**
     * Goes up by one every time a stored value actually changes (set, reset or load), and never
     * down. Compare against the last one seen to notice changes without taking the lock.
     */
    std::uint64_t revision() const { return m_revision.load(std::memory_order_acquire); }

    /** True while any value differs from what was last loaded or saved, i.e. from what a restart would give. */
    bool dirty() const;

    /** Every setting with its current and saved value, in registration order. */
    std::vector<SettingState> snapshot() const;

    struct LoadResult
    {
        /** False only when the file exists but could not be read; the values are then left alone. */
        bool ok        = true;
        bool fileFound = false;
        /** One line per skipped or adjusted entry, for the caller to print. Never fatal. */
        std::vector<std::string> warnings;
    };

    /**
     * Replaces the values with what a fresh start with this file gives: defaults, overridden by
     * each `key = value` line. A missing file therefore means all defaults. Unknown keys and lines
     * that do not parse are skipped, out-of-range values are clamped, and each is reported in the
     * result. Afterwards nothing is dirty.
     */
    LoadResult load(const std::filesystem::path& path);

    /**
     * Writes every setting as a commented `key = value` line. The text goes to a temporary file
     * beside @p path which is then renamed over it, so a crash mid-save leaves the old file intact.
     * Returns false with the reason in @p error on failure; the values and dirty state are untouched.
     */
    bool save(const std::filesystem::path& path, std::string* error = nullptr);

private:
    struct Entry
    {
        SettingInfo info;
        double      value = 0.0;
        double      saved = 0.0;
    };

    bool          add(SettingInfo info);
    const Entry*  find(std::string_view key) const;
    static double clamped(const SettingInfo& info, double value);

    mutable std::mutex m_mutex;
    /** Serializes load() and save() so two saves never share the temporary file. Taken before m_mutex. */
    std::mutex                                      m_fileMutex;
    std::vector<Entry>                              m_entries;
    std::map<std::string, std::size_t, std::less<>> m_index;
    std::atomic<std::uint64_t>                      m_revision{0};
};

} // namespace bridge
