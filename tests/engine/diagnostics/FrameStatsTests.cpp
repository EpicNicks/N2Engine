#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "engine/diagnostics/FrameStats.hpp"

// The frame timers of EN5 (FrameStats): the ring of the last 240 frames, what sinceFrame returns, the sections, the
// newest record's open time and the summary. The clock is injected (BeginFrame and EndFrame take the time), so the
// durations are exact.

using namespace N2Engine::Diagnostics;
using Clock = FrameStats::Clock;
using std::chrono::milliseconds;

namespace
{
    const Clock::time_point T0 = Clock::time_point{} + std::chrono::hours(1);

    /// One frame from `start`, `tickMs` long
    void Frame(FrameStats &stats, const Clock::time_point start, const int tickMs)
    {
        stats.BeginFrame(start);
        stats.EndFrame(start + milliseconds(tickMs));
    }

    std::vector<std::uint32_t> Numbers(const std::vector<FrameRecord> &records)
    {
        std::vector<std::uint32_t> numbers;
        for (const FrameRecord &record : records)
        {
            numbers.push_back(record.frame);
        }
        return numbers;
    }
}

TEST(FrameStatsTest, OffByDefaultAndRecordsNothingWhileOff)
{
    FrameStats stats;
    EXPECT_FALSE(stats.IsEnabled());
    Frame(stats, T0, 5);
    stats.Add(FrameSection::Update, 3.0);
    stats.NoteFixedTick();
    stats.AddToNewestFrame(FrameSection::Commands, 1.0);
    EXPECT_EQ(stats.Size(), 0u);
    EXPECT_FALSE(stats.NewestFrame().has_value());
    EXPECT_TRUE(stats.FramesAfter(0).empty());
    EXPECT_EQ(stats.Summarize().windowFrames, 0u);
}

TEST(FrameStatsTest, AFrameRecordsItsSectionsTicksAndLength)
{
    FrameStats stats;
    stats.SetEnabled(true);
    stats.BeginFrame(T0);
    EXPECT_TRUE(stats.IsFrameOpen());
    stats.Add(FrameSection::Fixed, 2.0);
    stats.Add(FrameSection::Physics, 1.25);
    stats.NoteFixedTick();
    stats.NoteFixedTick();
    stats.Add(FrameSection::Update, 0.5);
    stats.Add(FrameSection::Update, 0.25); // sections of one kind add up
    stats.Add(FrameSection::Audio, 0.125);
    stats.Add(FrameSection::Render, 4.0);
    stats.EndFrame(T0 + milliseconds(9));
    EXPECT_FALSE(stats.IsFrameOpen());

    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 1u);
    const FrameRecord &record = frames[0];
    EXPECT_EQ(record.frame, 1u) << "without a number from the host, frames count from 1";
    EXPECT_FLOAT_EQ(record.tickMs, 9.0f);
    EXPECT_FLOAT_EQ(record.intervalMs, 0.0f) << "the first frame has no frame before it";
    EXPECT_FLOAT_EQ(record.fixedMs, 2.0f);
    EXPECT_EQ(record.fixedTicks, 2u);
    EXPECT_FLOAT_EQ(record.physicsMs, 1.25f);
    EXPECT_FLOAT_EQ(record.updateMs, 0.75f);
    EXPECT_FLOAT_EQ(record.audioMs, 0.125f);
    EXPECT_FLOAT_EQ(record.renderMs, 4.0f);
    EXPECT_FLOAT_EQ(record.commandsMs, 0.0f);
}

TEST(FrameStatsTest, TheIntervalIsFromThePreviousFramesStartAndResetsAfterAPause)
{
    FrameStats stats;
    stats.SetEnabled(true);
    Frame(stats, T0, 2);
    Frame(stats, T0 + milliseconds(16), 2);
    stats.ResetInterval();
    Frame(stats, T0 + milliseconds(500), 2);
    Frame(stats, T0 + milliseconds(520), 2);

    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 4u);
    EXPECT_FLOAT_EQ(frames[0].intervalMs, 0.0f);
    EXPECT_FLOAT_EQ(frames[1].intervalMs, 16.0f);
    EXPECT_FLOAT_EQ(frames[2].intervalMs, 0.0f) << "a pause is not a frame";
    EXPECT_FLOAT_EQ(frames[3].intervalMs, 20.0f);
}

TEST(FrameStatsTest, TheHostsFrameNumbersAreKept)
{
    FrameStats stats;
    stats.SetEnabled(true);
    stats.SetNextFrameNumber(41);
    Frame(stats, T0, 1);
    Frame(stats, T0 + milliseconds(10), 1); // no number: the one after the last
    stats.SetNextFrameNumber(100);
    Frame(stats, T0 + milliseconds(20), 1);

    EXPECT_EQ(Numbers(stats.FramesAfter(0)), (std::vector<std::uint32_t>{41, 42, 100}));
    ASSERT_TRUE(stats.NewestFrame().has_value());
    EXPECT_EQ(*stats.NewestFrame(), 100u);
}

TEST(FrameStatsTest, TheRingKeepsTheLast240Frames)
{
    FrameStats stats;
    stats.SetEnabled(true);
    for (int i = 0; i < 250; ++i)
    {
        Frame(stats, T0 + milliseconds(16 * i), 1);
    }
    EXPECT_EQ(stats.Size(), FrameStats::HistoryFrames);
    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 240u);
    EXPECT_EQ(frames.front().frame, 11u) << "the oldest ten went";
    EXPECT_EQ(frames.back().frame, 250u);
    for (std::size_t i = 1; i < frames.size(); ++i)
    {
        EXPECT_EQ(frames[i].frame, frames[i - 1].frame + 1) << "oldest first, in order";
    }
}

TEST(FrameStatsTest, SinceFrameGivesExactlyTheNewerFrames)
{
    FrameStats stats;
    stats.SetEnabled(true);
    for (int i = 0; i < 6; ++i)
    {
        Frame(stats, T0 + milliseconds(16 * i), 1);
    }
    EXPECT_EQ(Numbers(stats.FramesAfter(0)), (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(Numbers(stats.FramesAfter(4)), (std::vector<std::uint32_t>{5, 6}));
    EXPECT_TRUE(stats.FramesAfter(6).empty()) << "nothing new";

    // A client that polls with the last frame it saw gets every frame once
    std::vector<std::uint32_t> seen = Numbers(stats.FramesAfter(0));
    Frame(stats, T0 + milliseconds(200), 1);
    Frame(stats, T0 + milliseconds(216), 1);
    for (const std::uint32_t frame : Numbers(stats.FramesAfter(seen.back())))
    {
        seen.push_back(frame);
    }
    EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(FrameStatsTest, ASinceFramePastTheNewestGetsEverything)
{
    // The client saw frame 900 of an earlier play session; this one is at frame 3
    FrameStats stats;
    stats.SetEnabled(true);
    for (int i = 0; i < 3; ++i)
    {
        Frame(stats, T0 + milliseconds(16 * i), 1);
    }
    EXPECT_EQ(Numbers(stats.FramesAfter(900)), (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(FrameStatsTest, TheNewestRecordTakesWhatFollowsItUntilTheNextFrameOrAClose)
{
    FrameStats stats;
    stats.SetEnabled(true);
    stats.AddToNewestFrame(FrameSection::Commands, 1.0); // no frame yet: nowhere to go
    Frame(stats, T0, 1);
    stats.AddToNewestFrame(FrameSection::Commands, 4.0);
    stats.AddToNewestFrame(FrameSection::RenderRequest, 2.5);
    stats.AddToNewestFrame(FrameSection::Readback, 0.5);
    stats.Add(FrameSection::Update, 9.0); // no open frame: dropped

    Frame(stats, T0 + milliseconds(16), 1);
    stats.AddToNewestFrame(FrameSection::Commands, 3.0);
    stats.CloseNewestFrame();
    stats.AddToNewestFrame(FrameSection::Commands, 100.0); // a paused game's commands: no frame

    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_FLOAT_EQ(frames[0].commandsMs, 4.0f);
    EXPECT_FLOAT_EQ(frames[0].renderRequestMs, 2.5f);
    EXPECT_FLOAT_EQ(frames[0].readbackMs, 0.5f);
    EXPECT_FLOAT_EQ(frames[0].updateMs, 0.0f);
    EXPECT_FLOAT_EQ(frames[1].commandsMs, 3.0f);
}

TEST(FrameStatsTest, AFrameLeftOpenIsDroppedByTheNext)
{
    // A Tick that threw never reached EndFrame
    FrameStats stats;
    stats.SetEnabled(true);
    stats.SetNextFrameNumber(1);
    stats.BeginFrame(T0);
    stats.Add(FrameSection::Update, 50.0);
    stats.SetNextFrameNumber(2);
    Frame(stats, T0 + milliseconds(16), 3);

    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].frame, 2u);
    EXPECT_FLOAT_EQ(frames[0].updateMs, 0.0f);
}

TEST(FrameStatsTest, TurningItOnAgainStartsClean)
{
    FrameStats stats;
    stats.SetEnabled(true);
    Frame(stats, T0, 1);
    Frame(stats, T0 + milliseconds(16), 1);
    stats.SetEnabled(false);
    EXPECT_EQ(stats.Size(), 2u) << "off keeps what was recorded";
    Frame(stats, T0 + milliseconds(32), 1);
    EXPECT_EQ(stats.Size(), 2u) << "and records nothing more";

    stats.SetEnabled(true);
    EXPECT_EQ(stats.Size(), 0u);
    Frame(stats, T0 + milliseconds(1000), 1);
    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FLOAT_EQ(frames[0].intervalMs, 0.0f) << "no interval across the time it was off";

    stats.SetEnabled(true); // on while on: nothing changes
    EXPECT_EQ(stats.Size(), 1u);
}

TEST(FrameStatsTest, TheSummaryHasTheAverage95thPercentileMaxAndFrameRate)
{
    FrameStats stats;
    stats.SetEnabled(true);
    // 20 frames 20 ms apart; ticks of 1 ms but for one of 10 ms and one of 30 ms
    for (int i = 0; i < 20; ++i)
    {
        const int tick = i == 7 ? 10 : (i == 13 ? 30 : 1);
        Frame(stats, T0 + milliseconds(20 * i), tick);
    }
    const FrameSummary summary = stats.Summarize();
    EXPECT_EQ(summary.windowFrames, 20u);
    EXPECT_FLOAT_EQ(summary.tickMs.avg, (18.0f + 10.0f + 30.0f) / 20.0f);
    EXPECT_FLOAT_EQ(summary.tickMs.max, 30.0f);
    EXPECT_FLOAT_EQ(summary.tickMs.p95, 10.0f) << "the 19th of 20 sorted ticks";
    EXPECT_FLOAT_EQ(summary.fps, 50.0f) << "1000 / 20 ms; the first frame has no interval and is left out";
}

TEST(FrameStatsTest, AScopedSectionTimesOnlyWhileOn)
{
    FrameStats stats;
    {
        const ScopedFrameSection section(stats, FrameSection::Update);
    }
    EXPECT_EQ(stats.Size(), 0u);

    stats.SetEnabled(true);
    stats.BeginFrame();
    {
        const ScopedFrameSection section(stats, FrameSection::Update);
    }
    stats.EndFrame();
    const std::vector<FrameRecord> frames = stats.FramesAfter(0);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_GE(frames[0].updateMs, 0.0f);
    EXPECT_LE(frames[0].updateMs, frames[0].tickMs) << "a section is inside its frame";
}
