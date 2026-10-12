// The guard that notices a stuck loop.

#include "stall_guard.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                              \
    do                                                                                \
    {                                                                                 \
        if (!(condition))                                                             \
        {                                                                             \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            ++g_failures;                                                             \
        }                                                                             \
    } while (false)

using namespace std::chrono_literals;

void sleepFor(std::chrono::milliseconds time)
{
    std::this_thread::sleep_for(time);
}

void testQuietUntilTheLoopStarts()
{
    std::atomic<int> stalls{0};
    {
        bridge::StallGuard guard(100ms, [&](std::chrono::milliseconds) { ++stalls; }, 10ms);
        sleepFor(400ms);
        CHECK(!guard.stalled());
    }
    CHECK(stalls == 0);
}

void testALoopThatKeepsGoingIsLeftAlone()
{
    std::atomic<int> stalls{0};
    {
        // The limit is far above the pause between beats, so that a busy machine stretching a
        // pause does not look like a stall.
        bridge::StallGuard guard(1500ms, [&](std::chrono::milliseconds) { ++stalls; }, 10ms);
        for (int i = 0; i < 60; ++i)
        {
            guard.beat();
            sleepFor(10ms);
        }
        CHECK(!guard.stalled());
    }
    CHECK(stalls == 0);
}

void testALoopThatStopsIsReportedOnce()
{
    std::atomic<int>       stalls{0};
    std::atomic<long long> reported{0};
    bridge::StallGuard     guard(
        150ms,
        [&](std::chrono::milliseconds silent) {
            ++stalls;
            reported = silent.count();
        },
        10ms);
    for (int i = 0; i < 10; ++i)
    {
        guard.beat();
        sleepFor(10ms);
    }
    CHECK(stalls == 0);
    // ... and then nothing more, as when the loop's thread is stuck in a call that never returns.
    sleepFor(600ms);
    CHECK(stalls == 1);
    CHECK(guard.stalled());
    // Past the limit; how far past depends on how busy the machine is, so only the lower end is exact.
    CHECK(reported > 150 && reported < 5000);
    // Beats that come afterwards do not bring a second report, and neither does more silence.
    guard.beat();
    sleepFor(400ms);
    CHECK(stalls == 1);
}

void testAnOrderlyEndIsNotAStall()
{
    std::atomic<int> stalls{0};
    {
        bridge::StallGuard guard(100ms, [&](std::chrono::milliseconds) { ++stalls; }, 10ms);
        guard.beat();
        guard.disarm();
        sleepFor(400ms);
        CHECK(!guard.stalled());
        // Watching can be taken up again.
        guard.beat();
        sleepFor(400ms);
        CHECK(guard.stalled());
    }
    CHECK(stalls == 1);
}

} // namespace

int main()
{
    testQuietUntilTheLoopStarts();
    testALoopThatKeepsGoingIsLeftAlone();
    testALoopThatStopsIsReportedOnce();
    testAnOrderlyEndIsNotAStall();
    if (g_failures != 0)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("stall guard tests passed");
    return 0;
}
