#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

/** One line of live status: what it is, and the number or text last published for it. */
struct StatusItem
{
    enum class Kind
    {
        /** Nothing published yet. */
        Empty,
        Number,
        Text,
    };

    std::string key;
    std::string group;
    std::string label;
    std::string unit;
    /** With a range the UI draws a bar: from the left end, or out from zero when the range spans it. */
    bool   hasRange = false;
    double minimum  = 0.0;
    double maximum  = 0.0;
    /** Digits after the decimal point when shown as a number. */
    int         decimals = 2;
    Kind        kind     = Kind::Empty;
    double      number   = 0.0;
    std::string text;
};

struct StatusSnapshot
{
    std::vector<StatusItem> items;
    /** Seconds since anything was last published, or negative if nothing ever was. */
    double ageSeconds = -1.0;
};

/**
 * What the main loop wants shown, and what the UI wants the main loop to do.
 *
 * The main loop registers items at startup, then publishes values a few times a second with set()
 * and setText(); the UI thread copies them out with snapshot(). Register everything before other
 * threads use the object. After that every member is safe to call concurrently.
 *
 * The two commands travel the other way: the UI writes them and the main loop polls them.
 */
class StatusBoard
{
public:
    /** Registers an item with no range. Returns false for an empty or duplicate key. */
    bool add(std::string key, std::string group, std::string label, std::string unit = {}, int decimals = 2);

    /** Registers an item drawn as a bar between @p minimum and @p maximum. Also false for an empty or non-finite range. */
    bool addRanged(std::string key, std::string group, std::string label, std::string unit, double minimum, double maximum,
                   int decimals = 2);

    /** Publishes a number. Returns false for an unknown key. A non-finite value is shown as "no value". */
    bool set(std::string_view key, double value);

    /** Publishes text in place of a number, e.g. a connection state. Returns false for an unknown key. */
    bool setText(std::string_view key, std::string_view text);

    /** Every item in registration order, copied under the lock. */
    StatusSnapshot snapshot() const;

    /** Master switch, on by default. While it is off the main loop must not apply any force. */
    bool forcesEnabled() const { return m_forcesEnabled.load(std::memory_order_acquire); }
    void setForcesEnabled(bool enabled) { m_forcesEnabled.store(enabled, std::memory_order_release); }

    /**
     * Counts "release forces now" requests. The main loop remembers the last count it acted on and
     * drops every force at once when it sees a higher one.
     */
    std::uint64_t releaseRequests() const { return m_releaseRequests.load(std::memory_order_acquire); }
    void          requestRelease() { m_releaseRequests.fetch_add(1, std::memory_order_acq_rel); }

private:
    using Clock = std::chrono::steady_clock;

    bool        addItem(StatusItem item);
    StatusItem* find(std::string_view key);

    mutable std::mutex                              m_mutex;
    std::vector<StatusItem>                         m_items;
    std::map<std::string, std::size_t, std::less<>> m_index;
    bool                                            m_published = false;
    Clock::time_point                               m_lastPublish;
    std::atomic<bool>                               m_forcesEnabled{true};
    std::atomic<std::uint64_t>                      m_releaseRequests{0};
};

} // namespace bridge
