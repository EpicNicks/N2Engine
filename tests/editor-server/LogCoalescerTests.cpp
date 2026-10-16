#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <editor-server/LogCoalescer.hpp>

using namespace N2Engine::Editor;
using namespace std::chrono_literals;

// The coalescer that bounds the log lines of connections that never authenticate (PR #176). Pure: the clock is a
// number the test sets, nothing sleeps.

namespace
{
    struct FakeClock
    {
        std::atomic<int64_t> ms{100000};

        LogCoalescer::Clock Get()
        {
            return [this] { return LogCoalescer::TimePoint(std::chrono::milliseconds(ms.load())); };
        }
    };

    constexpr const char *Tls = "TLS handshake failed";
    constexpr const char *Garbage = "command before Hello";
}

TEST(LogCoalescerTest, TheFirstOccurrenceIsWrittenAtOnceThenTheBurstAndThenOnlyCounted)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 3}, clock.Get());

    const LogCoalescer::Decision first = coalescer.Record(Tls);
    EXPECT_TRUE(first.write);
    EXPECT_FALSE(first.closed.has_value());
    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_TRUE(coalescer.Record(Tls).write);
    for (int i = 0; i < 20; ++i)
    {
        const LogCoalescer::Decision later = coalescer.Record(Tls);
        EXPECT_FALSE(later.write) << i;
        EXPECT_FALSE(later.closed.has_value()) << i;
    }
    EXPECT_EQ(coalescer.TotalSuppressed(), 20u);
}

TEST(LogCoalescerTest, KindsHaveTheirOwnWindows)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 1}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_FALSE(coalescer.Record(Tls).write);
    EXPECT_TRUE(coalescer.Record(Garbage).write) << "another kind is not held back by the first one's burst";
    EXPECT_FALSE(coalescer.Record(Garbage).write);
    EXPECT_EQ(coalescer.TotalSuppressed(), 2u);
}

TEST(LogCoalescerTest, TheNextOccurrenceAfterTheWindowReportsTheCountAndIsWritten)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 1}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FALSE(coalescer.Record(Tls).write);
    }
    clock.ms += 9999;
    EXPECT_FALSE(coalescer.Record(Tls).write) << "still inside the window";

    clock.ms += 1;
    const LogCoalescer::Decision next = coalescer.Record(Tls);
    EXPECT_TRUE(next.write) << "a new window: its first occurrence is written";
    ASSERT_TRUE(next.closed.has_value());
    EXPECT_EQ(next.closed->kind, Tls);
    EXPECT_EQ(next.closed->suppressed, 5u);
    EXPECT_EQ(next.closed->window, 10s);
    // The new window has its own burst
    EXPECT_FALSE(coalescer.Record(Tls).write);
}

TEST(LogCoalescerTest, AWindowThatEndsWithNothingUnwrittenReportsNothing)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 2}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_TRUE(coalescer.Record(Tls).write);
    clock.ms += 20000;
    const LogCoalescer::Decision next = coalescer.Record(Tls);
    EXPECT_TRUE(next.write);
    EXPECT_FALSE(next.closed.has_value());
    clock.ms += 20000;
    EXPECT_TRUE(coalescer.TakeClosed().empty());
}

TEST(LogCoalescerTest, TakeClosedReportsEndedWindowsOnceAndTheKindStartsAfresh)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 1}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_FALSE(coalescer.Record(Tls).write);
    EXPECT_FALSE(coalescer.Record(Tls).write);
    clock.ms += 5000;
    EXPECT_TRUE(coalescer.Record(Garbage).write);
    EXPECT_FALSE(coalescer.Record(Garbage).write);

    EXPECT_TRUE(coalescer.TakeClosed().empty()) << "neither window has ended";

    clock.ms += 5000; // the first kind's window has ended, the second's has not
    const std::vector<LogCoalescer::Summary> closed = coalescer.TakeClosed();
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0].kind, Tls);
    EXPECT_EQ(closed[0].suppressed, 2u);
    EXPECT_TRUE(coalescer.TakeClosed().empty()) << "reported once";

    // The reported kind starts afresh: its next occurrence is written and reports nothing more
    const LogCoalescer::Decision next = coalescer.Record(Tls);
    EXPECT_TRUE(next.write);
    EXPECT_FALSE(next.closed.has_value());

    clock.ms += 5000;
    const std::vector<LogCoalescer::Summary> later = coalescer.TakeClosed();
    ASSERT_EQ(later.size(), 1u);
    EXPECT_EQ(later[0].kind, Garbage);
    EXPECT_EQ(later[0].suppressed, 1u);
}

TEST(LogCoalescerTest, TakeAllReportsEverythingUnwrittenWhateverTheTime)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 1}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_FALSE(coalescer.Record(Tls).write);
    EXPECT_TRUE(coalescer.Record(Garbage).write);

    const std::vector<LogCoalescer::Summary> all = coalescer.TakeAll();
    ASSERT_EQ(all.size(), 1u) << "a kind with nothing unwritten has nothing to report";
    EXPECT_EQ(all[0].kind, Tls);
    EXPECT_EQ(all[0].suppressed, 1u);
    EXPECT_TRUE(coalescer.TakeAll().empty());
    EXPECT_TRUE(coalescer.Record(Tls).write) << "every kind starts afresh";
}

TEST(LogCoalescerTest, AClockThatGoesBackwardsReanchorsTheWindowAndKeepsItsCounts)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 1}, clock.Get());

    EXPECT_TRUE(coalescer.Record(Tls).write);
    EXPECT_FALSE(coalescer.Record(Tls).write);

    // An hour back: not a new window (no fresh burst), and the window does not wait an hour for the old time either
    clock.ms -= 3600 * 1000;
    const LogCoalescer::Decision back = coalescer.Record(Tls);
    EXPECT_FALSE(back.write);
    EXPECT_FALSE(back.closed.has_value());
    clock.ms += 5000;
    EXPECT_TRUE(coalescer.TakeClosed().empty()) << "the window counts from the new time";

    clock.ms += 5000; // 10 s after the clock went back
    const std::vector<LogCoalescer::Summary> closed = coalescer.TakeClosed();
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0].suppressed, 2u) << "what was counted before the jump is kept";
    EXPECT_TRUE(coalescer.Record(Tls).write);

    // Backwards while only TakeClosed looks: the same re-anchoring
    EXPECT_FALSE(coalescer.Record(Tls).write);
    clock.ms -= 60000;
    EXPECT_TRUE(coalescer.TakeClosed().empty());
    clock.ms += 10000;
    const std::vector<LogCoalescer::Summary> again = coalescer.TakeClosed();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].suppressed, 1u);
}

TEST(LogCoalescerTest, TheOptionsAreClamped)
{
    const LogCoalescer coalescer(LogCoalescerOptions{0ms, 0});
    EXPECT_EQ(coalescer.Options().window, 1ms);
    EXPECT_EQ(coalescer.Options().burst, 1u);

    const LogCoalescer defaults;
    EXPECT_EQ(defaults.Options().window, 10s);
    EXPECT_EQ(defaults.Options().burst, 10u);
}

TEST(LogCoalescerTest, TheSummaryLineNamesTheCountTheKindAndTheWindow)
{
    const std::string line = LogCoalescer::Describe(LogCoalescer::Summary{Garbage, 47, 10s}, 10);
    EXPECT_EQ(line, "47 more pre-auth failure(s) of kind 'command before Hello' in the last 10 s were not logged one by "
                    "one (at most 10 line(s) per kind every 10 s are)");
    EXPECT_NE(LogCoalescer::Describe(LogCoalescer::Summary{Tls, 1, 1500ms}, 1).find("in the last 1.5 s"),
              std::string::npos);
}

TEST(LogCoalescerTest, ConcurrentRecordsCountEveryOccurrenceOnce)
{
    FakeClock clock;
    LogCoalescer coalescer(LogCoalescerOptions{10s, 10}, clock.Get());

    constexpr int Threads = 4;
    constexpr int PerThread = 1000;
    std::atomic<int> written{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < PerThread; ++i)
                {
                    if (coalescer.Record(Tls).write)
                        ++written;
                }
            });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(written.load(), 10);
    EXPECT_EQ(coalescer.TotalSuppressed(), static_cast<uint64_t>(Threads * PerThread - 10));
    const std::vector<LogCoalescer::Summary> all = coalescer.TakeAll();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].suppressed, static_cast<uint64_t>(Threads * PerThread - 10));
}
