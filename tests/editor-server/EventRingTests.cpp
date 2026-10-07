#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/EventRing.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using nlohmann::json;

namespace
{
    std::vector<uint32_t> Seqs(const EventBatch &batch)
    {
        std::vector<uint32_t> seqs;
        for (const json &event : batch.events)
        {
            seqs.push_back(event.at("seq").get<uint32_t>());
        }
        return seqs;
    }

    std::vector<uint32_t> Range(uint32_t first, uint32_t last)
    {
        std::vector<uint32_t> seqs;
        for (uint32_t seq = first; seq <= last; ++seq)
        {
            seqs.push_back(seq);
        }
        return seqs;
    }

    void PushLogs(EventRing &ring, int count, const std::string &message = "line")
    {
        for (int i = 0; i < count; ++i)
        {
            ring.PushLog(Logger::LogLevel::Info, message, 0);
        }
    }

    constexpr uint32_t All = std::numeric_limits<uint32_t>::max();
}

// ==================== Order and afterSeq ====================

TEST(EventRingTest, AnEmptyRingHasNothingToRead)
{
    const EventRing ring;
    const EventBatch batch = ring.Read(0, All);
    EXPECT_TRUE(batch.events.empty());
    EXPECT_EQ(batch.nextSeq, 0u);
    EXPECT_EQ(batch.dropped, 0u);
    EXPECT_EQ(ring.LastSeq(), 0u);
}

TEST(EventRingTest, SeqsStartAtOneAndEventsComeOldestFirst)
{
    EventRing ring;
    EXPECT_EQ(ring.PushLog(Logger::LogLevel::Info, "first", 0), 1u);
    EXPECT_EQ(ring.PushLog(Logger::LogLevel::Info, "second", 0), 2u);
    EXPECT_EQ(ring.PushLog(Logger::LogLevel::Info, "third", 0), 3u);

    const EventBatch batch = ring.Read(0, All);
    EXPECT_EQ(Seqs(batch), Range(1, 3));
    ASSERT_EQ(batch.events.size(), 3u);
    EXPECT_EQ(batch.events[0].at("message").get<std::string>(), "first");
    EXPECT_EQ(batch.events[2].at("message").get<std::string>(), "third");
    EXPECT_EQ(batch.nextSeq, 3u);
    EXPECT_EQ(batch.dropped, 0u);
}

TEST(EventRingTest, OnlyEventsAfterAfterSeqAreReturned)
{
    EventRing ring;
    PushLogs(ring, 5);

    EXPECT_EQ(Seqs(ring.Read(2, All)), Range(3, 5));
    EXPECT_EQ(Seqs(ring.Read(4, All)), Range(5, 5));

    // Caught up: nothing new, and nextSeq stays where it was
    const EventBatch caughtUp = ring.Read(5, All);
    EXPECT_TRUE(caughtUp.events.empty());
    EXPECT_EQ(caughtUp.nextSeq, 5u);

    // A client that keeps passing nextSeq sees each event once
    ring.PushLog(Logger::LogLevel::Warn, "later", 0);
    const EventBatch next = ring.Read(caughtUp.nextSeq, All);
    EXPECT_EQ(Seqs(next), Range(6, 6));
    EXPECT_EQ(next.nextSeq, 6u);
}

TEST(EventRingTest, MaxEventsLimitsABatchAndNextSeqContinuesFromIt)
{
    EventRing ring;
    PushLogs(ring, 5);

    const EventBatch first = ring.Read(0, 2);
    EXPECT_EQ(Seqs(first), Range(1, 2));
    EXPECT_EQ(first.nextSeq, 2u) << "the last returned seq, so the next poll continues after it";

    const EventBatch second = ring.Read(first.nextSeq, 2);
    EXPECT_EQ(Seqs(second), Range(3, 4));
    const EventBatch third = ring.Read(second.nextSeq, 2);
    EXPECT_EQ(Seqs(third), Range(5, 5));
    EXPECT_EQ(third.nextSeq, 5u);
}

TEST(EventRingTest, MaxEventsZeroReturnsNothingAndSkipsToTheNewest)
{
    EventRing ring;
    PushLogs(ring, 4);

    const EventBatch skip = ring.Read(0, 0);
    EXPECT_TRUE(skip.events.empty());
    EXPECT_EQ(skip.nextSeq, 4u);
    EXPECT_EQ(skip.dropped, 0u) << "retained events skipped on purpose aren't dropped";

    ring.PushLog(Logger::LogLevel::Info, "new", 0);
    EXPECT_EQ(Seqs(ring.Read(skip.nextSeq, All)), Range(5, 5));
}

TEST(EventRingTest, OneReadReturnsAtMostMaxEventsPerRead)
{
    EventRing ring;
    PushLogs(ring, static_cast<int>(EventRing::MaxEventsPerRead) + 10);

    const EventBatch first = ring.Read(0, All);
    EXPECT_EQ(first.events.size(), EventRing::MaxEventsPerRead);
    EXPECT_EQ(first.nextSeq, EventRing::MaxEventsPerRead);

    const EventBatch rest = ring.Read(first.nextSeq, All);
    EXPECT_EQ(Seqs(rest), Range(EventRing::MaxEventsPerRead + 1, EventRing::MaxEventsPerRead + 10));
}

TEST(EventRingTest, AnAfterSeqNeverIssuedReadsAsZero)
{
    // A client still holding a seq from an earlier host process gets this one's backlog, rather than nothing until
    // the seqs catch up
    EventRing ring;
    PushLogs(ring, 3);
    const EventBatch batch = ring.Read(500, All);
    EXPECT_EQ(Seqs(batch), Range(1, 3));
    EXPECT_EQ(batch.nextSeq, 3u);
    EXPECT_EQ(batch.dropped, 0u);
}

// ==================== Bounds and the dropped count ====================

TEST(EventRingTest, BeyondMaxEventsTheOldestFallOffAndEachReaderCountsWhatItMissed)
{
    EventRing ring(3);
    PushLogs(ring, 5);
    EXPECT_EQ(ring.Size(), 3u);
    EXPECT_EQ(ring.LastSeq(), 5u);
    EXPECT_EQ(ring.DroppedTotal(), 2u);

    const EventBatch fromStart = ring.Read(0, All);
    EXPECT_EQ(Seqs(fromStart), Range(3, 5));
    EXPECT_EQ(fromStart.dropped, 2u) << "seqs 1 and 2 fell off before this reader saw them";

    EXPECT_EQ(ring.Read(1, All).dropped, 1u) << "this reader had seen seq 1, so only seq 2 is missing";
    EXPECT_EQ(ring.Read(2, All).dropped, 0u);
    EXPECT_EQ(Seqs(ring.Read(2, All)), Range(3, 5));
    EXPECT_EQ(ring.Read(4, All).dropped, 0u);
    EXPECT_EQ(Seqs(ring.Read(4, All)), Range(5, 5));

    // The ring wraps around for good: still the newest three, still counted per reader
    PushLogs(ring, 4);
    const EventBatch later = ring.Read(fromStart.nextSeq, All);
    EXPECT_EQ(Seqs(later), Range(7, 9));
    EXPECT_EQ(later.dropped, 1u) << "seq 6 fell off after the last poll";
    EXPECT_EQ(ring.DroppedTotal(), 6u);
}

TEST(EventRingTest, BeyondMaxBytesTheOldestFallOffButTheNewestIsKept)
{
    const std::string message(10, 'x');
    const size_t eventBytes = message.size() + EventRing::EventOverheadBytes;
    EventRing ring(100, 3 * eventBytes);

    PushLogs(ring, 3, message);
    EXPECT_EQ(ring.Size(), 3u) << "exactly at the byte limit";
    PushLogs(ring, 1, message);
    EXPECT_EQ(ring.Size(), 3u);
    EXPECT_EQ(Seqs(ring.Read(0, All)), Range(2, 4));
    EXPECT_EQ(ring.Read(0, All).dropped, 1u);

    // An event over the limit on its own pushes out everything else, but is kept
    ring.PushLog(Logger::LogLevel::Error, std::string(10 * eventBytes, 'y'), 0);
    EXPECT_EQ(Seqs(ring.Read(0, All)), Range(5, 5));
    EXPECT_EQ(ring.Read(0, All).dropped, 4u);
}

// ==================== Event contents ====================

TEST(EventRingTest, ALogEventHasItsLevelMessageAndTime)
{
    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, "an info", 1000);
    ring.PushLog(Logger::LogLevel::Warn, "a warning", 2000);
    ring.PushLog(Logger::LogLevel::Error, "an error \xE2\x9C\x93", 3000);

    const EventBatch batch = ring.Read(0, All);
    ASSERT_EQ(batch.events.size(), 3u);
    EXPECT_EQ(batch.events[0], (json{{"seq", 1}, {"kind", "log"}, {"level", "info"}, {"message", "an info"},
                                     {"time", 1000}}));
    EXPECT_EQ(batch.events[1].at("level").get<std::string>(), "warn");
    EXPECT_EQ(batch.events[2].at("level").get<std::string>(), "error");
    EXPECT_EQ(batch.events[2].at("message").get<std::string>(), "an error \xE2\x9C\x93");
    EXPECT_EQ(batch.events[2].at("time").get<int64_t>(), 3000);
}

TEST(EventRingTest, TheTimeIsUnixMilliseconds)
{
    // 2020-01-01 in Unix milliseconds: a clock in seconds, or since another epoch, would be far off
    EXPECT_GT(EventRing::NowUnixMilliseconds(), 1577836800000LL);
}

TEST(EventRingTest, ALongLogMessageIsCutAtACharacterBoundary)
{
    EXPECT_EQ(EventRing::TruncateUtf8("short", 10), "short");
    EXPECT_EQ(EventRing::TruncateUtf8("abcdef", 4), "abcd...");
    // U+00E9 (C3 A9) is two bytes: a cut after its first byte backs up to before it
    EXPECT_EQ(EventRing::TruncateUtf8("ab\xC3\xA9z", 3), "ab...");

    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, std::string(EventRing::MaxLogMessageBytes + 100, 'x'), 0);
    const std::string message = ring.Read(0, All).events.at(0).at("message").get<std::string>();
    EXPECT_EQ(message.size(), EventRing::MaxLogMessageBytes + 3);
    EXPECT_TRUE(message.ends_with("..."));
}

TEST(EventRingTest, PushSetsSeqAndKindOverTheFields)
{
    EventRing ring;
    EXPECT_EQ(ring.Push("sceneChanged", json{{"revision", 7}, {"seq", 99}, {"kind", "not this"}}), 1u);
    EXPECT_EQ(ring.Push("projectChanged"), 2u);
    EXPECT_EQ(ring.Push("odd", json::array({1, 2})), 3u);

    const EventBatch batch = ring.Read(0, All);
    ASSERT_EQ(batch.events.size(), 3u);
    EXPECT_EQ(batch.events[0], (json{{"seq", 1}, {"kind", "sceneChanged"}, {"revision", 7}}));
    EXPECT_EQ(batch.events[1], (json{{"seq", 2}, {"kind", "projectChanged"}}));
    EXPECT_EQ(batch.events[2], (json{{"seq", 3}, {"kind", "odd"}, {"value", {1, 2}}}));
}

TEST(EventRingTest, PushesFromManyThreadsGetUniqueConsecutiveSeqs)
{
    constexpr int Threads = 4;
    constexpr int PerThread = 500;
    EventRing ring;
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t)
    {
        threads.emplace_back([&ring] { PushLogs(ring, PerThread); });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }

    std::vector<uint32_t> seqs;
    uint32_t afterSeq = 0;
    while (true)
    {
        const EventBatch batch = ring.Read(afterSeq, All);
        if (batch.events.empty())
            break;
        const std::vector<uint32_t> more = Seqs(batch);
        seqs.insert(seqs.end(), more.begin(), more.end());
        afterSeq = batch.nextSeq;
    }
    EXPECT_EQ(seqs, Range(1, static_cast<uint32_t>(Threads * PerThread)));
}
