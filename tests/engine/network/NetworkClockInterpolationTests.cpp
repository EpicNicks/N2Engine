#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkClock.hpp"
#include "engine/network/NetworkInterpolation.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/prefabs/PrefabManager.hpp"

// Clock sync and snapshot interpolation of remote entities (netcode stage B3): the clock estimator and the interpolator
// on their own, then a NetworkServer and NetworkClients in one process over seeded virtual links (no sleeps, no wall
// clock), plus hostile peers that send what no honest client sends.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'4E33;
    constexpr std::uint64_t kSceneHash = 0x3B3B3B3Bull;
    constexpr const char *kScenePath = "res://clock.scene";
    constexpr double kRate = 60.0;
    /// The client's monotonic clock reads the server's plus this: the two clocks share nothing
    constexpr std::uint64_t kClientClockOffset = 5'000'000'000ull;

    // ---- The clock estimator on synthetic exchanges ---------------------------------------------------------------------

    constexpr std::uint64_t kC0 = 7'000'000'000ull; // the client's clock when the server's reads 0
    constexpr std::uint32_t kStartTick = 1000;

    struct Exchange
    {
        std::uint64_t send = 0;
        std::uint64_t receive = 0;
        std::uint32_t tick = 0;
        std::uint32_t tickMicros = 0;
    };

    /// A ping sent at client time `send`, one way `up` microseconds, answered at once, `down` back; the server's clock
    /// is the client's less kC0 plus `skew` (a server clock that jumps)
    Exchange MakeExchange(const std::uint64_t send, const std::uint64_t up, const std::uint64_t down,
                          const std::int64_t skew = 0)
    {
        const std::uint64_t serverMicros =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(send + up - kC0) + skew);
        const std::uint64_t index = serverMicros * 60 / 1'000'000;
        const std::uint64_t indexStart = index * 1'000'000 / 60;
        Exchange exchange;
        exchange.send = send;
        exchange.receive = send + up + down;
        exchange.tick = static_cast<std::uint32_t>(kStartTick + index);
        exchange.tickMicros = static_cast<std::uint32_t>(std::min<std::uint64_t>(serverMicros - indexStart, 16'666));
        return exchange;
    }

    /// The server's tick (fractional) when the client's clock reads `clientNow`
    double TruthTick(const std::uint64_t clientNow, const std::int64_t skew = 0)
    {
        return kStartTick + (static_cast<double>(clientNow) - static_cast<double>(kC0) + static_cast<double>(skew)) * kRate / 1e6;
    }

    ClockSample Feed(ClockSync &clock, const Exchange &exchange)
    {
        return clock.AddSample(exchange.send, exchange.receive, exchange.tick, exchange.tickMicros, 60);
    }
}

TEST(NetworkClockMessageTest, TheMessagesRoundTripAndAreExactlyTheirSize)
{
    const std::vector<std::uint8_t> ping = Encode(ClockPingMessage{77});
    ASSERT_EQ(ping.size(), kClockPingBytes);
    EXPECT_EQ(ping[0], kClockPingType);
    ASSERT_TRUE(DecodeClockPing(ping).has_value());
    EXPECT_EQ(DecodeClockPing(ping)->sequence, 77u);

    const std::vector<std::uint8_t> pong = Encode(ClockPongMessage{9, 0xFFFFFFF0u, 16'000, 60});
    ASSERT_EQ(pong.size(), kClockPongBytes);
    EXPECT_EQ(pong[0], kClockPongType);
    const auto decoded = DecodeClockPong(pong);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->sequence, 9u);
    EXPECT_EQ(decoded->tick, 0xFFFFFFF0u);
    EXPECT_EQ(decoded->tickMicros, 16'000u);
    EXPECT_EQ(decoded->tickRate, 60);

    const std::vector<std::uint8_t> beat = Encode(StreamTickMessage{123456});
    ASSERT_EQ(beat.size(), kStreamTickBytes);
    EXPECT_EQ(DecodeStreamTick(beat)->tick, 123456u);
}

TEST(NetworkClockMessageTest, MalformedMessagesAreRefusedWithAReason)
{
    EXPECT_EQ(DecodeClockPing({}).error(), DecodeError::Empty);
    std::vector<std::uint8_t> ping = Encode(ClockPingMessage{5});
    ping.push_back(0);
    EXPECT_EQ(DecodeClockPing(ping).error(), DecodeError::TrailingBytes);
    ping.resize(3);
    EXPECT_EQ(DecodeClockPing(ping).error(), DecodeError::Truncated);
    EXPECT_EQ(DecodeClockPing(Encode(ClockPingMessage{0})).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeClockPing(Encode(ClockPongMessage{1, 1, 1, 60})).error(), DecodeError::UnknownType);

    EXPECT_EQ(DecodeClockPong(Encode(ClockPongMessage{0, 1, 1, 60})).error(), DecodeError::BadValue) << "sequence 0";
    EXPECT_EQ(DecodeClockPong(Encode(ClockPongMessage{1, 1, 1, 0})).error(), DecodeError::BadValue) << "tick rate 0";
    EXPECT_EQ(DecodeClockPong(Encode(ClockPongMessage{1, 1, 1, kMaxClockTickRate + 1})).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeClockPong(Encode(ClockPongMessage{1, 1, 16'667, 60})).error(), DecodeError::BadValue)
        << "more than a tick";
    EXPECT_TRUE(DecodeClockPong(Encode(ClockPongMessage{1, 1, 16'666, 60})).has_value());
    std::vector<std::uint8_t> pong = Encode(ClockPongMessage{1, 1, 1, 60});
    pong.pop_back();
    EXPECT_EQ(DecodeClockPong(pong).error(), DecodeError::Truncated);

    std::vector<std::uint8_t> beat = Encode(StreamTickMessage{1});
    beat.push_back(7);
    EXPECT_EQ(DecodeStreamTick(beat).error(), DecodeError::TrailingBytes);
}

TEST(NetworkClockMessageTest, AStateUpdateCarriesTheServerTickAndTheInputTickAndTheWireVersionIsEight)
{
    EXPECT_EQ(kWireVersion, 8);
    EXPECT_EQ(kStateUpdateHeaderBytes, 16u);
    StateUpdateHeader header;
    header.sequence = 3;
    header.entryCount = 1;
    header.tick = 0xDEADBEEFu;
    std::vector<std::uint8_t> message = EncodeStateUpdateHeader(header);
    ASSERT_EQ(message.size(), kStateUpdateHeaderBytes);
    message.insert(message.end(), 5, 0); // one minimal entry's worth of bytes
    const auto view = DecodeStateUpdate(message);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->header.sequence, 3u);
    EXPECT_EQ(view->header.entryCount, 1);
    EXPECT_EQ(view->header.tick, 0xDEADBEEFu);
    EXPECT_FALSE(view->header.hasInputTick);
    EXPECT_EQ(view->entries.size(), 5u);
}

TEST(NetworkClockMessageTest, TheInputTickOfAStateUpdateRoundTripsAndIsStrict)
{
    StateUpdateHeader header;
    header.sequence = 9;
    header.entryCount = 1;
    header.tick = 77;
    header.hasInputTick = true;
    header.inputTick = 0xFFFFFFF0u;
    std::vector<std::uint8_t> message = EncodeStateUpdateHeader(header);
    message.insert(message.end(), 5, 0);
    const auto view = DecodeStateUpdate(message);
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->header.hasInputTick);
    EXPECT_EQ(view->header.inputTick, 0xFFFFFFF0u);
    EXPECT_EQ(view->entries.size(), 5u);

    // Flags: bit 0 only. Another bit, or a tick named without its flag, is not something an honest server writes.
    std::vector<std::uint8_t> unknownBit = message;
    unknownBit[11] = 0x03;
    EXPECT_EQ(DecodeStateUpdate(unknownBit).error(), DecodeError::BadValue);
    std::vector<std::uint8_t> tickWithoutFlag = message;
    tickWithoutFlag[11] = 0x00;
    EXPECT_EQ(DecodeStateUpdate(tickWithoutFlag).error(), DecodeError::BadValue);
    // The writer never writes a tick it was not asked to flag
    header.hasInputTick = false;
    const std::vector<std::uint8_t> cleared = EncodeStateUpdateHeader(header);
    EXPECT_EQ(cleared[11], 0);
    EXPECT_EQ(cleared[12] | cleared[13] | cleared[14] | cleared[15], 0);
}

TEST(NetworkClockSyncTest, NothingIsNaNOrZeroDividedBeforeAndAtTheFirstSample)
{
    ClockSync clock;
    EXPECT_FALSE(clock.Synced());
    EXPECT_EQ(clock.ServerTick(123), 0.0);
    EXPECT_EQ(clock.ServerTimeMicros(123), 0u);
    EXPECT_EQ(clock.RttMicros(), 0u);
    EXPECT_EQ(clock.JitterMicros(), 0u);

    // A round trip of zero is a valid sample (a loopback)
    EXPECT_EQ(clock.AddSample(kC0, kC0, 5, 0, 60), ClockSample::Accepted);
    EXPECT_TRUE(clock.Synced());
    EXPECT_TRUE(std::isfinite(clock.ServerTick(kC0)));
    EXPECT_NEAR(clock.ServerTick(kC0), 5.0, 1e-9);
    EXPECT_NEAR(clock.ServerTick(kC0 + 1'000'000), 65.0, 1e-6);
    EXPECT_EQ(clock.RttMicros(), 0u);
    EXPECT_EQ(clock.JitterMicros(), 0u);

    // The first estimate from one ping: the pong's tick plus half the round trip
    ClockSync second;
    const Exchange exchange = MakeExchange(kC0 + 1'000'000, 20'000, 20'000);
    ASSERT_EQ(Feed(second, exchange), ClockSample::Accepted);
    EXPECT_NEAR(second.ServerTick(exchange.receive), TruthTick(exchange.receive), 0.1);
    EXPECT_EQ(second.RttMicros(), 40'000u);
}

TEST(NetworkClockSyncTest, BadSamplesAreRefusedAndChangeNothing)
{
    ClockSync clock;
    EXPECT_EQ(clock.AddSample(2000, 1000, 1, 0, 60), ClockSample::BadTimes) << "the pong before the ping";
    EXPECT_EQ(clock.AddSample(0, 6'000'000, 1, 0, 60), ClockSample::RttTooLarge);
    EXPECT_EQ(clock.AddSample(0, 1000, 1, 0, 0), ClockSample::BadTick);
    EXPECT_EQ(clock.AddSample(0, 1000, 1, 0, kMaxClockTickRate + 1), ClockSample::BadTick);
    EXPECT_EQ(clock.AddSample(0, 1000, 1, 16'667, 60), ClockSample::BadTick);
    EXPECT_FALSE(clock.Synced());
    EXPECT_EQ(clock.Stats().samplesRejected, 5u);

    ASSERT_EQ(clock.AddSample(0, 1000, 1, 0, 60), ClockSample::Accepted);
    EXPECT_EQ(clock.AddSample(2000, 3000, 1, 0, 120), ClockSample::RateChanged) << "the tick rate is the first sample's";
    EXPECT_EQ(clock.SampleCount(), 1u);
}

TEST(NetworkClockSyncTest, ConvergesUnderJitterAndSpikes)
{
    N2Net::SimRandom random(7);
    ClockSync clock;
    constexpr std::uint64_t kLatency = 30'000;
    std::uint64_t now = kC0 + 500'000;
    std::uint64_t queryAt = 0;
    double worst = 0.0;
    double sum = 0.0;
    int queries = 0;
    for (int i = 0; i < 300; ++i)
    {
        // Each way: 30 ms plus up to 20 ms of jitter; one ping in ten has a 300 ms spike one way
        std::uint64_t up = kLatency + random.NextBelow(20'001);
        std::uint64_t down = kLatency + random.NextBelow(20'001);
        if (random.NextBelow(10) == 0)
        {
            (random.NextBelow(2) == 0 ? up : down) += 300'000;
        }
        const Exchange exchange = MakeExchange(now, up, down);
        ASSERT_EQ(Feed(clock, exchange), ClockSample::Accepted);
        // The client's clock is queried forwards only: exchanges can finish out of order
        queryAt = std::max(queryAt, exchange.receive);
        if (i >= 30)
        {
            const double error = std::fabs(clock.ServerTick(queryAt) - TruthTick(queryAt));
            worst = std::max(worst, error);
            sum += error;
            ++queries;
        }
        now += 100'000;
    }
    // Within a tick on average and two at worst (the jitter alone is up to 1.2 ticks of round trip)
    EXPECT_LT(sum / queries, 1.0);
    EXPECT_LT(worst, 2.0);
    EXPECT_GT(clock.RttMicros(), 60'000u);
    EXPECT_LT(clock.RttMicros(), 160'000u);
    EXPECT_GT(clock.JitterMicros(), 0u);
}

TEST(NetworkClockSyncTest, ServerTimeNeverRunsBackwardsEvenWhenTheServersClockDoes)
{
    ClockSync clock;
    std::uint64_t now = kC0 + 1'000'000;
    double previous = 0.0;
    std::int64_t skew = 0;
    for (int i = 0; i < 400; ++i)
    {
        if (i == 100)
        {
            skew = 2'000'000; // the server's clock jumps forward two seconds
        }
        if (i == 200)
        {
            skew = -1'000'000; // and back, past where it was
        }
        const Exchange exchange = MakeExchange(now, 20'000, 20'000, skew);
        ASSERT_EQ(Feed(clock, exchange), ClockSample::Accepted);
        // Ask often between the exchanges
        for (int q = 0; q < 10; ++q)
        {
            const double tick = clock.ServerTick(now + static_cast<std::uint64_t>(q) * 10'000);
            EXPECT_GE(tick, previous) << "exchange " << i << " query " << q;
            previous = tick;
        }
        now += 100'000;
    }
    EXPECT_GT(clock.Stats().snaps, 0u);
    // A client clock that steps back is tolerated: the answer holds
    const double held = clock.ServerTick(now - 5'000'000);
    EXPECT_GE(held, previous);
}

TEST(NetworkClockSyncTest, ADriftIsFollowedAtABoundedRate)
{
    ClockSync clock;
    std::uint64_t now = kC0 + 1'000'000;
    for (int i = 0; i < 40; ++i)
    {
        ASSERT_EQ(Feed(clock, MakeExchange(now, 20'000, 20'000)), ClockSample::Accepted);
        now += 100'000;
    }
    (void)clock.ServerTick(now);
    // The server's clock is 100 ms ahead from now on: below the snap threshold, so the estimate slews to it
    double previous = clock.ServerTick(now);
    std::uint64_t previousNow = now;
    double maxRate = 0.0;
    for (int i = 0; i < 80; ++i)
    {
        ASSERT_EQ(Feed(clock, MakeExchange(now, 20'000, 20'000, 100'000)), ClockSample::Accepted);
        for (int q = 0; q < 10; ++q)
        {
            now += 10'000;
            const double tick = clock.ServerTick(now);
            const double elapsedTicks = static_cast<double>(now - previousNow) * kRate / 1e6;
            maxRate = std::max(maxRate, (tick - previous) / elapsedTicks);
            EXPECT_GE(tick, previous);
            previous = tick;
            previousNow = now;
        }
    }
    EXPECT_LE(maxRate, 1.0 + 0.05 + 1e-6) << "at most 5 % faster than real time";
    EXPECT_EQ(clock.Stats().snaps, 0u);
    EXPECT_NEAR(previous, TruthTick(now, 100'000), 0.5) << "and it got there";
}

TEST(NetworkClockSyncTest, TheTickCounterWrappingChangesNothing)
{
    ClockSync clock;
    std::uint64_t now = kC0;
    double previous = 0.0;
    // The server's tick starts 20 ticks before the wrap of its 32-bit counter
    for (int i = 0; i < 60; ++i)
    {
        const std::uint64_t serverMicros = static_cast<std::uint64_t>(i) * 100'000 + 20'000;
        const std::uint64_t index = serverMicros * 60 / 1'000'000;
        const std::uint32_t tick = static_cast<std::uint32_t>(0xFFFFFFF0u + index);
        ASSERT_EQ(clock.AddSample(now, now + 20'000, tick, 0, 60), ClockSample::Accepted);
        const double estimate = clock.ServerTick(now + 20'000);
        EXPECT_GE(estimate, previous);
        previous = estimate;
        now += 100'000;
    }
    // 6 s at 60 Hz is 360 ticks: well across the wrap, and still increasing, not 4 billion lower
    EXPECT_GT(previous, 4294967296.0);
    EXPECT_LT(previous, 4294967296.0 + 400.0);
}

// ---- The interpolator --------------------------------------------------------------------------------------------------

namespace
{
    SnapshotSample S(const std::uint32_t tick, const float x, const int serial = 0)
    {
        SnapshotSample sample;
        sample.tick = tick;
        sample.position = Math::Vector3(x, 0.0f, 0.0f);
        sample.teleportSerial = serial;
        return sample;
    }

    SnapshotAdd FeedSample(SnapshotInterpolator &interpolator, const std::uint32_t id, const SnapshotSample &sample)
    {
        interpolator.NoteMessage(sample.tick);
        return interpolator.Add(id, sample);
    }

    InterpolatedPose Eval(SnapshotInterpolator &interpolator, const double tick, const std::uint32_t id = 1)
    {
        const std::optional<InterpolatedPose> pose = interpolator.Evaluate(id, tick, kRate);
        EXPECT_TRUE(pose.has_value());
        return pose.value_or(InterpolatedPose{});
    }

    double AngleBetween(const Math::Quaternion &a, const Math::Quaternion &b)
    {
        const double dot = static_cast<double>(a.GetW()) * b.GetW() + static_cast<double>(a.GetX()) * b.GetX() +
                           static_cast<double>(a.GetY()) * b.GetY() + static_cast<double>(a.GetZ()) * b.GetZ();
        return 2.0 * std::acos(std::min(1.0, std::fabs(dot)));
    }
}

TEST(SnapshotInterpolatorTest, PositionAndScaleAreLinearBetweenTwoSamples)
{
    SnapshotInterpolator interpolator;
    SnapshotSample a = S(100, 0.0f);
    SnapshotSample b = S(102, 2.0f);
    a.scale = Math::Vector3(1.0f, 1.0f, 1.0f);
    b.scale = Math::Vector3(3.0f, 1.0f, 1.0f);
    ASSERT_EQ(FeedSample(interpolator, 1, a), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, b), SnapshotAdd::Added);
    const InterpolatedPose quarter = Eval(interpolator, 100.5);
    EXPECT_EQ(quarter.mode, PoseMode::Interpolated);
    EXPECT_NEAR(quarter.position.x, 0.5f, 1e-5f);
    EXPECT_NEAR(quarter.scale.x, 1.5f, 1e-5f);
    const InterpolatedPose half = Eval(interpolator, 101.0);
    EXPECT_NEAR(half.position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(half.scale.x, 2.0f, 1e-5f);
}

TEST(SnapshotInterpolatorTest, RotationIsSlerpedTheShortWayAndStaysUnitLength)
{
    SnapshotInterpolator interpolator;
    const Math::Quaternion start = Math::Quaternion::Identity;
    const Math::Quaternion end(Math::Vector3(0.0f, 1.0f, 0.0f), 1.5707964f); // 90 degrees about Y
    SnapshotSample a = S(10, 0.0f);
    SnapshotSample b = S(12, 0.0f);
    a.rotation = start;
    // The same rotation written as its negative: the other hemisphere, the long way round if not handled
    b.rotation = Math::Quaternion(-end.GetW(), -end.GetX(), -end.GetY(), -end.GetZ());
    ASSERT_EQ(FeedSample(interpolator, 1, a), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, b), SnapshotAdd::Added);
    const InterpolatedPose middle = Eval(interpolator, 11.0);
    const Math::Quaternion expected(Math::Vector3(0.0f, 1.0f, 0.0f), 0.7853982f);
    EXPECT_LT(AngleBetween(middle.rotation, expected), 1e-3);
    EXPECT_NEAR(middle.rotation.LengthSquared(), 1.0f, 1e-4f);
    // The quarter is a quarter of the way, not a quarter of the long way
    const InterpolatedPose quarter = Eval(interpolator, 10.5);
    EXPECT_LT(AngleBetween(quarter.rotation, Math::Quaternion(Math::Vector3(0.0f, 1.0f, 0.0f), 0.3926991f)), 1e-3);
}

TEST(SnapshotInterpolatorTest, ASampleThatIsNotANumberOrHasNoRotationIsRefused)
{
    SnapshotInterpolator interpolator;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(FeedSample(interpolator, 1, S(1, nan)), SnapshotAdd::Invalid);
    EXPECT_EQ(FeedSample(interpolator, 1, S(2, inf)), SnapshotAdd::Invalid);
    SnapshotSample noRotation = S(3, 0.0f);
    noRotation.rotation = Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FeedSample(interpolator, 1, noRotation), SnapshotAdd::Invalid);
    SnapshotSample nanRotation = S(4, 0.0f);
    nanRotation.rotation = Math::Quaternion(nan, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(FeedSample(interpolator, 1, nanRotation), SnapshotAdd::Invalid);
    SnapshotSample nanScale = S(5, 0.0f);
    nanScale.scale = Math::Vector3(1.0f, nan, 1.0f);
    EXPECT_EQ(FeedSample(interpolator, 1, nanScale), SnapshotAdd::Invalid);
    EXPECT_EQ(interpolator.ObjectCount(), 0u);
    EXPECT_EQ(interpolator.Stats().samplesInvalid, 5u);

    // A rotation that is not normalised is accepted and normalised
    SnapshotSample big = S(6, 0.0f);
    big.rotation = Math::Quaternion(2.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(FeedSample(interpolator, 1, big), SnapshotAdd::Added);
    EXPECT_NEAR(Eval(interpolator, 6.0).rotation.LengthSquared(), 1.0f, 1e-5f);
    EXPECT_FALSE(interpolator.Evaluate(99, 6.0, kRate).has_value()) << "an object without samples";
}

TEST(SnapshotInterpolatorTest, BeforeTheFirstSampleTheFirstIsShown)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 5.0f)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(102, 7.0f)), SnapshotAdd::Added);
    const InterpolatedPose pose = Eval(interpolator, 50.0);
    EXPECT_EQ(pose.mode, PoseMode::BeforeFirst);
    EXPECT_FLOAT_EQ(pose.position.x, 5.0f);
}

TEST(SnapshotInterpolatorTest, ATeleportIsNeverInterpolatedAcross)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 0.0f, 0)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(110, 100.0f, 1)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(112, 102.0f, 1)), SnapshotAdd::Added);
    for (const double tick : {100.0, 103.0, 109.5, 109.99})
    {
        const InterpolatedPose pose = Eval(interpolator, tick);
        EXPECT_EQ(pose.mode, PoseMode::Teleport) << tick;
        EXPECT_FLOAT_EQ(pose.position.x, 0.0f) << "stays where it was until the jump, at tick " << tick;
    }
    EXPECT_FLOAT_EQ(Eval(interpolator, 110.0).position.x, 100.0f);
    // And the move after the jump is interpolated as usual
    EXPECT_NEAR(Eval(interpolator, 111.0).position.x, 101.0f, 1e-4f);
    EXPECT_GE(interpolator.Stats().teleportsHeld, 3u);
}

TEST(SnapshotInterpolatorTest, AnObjectWithNothingNewIsHeldWhileTheStreamCoversTheTime)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 0.0f)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(101, 1.0f)), SnapshotAdd::Added);
    // The server says the stream is at 110 (a StreamTick or an update of another object): the object stopped
    interpolator.NoteMessage(110);
    const InterpolatedPose pose = Eval(interpolator, 105.0);
    EXPECT_EQ(pose.mode, PoseMode::Held);
    EXPECT_FLOAT_EQ(pose.position.x, 1.0f);
    EXPECT_EQ(interpolator.Stats().extrapolations, 0u);
}

TEST(SnapshotInterpolatorTest, ALateSnapshotIsExtrapolatedByItsVelocityAndClamped)
{
    InterpolationOptions options;
    options.maxExtrapolationMicros = 250'000; // 15 ticks at 60 Hz
    SnapshotInterpolator interpolator(options);
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 0.0f)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(102, 2.0f)), SnapshotAdd::Added);
    // Nothing since tick 102: the render time is past the stream
    const InterpolatedPose soon = Eval(interpolator, 106.0);
    EXPECT_EQ(soon.mode, PoseMode::Extrapolated);
    EXPECT_NEAR(soon.position.x, 6.0f, 1e-4f) << "1 unit per tick for the 4 ticks since the newest sample";
    EXPECT_EQ(interpolator.Stats().extrapolationsClamped, 0u);

    const InterpolatedPose limit = Eval(interpolator, 117.0);
    EXPECT_NEAR(limit.position.x, 17.0f, 1e-4f);
    const InterpolatedPose far = Eval(interpolator, 1000.0);
    EXPECT_NEAR(far.position.x, 17.0f, 1e-4f) << "never further than the limit, however late";
    EXPECT_GE(interpolator.Stats().extrapolationsClamped, 1u);

    // A first sample alone has no velocity: it is held
    SnapshotInterpolator lone(options);
    ASSERT_EQ(FeedSample(lone, 1, S(100, 3.0f)), SnapshotAdd::Added);
    EXPECT_FLOAT_EQ(Eval(lone, 120.0).position.x, 3.0f);
    // And a jump has none either
    SnapshotInterpolator jumped(options);
    ASSERT_EQ(FeedSample(jumped, 1, S(100, 0.0f, 0)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(jumped, 1, S(102, 50.0f, 1)), SnapshotAdd::Added);
    EXPECT_FLOAT_EQ(Eval(jumped, 110.0).position.x, 50.0f);
}

TEST(SnapshotInterpolatorTest, OutOfOrderAndRepeatedSamplesAreHandled)
{
    SnapshotInterpolator interpolator;
    interpolator.NoteMessage(104);
    EXPECT_EQ(interpolator.Add(1, S(104, 4.0f)), SnapshotAdd::Added);
    EXPECT_EQ(interpolator.Add(1, S(100, 0.0f)), SnapshotAdd::Added) << "older, arrived later";
    EXPECT_EQ(interpolator.Add(1, S(102, 2.0f)), SnapshotAdd::Added);
    ASSERT_EQ(interpolator.SampleCount(1), 3u);
    EXPECT_NEAR(Eval(interpolator, 101.0).position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(Eval(interpolator, 103.0).position.x, 3.0f, 1e-5f);

    // The same tick again: the newer message replaces the older, the buffer does not grow
    EXPECT_EQ(interpolator.Add(1, S(104, 4.0f)), SnapshotAdd::Replaced);
    EXPECT_EQ(interpolator.Add(1, S(104, 8.0f)), SnapshotAdd::Replaced);
    EXPECT_EQ(interpolator.SampleCount(1), 2u) << "the pruned buffer, and no third copy of 104";
    EXPECT_NEAR(Eval(interpolator, 104.0).position.x, 8.0f, 1e-5f);
    EXPECT_EQ(interpolator.Stats().samplesReplaced, 2u);
}

TEST(SnapshotInterpolatorTest, ASampleFarOlderThanTheNewestIsStale)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(1000, 0.0f)), SnapshotAdd::Added);
    EXPECT_EQ(interpolator.Add(1, S(1000 - kMaxSampleAgeTicks - 1, 0.0f)), SnapshotAdd::Stale);
    EXPECT_EQ(interpolator.Add(1, S(1000 - kMaxSampleAgeTicks, 0.0f)), SnapshotAdd::Added);
    EXPECT_EQ(interpolator.Stats().samplesStale, 1u);
}

TEST(SnapshotInterpolatorTest, TheBufferIsBoundedWhateverIsSent)
{
    InterpolationOptions options;
    options.maxSamplesPerObject = 32;
    SnapshotInterpolator interpolator(options);
    for (std::uint32_t tick = 0; tick < 10'000; ++tick)
    {
        ASSERT_EQ(FeedSample(interpolator, 1, S(tick, static_cast<float>(tick))), SnapshotAdd::Added);
        ASSERT_LE(interpolator.SampleCount(1), 32u);
    }
    EXPECT_EQ(interpolator.SampleCount(1), 32u);
    EXPECT_EQ(interpolator.Stats().samplesDropped, 10'000u - 32u);
    // Out of order, in a burst, a stalled stream catching up: still bounded
    for (std::uint32_t tick = 10'040; tick > 10'000; --tick)
    {
        interpolator.NoteMessage(10'040);
        (void)interpolator.Add(1, S(tick, 1.0f));
        ASSERT_LE(interpolator.SampleCount(1), 32u);
    }
    // The cap is clamped to what the option allows
    InterpolationOptions huge;
    huge.maxSamplesPerObject = 1'000'000;
    EXPECT_EQ(SnapshotInterpolator(huge).Options().maxSamplesPerObject, kMaxInterpolationSamples);
    InterpolationOptions tiny;
    tiny.maxSamplesPerObject = 0;
    EXPECT_EQ(SnapshotInterpolator(tiny).Options().maxSamplesPerObject, 4u);
}

TEST(SnapshotInterpolatorTest, ABreakInTheServersTickCountStartsAgain)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 0.0f)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 2, S(100, 0.0f)), SnapshotAdd::Added);
    ASSERT_EQ(interpolator.ObjectCount(), 2u);
    // A tick far in the future cannot park its sample at the end of the buffer for ever
    interpolator.NoteMessage(100 + kMaxMessageTickJump + 1);
    EXPECT_EQ(interpolator.ObjectCount(), 0u);
    EXPECT_EQ(interpolator.Stats().resyncs, 1u);
    // A server whose counter restarted (far behind) does the same
    ASSERT_EQ(FeedSample(interpolator, 1, S(100 + kMaxMessageTickJump + 1, 0.0f)), SnapshotAdd::Added);
    interpolator.NoteMessage(5);
    EXPECT_EQ(interpolator.ObjectCount(), 0u);
    EXPECT_EQ(interpolator.Stats().resyncs, 2u);
    // A message a little behind the newest is just out of order
    interpolator.NoteMessage(105);
    ASSERT_EQ(interpolator.Add(1, S(105, 0.0f)), SnapshotAdd::Added);
    interpolator.NoteMessage(104);
    EXPECT_EQ(interpolator.ObjectCount(), 1u);
}

TEST(SnapshotInterpolatorTest, TheCounterWrappingChangesNothing)
{
    SnapshotInterpolator interpolator;
    const std::uint32_t ticks[] = {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u};
    const float xs[] = {-2.0f, -1.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_EQ(FeedSample(interpolator, 1, S(ticks[i], xs[i])), SnapshotAdd::Added) << i;
    }
    ASSERT_EQ(interpolator.SampleCount(1), 4u);
    // The render time in the clock's 64-bit domain, a quarter after tick 0xFFFFFFFE
    EXPECT_NEAR(Eval(interpolator, 4294967294.25).position.x, -1.75f, 1e-4f);
    // And half-way between 0 and 1, past the wrap
    EXPECT_NEAR(Eval(interpolator, 4294967296.5).position.x, 0.5f, 1e-4f);
    // A render time in the domain below the wrap reduces the same way
    EXPECT_NEAR(Eval(interpolator, 0.5).position.x, 0.5f, 1e-4f);
}

TEST(SnapshotInterpolatorTest, ARenderTimeThatIsNotANumberCannotCrashIt)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 4.0f)), SnapshotAdd::Added);
    ASSERT_EQ(FeedSample(interpolator, 1, S(101, 5.0f)), SnapshotAdd::Added);
    for (const double tick : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), 1.0e300, -1.0e300})
    {
        const std::optional<InterpolatedPose> pose = interpolator.Evaluate(1, tick, kRate);
        ASSERT_TRUE(pose.has_value());
        EXPECT_TRUE(std::isfinite(pose->position.x));
        EXPECT_FLOAT_EQ(pose->position.x, 5.0f) << "the newest state";
    }
    EXPECT_TRUE(interpolator.Evaluate(1, 100.5, 0.0).has_value()) << "a tick rate of 0 is survived";
    EXPECT_TRUE(std::isfinite(interpolator.Evaluate(1, 100.5, -3.0)->position.x));
}

TEST(SnapshotInterpolatorTest, AMoveDoesNotStartBeforeTheMessageThatHadNoSampleOfTheObject)
{
    SnapshotInterpolator interpolator;
    ASSERT_EQ(FeedSample(interpolator, 1, S(100, 0.0f)), SnapshotAdd::Added);
    // Nine messages without the object (it did not change), then it moves 10 units at once
    for (std::uint32_t tick = 101; tick < 110; ++tick)
    {
        interpolator.NoteMessage(tick);
    }
    ASSERT_EQ(FeedSample(interpolator, 1, S(110, 10.0f)), SnapshotAdd::Added);
    EXPECT_EQ(interpolator.Stats().holdsInserted, 1u);
    EXPECT_FLOAT_EQ(Eval(interpolator, 105.0).position.x, 0.0f);
    EXPECT_FLOAT_EQ(Eval(interpolator, 108.0).position.x, 0.0f);
    EXPECT_NEAR(Eval(interpolator, 109.5).position.x, 5.0f, 1e-4f) << "the move spans the last interval, not ten";
}

TEST(SnapshotInterpolatorTest, AStaticRunIsTwoSamplesWhateverItsLength)
{
    SnapshotInterpolator interpolator;
    for (std::uint32_t tick = 100; tick <= 220; ++tick)
    {
        ASSERT_EQ(FeedSample(interpolator, 1, S(tick, 3.0f)), SnapshotAdd::Added);
    }
    EXPECT_EQ(interpolator.SampleCount(1), 2u);
    EXPECT_GT(interpolator.Stats().samplesCoalesced, 100u);
    EXPECT_FLOAT_EQ(Eval(interpolator, 150.0).position.x, 3.0f);
    // A move after the run starts from its end
    ASSERT_EQ(FeedSample(interpolator, 1, S(221, 4.0f)), SnapshotAdd::Added);
    EXPECT_NEAR(Eval(interpolator, 220.5).position.x, 3.5f, 1e-4f);
}

TEST(SnapshotInterpolatorTest, MovementIsSmoothThroughJitterReorderingAndLoss)
{
    // An object at 0.1 units per tick; snapshots every 2 ticks that arrive 2 to 5 ticks late in no fixed order, one in
    // five lost; frames every tick, rendering 6 ticks behind the newest time
    N2Net::SimRandom random(99);
    SnapshotInterpolator interpolator;
    struct Arrival
    {
        double at;
        std::uint32_t tick;
    };
    std::vector<Arrival> arrivals;
    for (std::uint32_t tick = 0; tick < 600; tick += 2)
    {
        if (random.NextBelow(5) == 0)
        {
            continue;
        }
        arrivals.push_back({static_cast<double>(tick) + 2.0 + random.NextBelow(4), tick});
    }
    std::stable_sort(arrivals.begin(), arrivals.end(), [](const Arrival &a, const Arrival &b) { return a.at < b.at; });

    std::vector<double> shown;
    std::size_t next = 0;
    for (int frame = 20; frame < 590; ++frame)
    {
        const double now = static_cast<double>(frame);
        while (next < arrivals.size() && arrivals[next].at <= now)
        {
            const std::uint32_t tick = arrivals[next].tick;
            interpolator.NoteMessage(tick);
            (void)interpolator.Add(1, S(tick, 0.1f * static_cast<float>(tick)));
            ++next;
        }
        shown.push_back(Eval(interpolator, now - 6.0).position.x);
    }
    double worst = 0.0;
    for (std::size_t i = 2; i < shown.size(); ++i)
    {
        worst = std::max(worst, std::fabs(shown[i] - 2.0 * shown[i - 1] + shown[i - 2]));
    }
    EXPECT_LT(worst, 1e-3) << "a straight path shown through a jittery stream has no second difference";
    // And it is where it should be: 6 ticks behind
    EXPECT_NEAR(shown.back(), 0.1 * (589.0 - 6.0), 1e-2);
    EXPECT_GT(interpolator.Stats().samplesAdded, 100u);
}

// ---- A server and clients over the simulator ---------------------------------------------------------------------------

namespace
{
    std::unique_ptr<Scene> MakeTransformScene(const std::string &name, const std::vector<std::uint32_t> &uuids)
    {
        std::unique_ptr<Scene> scene = Scene::Create(name);
        for (const std::uint32_t n : uuids)
        {
            const GameObject::Ptr gameObject = GameObject::Create("Object" + std::to_string(n));
            gameObject->SetUUID(TestUuid(n));
            gameObject->AddComponent<NetworkObject>();
            gameObject->AddComponent<NetworkTransform>();
            scene->AddRootGameObject(gameObject);
        }
        return scene;
    }

    /// A scene and the world bound to it. The world is declared after the scene, so it is destroyed first.
    struct Side
    {
        Side(const std::vector<std::uint32_t> &uuids, const std::string &name) : scene(MakeTransformScene(name, uuids))
        {
            bound = world.BindScene(*scene);
        }

        void SetX(const std::uint32_t n, const float x) const
        {
            const GameObject::Ptr object = FindByNumber(*scene, n);
            if (!object->HasPositionable())
            {
                object->CreatePositionable();
            }
            object->GetPositionable()->SetLocalPosition(Math::Vector3(x, 0.0f, 0.0f));
        }

        [[nodiscard]] float X(const std::uint32_t n) const
        {
            const GameObject::Ptr object = FindByNumber(*scene, n);
            return object->HasPositionable() ? object->GetPositionable()->GetLocalPosition().x : 0.0f;
        }

        [[nodiscard]] NetworkTransform *Transform(const std::uint32_t n) const
        {
            const GameObject::Ptr object = FindByNumber(*scene, n);
            return object ? object->GetComponent<NetworkTransform>() : nullptr;
        }

        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        std::expected<std::uint32_t, std::string> bound{std::unexpected(std::string("not bound"))};
    };

    struct ClientRig
    {
        explicit ClientRig(const std::string &name) : side({1, 2}, name) {}

        Side side;
        N2Net::SimTransport *endpoint = nullptr;
        std::unique_ptr<NetworkClient> client;
    };

    /// A hand-made peer of the server: a Connection over a simulator endpoint that sends whatever it is told to
    class RawPeer
    {
    public:
        RawPeer(const std::uint64_t now, const N2Net::PeerId server, N2Net::SimTransport &endpoint,
                const N2Net::ConnectionConfig &config)
            : _endpoint(&endpoint), _server(server), _connection(config, now)
        {
        }

        void Pump(const std::uint64_t now)
        {
            const auto handler = [&](const N2Net::TransportEvent &event)
            {
                if (event.type != N2Net::TransportEventType::Datagram || event.peer != _server)
                {
                    return;
                }
                (void)_connection.OnPacket(now, event.data,
                                           [&](const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
                                           {
                                               if (channel == N2Net::MessageChannel::ReliableOrdered)
                                               {
                                                   received.emplace_back(message.begin(), message.end());
                                               }
                                           });
            };
            _endpoint->Poll(handler);
            if (!_connection.IsFailed())
            {
                _connection.Update(now, [&](const std::span<const std::uint8_t> packet) { _endpoint->Send(_server, packet); });
            }
        }

        void SendReliable(const std::vector<std::uint8_t> &message) { (void)_connection.SendReliable(message); }
        void SendUnreliable(const std::vector<std::uint8_t> &message) { (void)_connection.SendUnreliable(message); }

        [[nodiscard]] std::optional<ServerHelloMessage> Hello() const
        {
            for (const std::vector<std::uint8_t> &message : received)
            {
                const auto decoded = DecodeServerMessage(message);
                if (decoded.has_value())
                {
                    if (const auto *hello = std::get_if<ServerHelloMessage>(&*decoded))
                    {
                        return *hello;
                    }
                }
            }
            return std::nullopt;
        }

        std::vector<std::vector<std::uint8_t>> received;

    private:
        N2Net::SimTransport *_endpoint;
        N2Net::PeerId _server;
        N2Net::Connection _connection;
    };

    struct SessionConfig
    {
        N2Net::SimLinkConfig link;
        std::uint64_t seed = 1;
        bool interpolate = true;
        std::uint32_t delayMicros = 100'000;
        std::uint32_t startTick = kStartTick;
    };

    class Session
    {
    public:
        explicit Session(SessionConfig configuration = {})
            : config(configuration), network(configuration.seed), serverSide({1, 2}, "server")
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverSide.world, std::move(options));
            server->SetCurrentTick(config.startTick);
        }

        ClientRig &AddClient()
        {
            auto rig = std::make_unique<ClientRig>("client");
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);

            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.interpolate = config.interpolate;
            options.interpolation.delayMicros = config.delayMicros;
            rig->client = std::make_unique<NetworkClient>(endpoint, rig->side.world, std::move(options));
            clients.push_back(std::move(rig));
            return *clients.back();
        }

        RawPeer &AddRaw()
        {
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);
            raws.push_back(std::make_unique<RawPeer>(network.NowMicros(), serverEndpoint->Id(), endpoint,
                                                     MakeWorldConnectionConfig(kProtocol, endpoint)));
            return *raws.back();
        }

        /// The server's tick (whole) at virtual time `now`
        [[nodiscard]] std::uint32_t TickAt(const std::uint64_t now) const
        {
            return static_cast<std::uint32_t>(config.startTick + now * 60 / 1'000'000);
        }

        /// The server's tick, fractional, at virtual time `now`
        [[nodiscard]] double TruthTickAt(const std::uint64_t now) const
        {
            return static_cast<double>(config.startTick) + static_cast<double>(now) * kRate / 1e6;
        }

        void Step(const std::uint64_t micros = 1000)
        {
            network.Advance(micros);
            const std::uint64_t now = network.NowMicros();
            if (serverRuns)
            {
                const std::uint32_t tick = TickAt(now);
                server->SetCurrentTick(tick);
                if (!haveTick || tick != lastTick)
                {
                    haveTick = true;
                    lastTick = tick;
                    if (onTick)
                    {
                        onTick(tick);
                    }
                }
                server->Update(now);
            }
            // The clients' monotonic clock has nothing to do with the server's
            for (const auto &rig : clients)
            {
                rig->client->Update(now + kClientClockOffset);
            }
            for (const auto &raw : raws)
            {
                raw->Pump(now);
            }
            if (afterStep)
            {
                afterStep(now);
            }
        }

        bool RunUntil(const std::function<bool()> &done, const std::uint64_t limitMillis = 20'000)
        {
            for (std::uint64_t elapsed = 0; elapsed < limitMillis; ++elapsed)
            {
                if (done())
                {
                    return true;
                }
                Step();
            }
            return done();
        }

        void RunFor(const std::uint64_t millis)
        {
            for (std::uint64_t elapsed = 0; elapsed < millis; ++elapsed)
            {
                Step();
            }
        }

        void JoinRaw(RawPeer &raw)
        {
            ASSERT_TRUE(RunUntil([&] { return raw.Hello().has_value(); }));
            ClientAcceptMessage accept;
            accept.fingerprint = raw.Hello()->fingerprint;
            raw.SendReliable(Encode(accept));
            const std::size_t before = server->JoinedClientCount();
            ASSERT_TRUE(RunUntil([&] { return server->JoinedClientCount() == before + 1; }));
        }

        /// Object 1 of the server moves at 5 units a second, by whole ticks
        void MoveObjectOneAlongX()
        {
            onTick = [this](const std::uint32_t tick)
            {
                if (moving)
                {
                    serverSide.SetX(1, 5.0f * static_cast<float>(tick - config.startTick) / 60.0f);
                }
            };
        }

        SessionConfig config;
        N2Net::SimNetwork network;
        Side serverSide;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<std::unique_ptr<RawPeer>> raws;
        std::function<void(std::uint32_t)> onTick;
        std::function<void(std::uint64_t)> afterStep;
        bool serverRuns = true;
        bool moving = true;
        bool haveTick = false;
        std::uint32_t lastTick = 0;
    };

    N2Net::SimLinkConfig Impaired()
    {
        N2Net::SimLinkConfig link;
        link.latencyMicros = 40'000;
        link.jitterMicros = 10'000;
        link.lossPpm = 30'000;
        link.reorderPpm = 50'000;
        link.reorderDelayMicros = 30'000;
        return link;
    }

    struct ClockRun
    {
        std::vector<double> errors;
        double worst = 0.0;
        bool backwards = false;
        std::uint64_t pings = 0;
        std::uint64_t pongs = 0;
        std::uint64_t rtt = 0;
        std::uint64_t jitter = 0;
        std::uint64_t finalServerMicros = 0;
        std::uint64_t samplesAdded = 0;
    };

    ClockRun RunClockScenario(const std::uint64_t seed)
    {
        SessionConfig config;
        config.seed = seed;
        config.link = Impaired();
        Session session(config);
        session.MoveObjectOneAlongX();
        ClientRig &rig = session.AddClient();
        ClockRun run;
        double previous = 0.0;
        const std::uint64_t total = 12'000;
        for (std::uint64_t i = 0; i < total; ++i)
        {
            session.Step();
            if (!rig.client->ClockSynced())
            {
                continue;
            }
            const double tick = rig.client->ServerTick();
            if (tick < previous)
            {
                run.backwards = true;
            }
            previous = tick;
            if (i > 5'000)
            {
                const double error = std::fabs(tick - session.TruthTickAt(session.network.NowMicros()));
                run.errors.push_back(error);
                run.worst = std::max(run.worst, error);
            }
        }
        run.pings = rig.client->ClockStats().pingsSent;
        run.pongs = rig.client->ClockStats().pongsReceived;
        run.rtt = rig.client->RttMicros();
        run.jitter = rig.client->JitterMicros();
        run.finalServerMicros = rig.client->ServerTimeMicros();
        run.samplesAdded = rig.client->Interpolator().Stats().samplesAdded;
        return run;
    }
}

TEST(NetworkClockSessionTest, TheClientEstimatesTheServersClockWithinABoundAndNeverGoesBackwards)
{
    const ClockRun run = RunClockScenario(5);
    ASSERT_FALSE(run.errors.empty());
    double sum = 0.0;
    for (const double error : run.errors)
    {
        sum += error;
    }
    EXPECT_LT(sum / static_cast<double>(run.errors.size()), 1.0) << "mean error in ticks (a tick is 16.7 ms)";
    EXPECT_LT(run.worst, 2.5) << "worst error in ticks, over a link with 3 % loss, 5 % reordering and 10 ms jitter";
    EXPECT_FALSE(run.backwards);
    // 80 ms of latency both ways plus jitter
    EXPECT_GT(run.rtt, 70'000u);
    EXPECT_LT(run.rtt, 130'000u);
    EXPECT_GT(run.jitter, 0u);
    EXPECT_LT(run.jitter, 40'000u);
}

TEST(NetworkClockSessionTest, ThePingRateIsBounded)
{
    const ClockRun run = RunClockScenario(6);
    // 12 s: ten a second until eight samples, then two a second
    EXPECT_GE(run.pings, 15u);
    EXPECT_LE(run.pings, 40u);
    EXPECT_LE(run.pongs, run.pings);
    EXPECT_GT(run.pongs, 10u) << "most are answered over a 3 % loss link";
}

TEST(NetworkClockSessionTest, TheSameSeedGivesTheSameRunToTheLastBit)
{
    const ClockRun first = RunClockScenario(11);
    const ClockRun second = RunClockScenario(11);
    EXPECT_EQ(first.errors, second.errors);
    EXPECT_EQ(first.finalServerMicros, second.finalServerMicros);
    EXPECT_EQ(first.samplesAdded, second.samplesAdded);
    EXPECT_EQ(first.rtt, second.rtt);
    EXPECT_GT(first.samplesAdded, 50u);
}

namespace
{
    struct MotionRun
    {
        std::vector<double> x;
        double worstError = 0.0;
        std::uint64_t extrapolations = 0;
    };

    /// A client that watches object 1 move for 8 s over an impaired link; `x` holds what it showed every 16 ms from 3 s on
    MotionRun RunMotion(const bool interpolate, const std::uint64_t seed)
    {
        SessionConfig config;
        config.seed = seed;
        config.interpolate = interpolate;
        config.link = Impaired();
        config.link.jitterMicros = 15'000;
        config.link.lossPpm = 50'000;
        Session session(config);
        session.MoveObjectOneAlongX();
        ClientRig &rig = session.AddClient();
        MotionRun run;
        for (std::uint64_t i = 0; i < 8'000; ++i)
        {
            session.Step();
            if (i >= 3'000 && i % 16 == 0 && rig.client->ClockSynced())
            {
                run.x.push_back(rig.side.X(1));
                if (interpolate)
                {
                    // Where the object was on the server 100 ms of server time ago
                    const double renderTick = session.TruthTickAt(session.network.NowMicros()) - 6.0;
                    const double expected = 5.0 * (renderTick - static_cast<double>(config.startTick)) / 60.0;
                    run.worstError = std::max(run.worstError, std::fabs(static_cast<double>(rig.side.X(1)) - expected));
                }
            }
        }
        run.extrapolations = rig.client->Interpolator().Stats().extrapolations;
        return run;
    }

    double WorstSecondDifference(const std::vector<double> &x)
    {
        double worst = 0.0;
        for (std::size_t i = 2; i < x.size(); ++i)
        {
            worst = std::max(worst, std::fabs(x[i] - 2.0 * x[i - 1] + x[i - 2]));
        }
        return worst;
    }
}

TEST(NetworkInterpolationSessionTest, ARemoteObjectMovesSmoothlyThroughJitterAndLoss)
{
    const MotionRun smooth = RunMotion(true, 21);
    const MotionRun raw = RunMotion(false, 21);
    ASSERT_GT(smooth.x.size(), 200u);
    // 5 units a second is 8.3 cm a frame: the straight path through the jitter has a second difference of a few cm at most
    // (the 1 cm position step and the clock estimate's slew), where the state as it arrives jumps by a snapshot at a time
    EXPECT_LT(WorstSecondDifference(smooth.x), 0.05);
    EXPECT_GT(WorstSecondDifference(raw.x), 0.1);
    EXPECT_LT(smooth.worstError, 0.4) << "and it is 100 ms behind the server, to within the clock estimate";
}

TEST(NetworkInterpolationSessionTest, AStoppedObjectStaysWhereItStoppedWithNoExtrapolation)
{
    SessionConfig config;
    config.link.latencyMicros = 30'000;
    Session session(config);
    session.MoveObjectOneAlongX();
    ClientRig &rig = session.AddClient();
    session.RunFor(3'000);
    session.moving = false;
    const float stoppedAt = session.serverSide.X(1);
    session.RunFor(1'000);
    const std::uint64_t extrapolationsBefore = rig.client->Interpolator().Stats().extrapolations;
    float low = 1.0e9f;
    float high = -1.0e9f;
    for (int i = 0; i < 4'000; ++i)
    {
        session.Step();
        low = std::min(low, rig.side.X(1));
        high = std::max(high, rig.side.X(1));
    }
    EXPECT_NEAR(low, stoppedAt, 0.011f);
    EXPECT_NEAR(high, stoppedAt, 0.011f);
    EXPECT_EQ(rig.client->Interpolator().Stats().extrapolations, extrapolationsBefore)
        << "the stream tick says nothing is missing";
}

TEST(NetworkInterpolationSessionTest, ATeleportIsNotInterpolatedAcrossOverTheNetwork)
{
    SessionConfig config;
    config.link.latencyMicros = 20'000;
    config.link.jitterMicros = 5'000;
    Session session(config);
    ClientRig &rig = session.AddClient();
    std::uint32_t teleportTick = 0;
    session.onTick = [&](const std::uint32_t tick)
    {
        const std::uint32_t since = tick - session.config.startTick;
        if (teleportTick == 0 && since == 300)
        {
            teleportTick = tick;
            session.serverSide.Transform(1)->Teleport(Math::Vector3(100.0f, 0.0f, 0.0f));
        }
        else if (teleportTick == 0)
        {
            session.serverSide.SetX(1, 5.0f * static_cast<float>(since) / 60.0f);
        }
        else
        {
            session.serverSide.SetX(1, 100.0f + 5.0f * static_cast<float>(tick - teleportTick) / 60.0f);
        }
    };
    float beforeJump = 0.0f;
    bool jumped = false;
    std::vector<float> between;
    for (int i = 0; i < 9'000; ++i)
    {
        session.Step();
        const float x = rig.side.X(1);
        if (!jumped && x < 90.0f)
        {
            beforeJump = std::max(beforeJump, x);
        }
        else if (x >= 90.0f)
        {
            jumped = true;
        }
        if (beforeJump > 0.0f && x > beforeJump + 0.2f && x < 99.8f)
        {
            between.push_back(x);
        }
    }
    ASSERT_NE(teleportTick, 0u);
    EXPECT_TRUE(jumped);
    EXPECT_GT(beforeJump, 20.0f) << "it was moving before the jump (the teleport was at 25 units)";
    EXPECT_TRUE(between.empty()) << "saw " << between.size() << " positions between the old place and the new one, first "
                                 << (between.empty() ? 0.0f : between.front());
    EXPECT_GT(rig.client->Interpolator().Stats().teleportsHeld, 0u);
    EXPECT_GT(rig.side.X(1), 100.0f);
}

TEST(NetworkInterpolationSessionTest, ASnapshotThatIsLateIsExtrapolatedForAtMostTheLimit)
{
    SessionConfig config;
    config.link.latencyMicros = 20'000;
    Session session(config);
    session.MoveObjectOneAlongX();
    ClientRig &rig = session.AddClient();
    session.RunFor(4'000);

    // The server falls silent for a second (the client's connection survives that: its idle timeout is ten). What was
    // already on the wire still arrives; the newest state is what the client holds after that.
    session.serverRuns = false;
    session.RunFor(100);
    const float serverX = rig.side.Transform(1)->GetReplicatedPosition().x;
    float highest = 0.0f;
    for (int i = 0; i < 900; ++i)
    {
        session.Step();
        highest = std::max(highest, rig.side.X(1));
    }
    // The newest state is serverX; the clock runs on, so the object reaches it and then goes on at its last speed for 250 ms
    // (1.25 units at 5 a second, give or take the 1 cm step of the two states the speed comes from) and no further
    EXPECT_GE(highest, serverX) << "it reached the newest state";
    EXPECT_LE(highest, serverX + 1.25f + 0.2f);
    EXPECT_GE(rig.side.X(1), serverX + 1.0f) << "and is held at the limit";
    EXPECT_GT(rig.client->Interpolator().Stats().extrapolationsClamped, 0u);
    EXPECT_EQ(rig.client->State(), NetworkClientState::Joined);

    // The server comes back: the object is where the server says it was 100 ms ago, not stuck at the limit
    session.serverRuns = true;
    session.RunFor(1'500);
    const double renderTick = session.TruthTickAt(session.network.NowMicros()) - 6.0;
    const double expected = 5.0 * (renderTick - static_cast<double>(session.config.startTick)) / 60.0;
    EXPECT_NEAR(static_cast<double>(rig.side.X(1)), expected, 0.5);
}

TEST(NetworkInterpolationSessionTest, ARemoteObjectIsDelayedAndTheDelayIsConfigurable)
{
    // With a longer delay the shown position lags further behind the server's
    double lagShort = 0.0;
    double lagLong = 0.0;
    for (const std::uint32_t delay : {50'000u, 250'000u})
    {
        SessionConfig config;
        config.delayMicros = delay;
        config.link.latencyMicros = 20'000;
        Session session(config);
        session.MoveObjectOneAlongX();
        ClientRig &rig = session.AddClient();
        session.RunFor(4'000);
        const double lag = static_cast<double>(session.serverSide.X(1) - rig.side.X(1));
        (delay == 50'000u ? lagShort : lagLong) = lag;
    }
    // 5 units a second: 50 ms is 0.25, 250 ms is 1.25 (plus the send period and the latency, which both share)
    EXPECT_GT(lagLong, lagShort + 0.8);
    EXPECT_GT(lagShort, 0.05);
}

TEST(NetworkInterpolationSessionTest, ThePositionIsTheServersTheMomentItIsOnlyTheRawStateWithoutInterpolation)
{
    SessionConfig config;
    config.interpolate = false;
    config.link.latencyMicros = 20'000;
    Session session(config);
    session.MoveObjectOneAlongX();
    ClientRig &rig = session.AddClient();
    session.RunFor(4'000);
    EXPECT_EQ(rig.client->Interpolator().ObjectCount(), 0u);
    EXPECT_EQ(rig.client->Interpolator().Stats().samplesAdded, 0u);
    // The state as it arrived: at most a send period and the latency behind the server
    EXPECT_NEAR(rig.side.X(1), session.serverSide.X(1), 0.5f);
    EXPECT_TRUE(rig.client->ClockSynced()) << "the clock is estimated all the same";
}

TEST(NetworkInterpolationSessionTest, AnObjectTheClientOwnsIsNeverInterpolated)
{
    PrefabManager::UnregisterPrefab("ClockTest_Pawn");
    const GameObject::Ptr root = GameObject::Create("Prefab_ClockTest_Pawn");
    root->AddComponent<NetworkObject>();
    root->AddComponent<NetworkTransform>();
    ASSERT_TRUE(PrefabManager::RegisterPrefab("ClockTest_Pawn", root));

    {
        SessionConfig config;
        config.link.latencyMicros = 30'000;
        config.delayMicros = 250'000;
        Session session(config);
        ClientRig &rig = session.AddClient();
        ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
        const ClientId clientId = rig.client->LocalClientId();
        const auto pawn = session.server->Spawn("ClockTest_Pawn", clientId);
        const auto remote = session.server->Spawn("ClockTest_Pawn", kServerClientId);
        ASSERT_TRUE(pawn.has_value());
        ASSERT_TRUE(remote.has_value());
        session.onTick = [&](const std::uint32_t tick)
        {
            const float x = 5.0f * static_cast<float>(tick - session.config.startTick) / 60.0f;
            for (const NetworkId id : {*pawn, *remote})
            {
                if (NetworkObject *const object = session.serverSide.world.Find(id))
                {
                    GameObject &gameObject = object->GetGameObject();
                    if (!gameObject.HasPositionable())
                    {
                        gameObject.CreatePositionable();
                    }
                    gameObject.GetPositionable()->SetLocalPosition(Math::Vector3(x, 0.0f, 0.0f));
                }
            }
        };
        session.RunFor(3'000);
        EXPECT_FALSE(rig.client->Interpolator().Has(pawn->value)) << "its own pawn";
        EXPECT_TRUE(rig.client->Interpolator().Has(remote->value)) << "somebody else's";

        // The client's copy of its own pawn shows the newest state as it arrived; the remote one lags 250 ms behind the
        // server (a second of motion is 5 units: the gap is a unit, less the age of the newest state)
        const NetworkObject *const ownCopy = rig.side.world.Find(*pawn);
        const NetworkObject *const remoteCopy = rig.side.world.Find(*remote);
        ASSERT_NE(ownCopy, nullptr);
        ASSERT_NE(remoteCopy, nullptr);
        const float own = ownCopy->GetGameObject().GetPositionable()->GetLocalPosition().x;
        const float other = remoteCopy->GetGameObject().GetPositionable()->GetLocalPosition().x;
        EXPECT_GT(own, other + 0.5f);
    }
    PrefabManager::UnregisterPrefab("ClockTest_Pawn");
}

TEST(NetworkInterpolationSessionTest, TheFilterHookLeavesAnObjectToTheGame)
{
    SessionConfig config;
    config.link.latencyMicros = 20'000;
    Session session(config);
    session.MoveObjectOneAlongX();
    ClientRig &rig = session.AddClient();
    session.RunFor(2'000);

    // Interpolated: the shown position differs from the newest state while the object moves
    int differing = 0;
    for (int i = 0; i < 500; ++i)
    {
        session.Step();
        if (std::fabs(rig.side.X(1) - rig.side.Transform(1)->GetReplicatedPosition().x) > 0.05f)
        {
            ++differing;
        }
    }
    EXPECT_GT(differing, 400);

    // The hook says no to everything: the transform is the state as it arrived, every step
    rig.client->SetInterpolationFilter([](const NetworkObject &) { return false; });
    session.RunFor(100); // the next state updates overwrite what was interpolated
    int off = 0;
    for (int i = 0; i < 500; ++i)
    {
        session.Step();
        if (std::fabs(rig.side.X(1) - rig.side.Transform(1)->GetReplicatedPosition().x) > 0.011f)
        {
            ++off;
        }
    }
    EXPECT_EQ(off, 0);
}

TEST(NetworkClockSessionTest, ARemovedObjectLeavesNoSamplesBehind)
{
    SessionConfig config;
    config.link.latencyMicros = 20'000;
    Session session(config);
    session.MoveObjectOneAlongX();
    ClientRig &rig = session.AddClient();
    session.RunFor(2'000);
    EXPECT_TRUE(rig.client->Interpolator().Has(1));
    // The server destroys the scene object: the client forgets its samples with the despawn
    const GameObject::Ptr object = FindByNumber(*session.serverSide.scene, 1);
    session.moving = false;
    object->Destroy();
    session.serverSide.scene->ProcessDestroyed();
    session.RunFor(1'000);
    EXPECT_FALSE(rig.client->Interpolator().Has(1));
}

// ---- Hostile peers -----------------------------------------------------------------------------------------------------

TEST(NetworkClockSessionTest, AClientThatPingsFasterThanAllowedIsDropped)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    session.JoinRaw(raw);
    // One ping a tick is far more than any client asks for
    for (std::uint32_t sequence = 1; sequence <= 200; ++sequence)
    {
        raw.SendUnreliable(Encode(ClockPingMessage{sequence}));
        session.Step();
    }
    EXPECT_GE(session.server->Stats().droppedRateLimited, 1u);
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);
}

TEST(NetworkClockSessionTest, AHonestRateOfPingsIsNotLimited)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    session.JoinRaw(raw);
    for (std::uint32_t sequence = 1; sequence <= 50; ++sequence)
    {
        raw.SendUnreliable(Encode(ClockPingMessage{sequence}));
        session.RunFor(100);
    }
    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    EXPECT_EQ(session.server->Stats().droppedRateLimited, 0u);
    EXPECT_GT(session.server->Stats().clockPongsSent, 40u);
}

TEST(NetworkClockSessionTest, AMalformedPingOrOneBeforeJoiningDropsTheClient)
{
    {
        Session session;
        RawPeer &raw = session.AddRaw();
        session.JoinRaw(raw);
        std::vector<std::uint8_t> ping = Encode(ClockPingMessage{3});
        ping.push_back(0);
        raw.SendUnreliable(ping);
        session.RunFor(200);
        EXPECT_EQ(session.server->Stats().droppedMalformed, 1u);
        EXPECT_EQ(session.server->JoinedClientCount(), 0u);
    }
    {
        Session session;
        RawPeer &raw = session.AddRaw();
        session.JoinRaw(raw);
        raw.SendUnreliable(Encode(ClockPingMessage{0}));
        session.RunFor(200);
        EXPECT_EQ(session.server->Stats().droppedMalformed, 1u) << "sequence 0";
    }
    {
        Session session;
        RawPeer &raw = session.AddRaw();
        // Connected, but has not accepted the world
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        raw.SendUnreliable(Encode(ClockPingMessage{1}));
        session.RunFor(200);
        EXPECT_EQ(session.server->Stats().droppedProtocol, 1u);
    }
    {
        // A client sends pings, never pongs or stream ticks: those are the server's
        Session session;
        RawPeer &raw = session.AddRaw();
        session.JoinRaw(raw);
        raw.SendUnreliable(Encode(ClockPongMessage{1, 1, 0, 60}));
        session.RunFor(200);
        EXPECT_EQ(session.server->Stats().droppedProtocol, 1u);
    }
}

TEST(NetworkClockSessionTest, TheServerAnswersWithItsTickAndTickRate)
{
    SessionConfig config;
    config.startTick = 0xFFFFFFF0u; // the counter wraps during the run
    config.link.latencyMicros = 20'000;
    Session session(config);
    ClientRig &rig = session.AddClient();
    session.RunFor(5'000);
    ASSERT_TRUE(rig.client->ClockSynced());
    EXPECT_EQ(rig.client->ServerTickRate(), 60u);
    // 5 s at 60 Hz is 300 ticks past the wrap: the estimate carries on in 64 bits
    const double tick = rig.client->ServerTick();
    EXPECT_GT(tick, 4294967296.0);
    EXPECT_NEAR(tick, session.TruthTickAt(session.network.NowMicros()) - 0.0, 3.0);
}
