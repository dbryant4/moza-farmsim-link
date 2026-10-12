#pragma once

// Notices when a loop that should keep going has stopped, which a thread stuck inside someone
// else's library cannot report by itself.

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace bridge {

/**
 * Watches a loop from a thread of its own. The loop calls beat() on every pass; if no beat arrives
 * for longer than the limit, onStall is called, once, from the watching thread. Watching begins
 * with the first beat.
 *
 * A computer that sleeps makes time jump without the loop being at fault, so a gap in the
 * watcher's own schedule is taken as that and starts the count afresh.
 */
class StallGuard
{
public:
    using Clock = std::chrono::steady_clock;

    StallGuard(std::chrono::milliseconds limit, std::function<void(std::chrono::milliseconds stalledFor)> onStall,
               std::chrono::milliseconds poll = std::chrono::milliseconds(250))
        : m_limit(limit), m_poll(poll), m_onStall(std::move(onStall)), m_thread([this] { watch(); })
    {
    }

    ~StallGuard()
    {
        m_stop = true;
        m_thread.join();
    }

    StallGuard(const StallGuard&)            = delete;
    StallGuard& operator=(const StallGuard&) = delete;

    /** The loop is alive. Safe from any thread, and cheap enough for every pass. */
    void beat() { m_lastBeat.store(Clock::now().time_since_epoch().count(), std::memory_order_release); }

    /** Stops watching without ending the object: for the loop's own orderly end, which may take a while. */
    void disarm() { m_lastBeat.store(0, std::memory_order_release); }

    bool stalled() const { return m_stalled.load(std::memory_order_acquire); }

private:
    void watch()
    {
        auto lastLook = Clock::now();
        while (!m_stop)
        {
            std::this_thread::sleep_for(m_poll);
            const auto now  = Clock::now();
            const bool gap  = now - lastLook > m_poll * 8 + std::chrono::seconds(1);
            lastLook        = now;
            const auto beat = m_lastBeat.load(std::memory_order_acquire);
            if (beat == 0 || m_stop)
            {
                continue;
            }
            if (gap)
            {
                // This thread was held up too: the whole machine paused, not just the loop.
                Clock::rep expected = beat;
                m_lastBeat.compare_exchange_strong(expected, now.time_since_epoch().count());
                continue;
            }
            const auto silent = now - Clock::time_point(Clock::duration(beat));
            if (silent > m_limit)
            {
                m_stalled = true;
                if (m_onStall)
                {
                    m_onStall(std::chrono::duration_cast<std::chrono::milliseconds>(silent));
                }
                return;
            }
        }
    }

    const std::chrono::milliseconds                        m_limit;
    const std::chrono::milliseconds                        m_poll;
    std::function<void(std::chrono::milliseconds)>         m_onStall;
    std::atomic<Clock::rep>                                m_lastBeat{0};
    std::atomic<bool>                                      m_stop{false};
    std::atomic<bool>                                      m_stalled{false};
    std::thread                                            m_thread;
};

} // namespace bridge
