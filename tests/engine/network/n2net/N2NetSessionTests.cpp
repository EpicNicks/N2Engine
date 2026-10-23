#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"

// Whole sessions over the simulator (net-a3): two peers exchanging 10,000 messages each way over a link that loses,
// reorders and duplicates, and a golden session trace (every delivery and the final statistics hashed to a constant that
// must be the same on every CI leg).

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    SimLinkConfig InternetLikeLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 30'000;
        link.jitterMicros = 10'000;
        link.lossPpm = 100'000;
        link.burstEnterPpm = 5'000;
        link.burstExitPpm = 300'000;
        link.burstLossPpm = 800'000;
        link.reorderPpm = 50'000;
        link.reorderDelayMicros = 30'000;
        link.duplicatePpm = 20'000;
        return link;
    }

    std::size_t SessionSize(std::uint32_t index)
    {
        return 1 + (index * 2654435761u >> 7) % 1500u;
    }

    std::vector<std::uint8_t> Unreliable(std::uint32_t index)
    {
        return MakeMessage(index, 8 + index % 40);
    }

    std::uint64_t HashReceived(std::uint64_t hash, const std::vector<ReceivedMessage> &received)
    {
        for (const ReceivedMessage &message : received)
        {
            hash = HashU64(hash, message.at);
            hash = HashU64(hash, static_cast<std::uint64_t>(message.channel));
            hash = HashBytes(hash, message.data);
        }
        return hash;
    }

    std::uint64_t HashStats(std::uint64_t hash, const Connection &connection)
    {
        const ConnectionStats &stats = connection.Stats();
        for (std::uint64_t value : {stats.packetsSent, stats.bytesSent, stats.packetsReceived, stats.packetsRejected,
                                    stats.packetsAcked, stats.packetsLost, stats.unitsSent, stats.unitsResent,
                                    stats.reliableMessagesDelivered, stats.unreliableMessagesDelivered,
                                    stats.unreliableStale, stats.unitsDuplicate, connection.Rtt().SmoothedMicros(),
                                    connection.Rtt().VarianceMicros(), connection.Rtt().RtoMicros()})
        {
            hash = HashU64(hash, value);
        }
        return hash;
    }
} // namespace

TEST(N2NetSessionTest, TenThousandMessagesEachWayOverALossyLink)
{
    constexpr std::uint32_t kCount = 10'000;
    TwoPeers peers(20261020, InternetLikeLink());
    Connection &a = *peers.peerA.connection;
    Connection &b = *peers.peerB.connection;
    for (std::uint32_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(a.SendReliable(MakeMessage(i, SessionSize(i))), SendResult::Ok);
        ASSERT_EQ(b.SendReliable(MakeMessage(i + 1'000'000, SessionSize(i + 7))), SendResult::Ok);
    }

    const std::size_t bound = a.MemoryBoundBytes();
    std::size_t peak = 0;
    std::uint32_t unreliableSent = 0;
    const bool done = peers.RunUntil(
        [&]
        {
            // an unreliable message per step alongside, as a game's snapshots would be
            if (unreliableSent < 3000)
            {
                a.SendUnreliable(Unreliable(unreliableSent));
                b.SendUnreliable(Unreliable(unreliableSent + 500'000));
                ++unreliableSent;
            }
            peak = std::max({peak, a.MemoryInUseBytes(), b.MemoryInUseBytes()});
            return peers.peerA.connection->Stats().reliableMessagesDelivered >= kCount && peers.peerB.connection->Stats().reliableMessagesDelivered >= kCount;
        },
        900'000'000);
    ASSERT_TRUE(done) << "delivered " << peers.peerB.connection->Stats().reliableMessagesDelivered << " / " << peers.peerA.connection->Stats().reliableMessagesDelivered;
    EXPECT_FALSE(a.IsFailed());
    EXPECT_FALSE(b.IsFailed());
    EXPECT_LE(peak, bound);

    // exactly once, in order, byte for byte
    std::uint32_t atB = 0;
    std::uint32_t atA = 0;
    for (const ReceivedMessage &message : peers.peerB.received)
    {
        if (message.channel == MessageChannel::ReliableOrdered)
        {
            ASSERT_EQ(message.data, MakeMessage(atB, SessionSize(atB))) << "at B, message " << atB;
            ++atB;
        }
    }
    for (const ReceivedMessage &message : peers.peerA.received)
    {
        if (message.channel == MessageChannel::ReliableOrdered)
        {
            ASSERT_EQ(message.data, MakeMessage(atA + 1'000'000, SessionSize(atA + 7))) << "at A, message " << atA;
            ++atA;
        }
    }
    EXPECT_EQ(atB, kCount);
    EXPECT_EQ(atA, kCount);

    // the unreliable stream only moved forward
    std::uint32_t lastUnreliable = 0;
    bool first = true;
    for (const ReceivedMessage &message : peers.peerB.received)
    {
        if (message.channel == MessageChannel::UnreliableSequenced)
        {
            const std::uint32_t index = MessageIndex(message.data);
            ASSERT_TRUE(first || index > lastUnreliable);
            lastUnreliable = index;
            first = false;
        }
    }

    // nothing arrives late, and once everything is acknowledged the senders go quiet
    peers.RunFor(10'000'000);
    EXPECT_EQ(peers.peerB.connection->Stats().reliableMessagesDelivered, kCount);
    EXPECT_EQ(peers.peerA.connection->Stats().reliableMessagesDelivered, kCount);
    EXPECT_TRUE(a.ReliableIdle());
    EXPECT_TRUE(b.ReliableIdle());
    EXPECT_GT(a.Stats().packetsLost, 100u);
    EXPECT_GT(a.LossRatePpm(), 30'000u);
    EXPECT_LT(a.LossRatePpm(), 400'000u);
    EXPECT_GE(a.Rtt().SmoothedMicros(), 60'000u);
}

TEST(N2NetSessionTest, GoldenSessionTraceIsTheSameOnEveryPlatform)
{
    // A bidirectional session over a seeded lossy link: every message delivered (when, on which channel, which bytes) and
    // the final statistics of both ends, hashed. The same constant must come out on every CI leg.
    TwoPeers peers(424242, InternetLikeLink());
    for (std::uint32_t i = 0; i < 400; ++i)
    {
        peers.peerA.connection->SendReliable(MakeMessage(i, SessionSize(i)));
        peers.peerB.connection->SendReliable(MakeMessage(i + 7000, SessionSize(i + 3)));
    }
    for (std::uint32_t step = 0; step < 4000; ++step)
    {
        if (step % 4 == 0 && step < 800)
        {
            peers.peerA.connection->SendUnreliable(Unreliable(step));
            peers.peerB.connection->SendUnreliable(Unreliable(step + 90000));
        }
        peers.Step(1000);
    }
    EXPECT_EQ(peers.peerA.connection->Stats().reliableMessagesDelivered, 400u);
    EXPECT_EQ(peers.peerB.connection->Stats().reliableMessagesDelivered, 400u);

    std::uint64_t hash = kFnvOffset;
    hash = HashReceived(hash, peers.peerA.received);
    hash = HashReceived(hash, peers.peerB.received);
    hash = HashStats(hash, *peers.peerA.connection);
    hash = HashStats(hash, *peers.peerB.connection);
    EXPECT_EQ(hash, 0xA29A834D38E9F126ULL);
}
